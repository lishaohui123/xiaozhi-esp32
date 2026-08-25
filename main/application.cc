#include "application.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "gloable_var.h"
#include "alarm_manager.h"
#include "mcp_server_alarm.h"
#include "iot/iot.h"
#include "constants.h"

#include <cstring>
#include <map>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"



// URL编码函数
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // 空格编码为'+'或'%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}



Application::Application() {
    voice_call_ = VoiceCall::get_instance();

    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    touch_debounce_queue_ = xQueueCreate(50, sizeof(TouchRegion));
    touch_task_queue_.clear();

    // 触摸传感器的震动定时器
    esp_timer_create_args_t touch_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            
            SW_Vibrating(0);
        },
        .arg = this,
    };
    esp_timer_create(&touch_timer_args, &touch_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (touch_timer_handle_ != nullptr) {
        esp_timer_stop(touch_timer_handle_);
        esp_timer_delete(touch_timer_handle_);
    }
    if (touch_debounce_queue_) {
        vQueueDelete(touch_debounce_queue_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    IOT* iot = board.GetIOT();
    iot->SPK_EN_init();
    SPK_EN(1);

    // Setup the display
    auto display = board.GetDisplay();

    // Check for new assets version
    // 因为要上电之后显示默认gif，提前调用了这个函数
    CheckAssetsVersion();

    // Print board name/version info
    // display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };

    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        // auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                // display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    // display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    // display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                // display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                // display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                // display->SetStatus(Lang::Strings::DETECTING_MODULE);
                // display->SetChatMessage("system", Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorTimeout:
                // display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    // display->UpdateStatusBar(true);
}

void Application::Run() {
    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }
#if 0
        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }
#endif
        
        if (bits & MAIN_EVENT_SEND_AUDIO) {
            if (voice_call_->udp_send_task_exit) {
                while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                    if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                        break;
                    }
                }
            }
        }

#if 0
        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }
#endif
        
        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            if (voice_call_->udp_send_task_exit) {
                HandleWakeWordDetectedEvent();
            }
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            // auto display = Board::GetInstance().GetDisplay();
            // display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    // auto display = Board::GetInstance().GetDisplay();
    // display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    // auto display = Board::GetInstance().GetDisplay();
    // display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    // auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    // display->ShowNotification(message.c_str());
    // display->SetChatMessage("system", "");

    // Play the success sound to indicate the device is ready
    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

    // Release OTA object after activation is complete
    // ota_.reset();        // 后续的固件升级需要用到这个句柄，这里不能释放
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    InitializeGloableVar();
#if 0
    // Check for new assets version
    CheckAssetsVersion();
#endif
    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    SW_Vibrating_init();

    InitializeDebounceTouchThread();
    InitializeTouchThread();

    TOUCH_1_init();
    TOUCH_2_init();
    TOUCH_3_init();
    TOUCH_4_init();

    InitializeMqtt();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        // display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                // display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    // display->SetChatMessage("system", "");
    display->SetEmotion("neutral");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        // auto display = board.GetDisplay();
        // display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay
#if 0 // 去掉主动更新固件
        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }
#endif
        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        // display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            // 自动进行设备绑定
            ota_->SendActivationCode(SystemInfo::GetMacAddress(), blue_device, board_id);
            // ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());

            if (blue_device == "")  {
                std::string s = "小主人，请先长按开关键四到五秒来开机，然后先双击开关键，进行蓝牙配网";
                auto& alarm_manager = AlarmManager::GetInstance();
                alarm_manager.PlayTtsAudioStreamVoice(s, 3);

                ESP_LOGE(TAG, "请重新进行蓝牙配网");
            }
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

/************************
 * 自动进行固件的升级
 ************************/
void Application::UpdateFirmwareTask() {
    if (update_firmware_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "UpdateFirmwareTask task already running");
        return;
    }

    xTaskCreate([](void* arg) {
        Application* app = static_cast<Application*>(arg);
        app->UpdateFirmware(arg);
        app->update_firmware_task_handle_ = nullptr;

        vTaskDelete(NULL);
    }, "UpdateFirmwareTask", 1024 * 48, this, 2, &update_firmware_task_handle_);
    assert(update_firmware_task_handle_ != nullptr);
}

void Application::UpdateFirmware(void *arg) {
    // 正常对话时、电话时、播放故事时，都不触发的
    if ((kDeviceStateIdle == GetDeviceState()) && (Application::GetInstance().GetAudioService().is_voice_out_ == false)) {
        Application* application = static_cast<Application*>(arg);

        application->ota_->CheckVersion();

        if (application->ota_->HasNewVersion()) {
            ESP_LOGI(TAG, "正在进行升级。%s %s", application->ota_->GetFirmwareUrl().c_str(),application->ota_->GetFirmwareVersion().c_str());
    #if 1
            std::string s = "小主人，正在进行升级，请保持设备处于开机状态。";
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);
    #endif
            if (UpgradeFirmware(application->ota_->GetFirmwareUrl(), application->ota_->GetFirmwareVersion())) {
    #if 1
                std::string s = "小主人，已经升级完成了，请长按开关键重新起动设备。";
                auto& alarm_manager = AlarmManager::GetInstance();
                alarm_manager.PlayTtsAudioStreamVoice(s, 1);
    #endif
                return; // This line will never be reached after reboot
            }
        }
        else {
            std::string s = "小主人，目前设备已经是最新版本了。";
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);

            ESP_LOGI(TAG, "目前固件已经是最新版本了");
        }
    }
}

/***********************************************
* 增加的在设备启动时需要读取的变量
***********************************************/
void Application::InitializeGloableVar() {
    GloableVar::init_ntp_time();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "NTP server时间同步");

    if (GloableVar::get_gloable_var() == ESP_OK) {
        ESP_LOGI(TAG, "读取全局环境变量");   
    }
    else {
        ESP_LOGE(TAG, "读取全局环境变量失败"); 
    }

    Board::GetInstance().GetAudioCodec()->SetOutputVolume(GloableVar::volume);
    vTaskDelay(pdMS_TO_TICKS(500));

    if (GloableVar::get_alarm_var() == ESP_OK) {
        ESP_LOGI(TAG, "读取闹钟环境变量");   
    }
    else {
        ESP_LOGE(TAG, "读取闹钟环境变量失败"); 
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}

void Application::InitializeMqtt() {
    // 初始化闹钟管理器（使用数据库）
    auto& alarm_manager = AlarmManager::GetInstance();
    if (!alarm_manager.Initialize(OTA_URI, GloableVar::device_id)) {
        ESP_LOGE(TAG, "Failed to initialize AlarmManager");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "初始化闹钟管理器成功");

    // 初始化MCP服务器（带闹钟功能）
    static McpServerWithAlarm mcp_server2;
    mcp_server2.Init();
    ESP_LOGI(TAG, "初始化闹钟MCP成功");

    // 音视频功能
    VoiceCall *voiceCall = VoiceCall::get_instance();
    Settings settings("mqtt", false);
    std::string mqtt_url = settings.GetString("url");
    mqtt_url = MQTT_URL;
    voiceCall->initMqtt(GloableVar::device_id, "KidAI_Robot", mqtt_url);
    voiceCall->initUdp();
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "初始化音视频功能成功 OK 啦 OK 啦 OK 啦了");
}

void Application::SW_Vibrating_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << SW_Vibrating_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    SW_Vibrating(0);                                        /* 关闭震动电机 */
}

/**************************
 * 初始化触摸传感器对应的任务线程
 **************************/
void Application::InitializeDebounceTouchThread() {
    touch_pin_map_["head"]  = TOUCH_1_GPIO_PIN;
    touch_pin_map_["hand"]  = TOUCH_2_GPIO_PIN;
    touch_pin_map_["chest"] = TOUCH_3_GPIO_PIN;
    touch_pin_map_["tail"]  = TOUCH_4_GPIO_PIN;

    xTaskCreate([](void* arg) {
        auto this_ = (Application*)arg;
        this_->TouchDebounceTask(arg);
        vTaskDelete(NULL);
    }, "touch_debounce_task", 1024 * 8, this, 3, nullptr);
}

/*****************************
 * 这个线程任务只负责对触摸进行消抖
 *****************************/
void Application::TouchDebounceTask(void *arg) {
    Application* application = static_cast<Application*>(arg);

    std::map<std::string, int64_t> last_touch_map;
    const int64_t repeat_debounce_ms = 8000;
    const int level_confirm_ms = 300;
    std::string region;
    while (true) {
        TouchRegion touch_region;
        if (xQueueReceive(application->touch_debounce_queue_, &touch_region, 0) == pdTRUE) {
            switch (touch_region) {
                case TouchRegion::HEAD:  region = "head";  break;
                case TouchRegion::HAND:  region = "hand";  break;
                case TouchRegion::CHEST: region = "chest"; break;
                case TouchRegion::TAIL:  region = "tail";  break;
                default: region = "head";  break;
            }

            // 第一级：等待 300ms 让电平稳定
            vTaskDelay(pdMS_TO_TICKS(level_confirm_ms));

            // 第二级：确认引脚仍然为高电平
            gpio_num_t gpio = application->touch_pin_map_[region];
            if (gpio_get_level(gpio) != 1) {
                continue;
            }

            // 第三级：同一区域 8000ms 内不重复处理
            int64_t now = esp_timer_get_time() / 1000;
            auto& last = last_touch_map[region];
            if (now - last >= repeat_debounce_ms) {
                last = now;

                static_cast<Application*>(arg)->touch_task_queue_.push_back(region);
                ESP_LOGI(TAG, "触摸 %s", region.c_str());
            }
        }
        else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}


/*****************************
 * 这个线程任务只负责任务的执行
 *****************************/
void Application::InitializeTouchThread() {
    xTaskCreate([](void* arg) {
        auto this_ = (Application*)arg;
        this_->TouchTask(arg);
        vTaskDelete(NULL);
    }, "touch_task", 1024 * 8, this, 3, nullptr);
}

void Application::TouchTask(void *arg) {
    Application* application = static_cast<Application*>(arg);

    std::string region;
    while (true) {
        if (!application->touch_task_queue_.empty()) {
            region = application->touch_task_queue_.front();
            application->touch_task_queue_.pop_front();

            // 正常对话时、电话时、播放故事时，都不触发的
            if ((kDeviceStateIdle == GetDeviceState()) && (Application::GetInstance().GetAudioService().is_voice_out_ == false)) {
                {
                    SW_Vibrating(1);
                    esp_timer_start_once(touch_timer_handle_, 3000 * 1000);
                }

                {
                    std::string touch_url = std::format("{}touch?deviceId={}&voiceType={}&region={}", OTA_URI, GloableVar::device_id, url_encode(GloableVar::voice_type), region);
                    Board::GetInstance().GetMusic()->PlayTouchAudio(touch_url);
                } 
            }
        }
        else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/*******************
 * 触摸传感器的中断函数
 * 补充：
 * 在中断函数中只做最轻量的操作
 *******************/
// 对应头部
static void IRAM_ATTR exit_TOUCH_1_isr_handler(void *arg)
{
    TouchRegion region = TouchRegion::HEAD;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(static_cast<Application*>(arg)->touch_debounce_queue_, &region, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR exit_TOUCH_2_isr_handler(void *arg)
{
    TouchRegion region = TouchRegion::HAND;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(static_cast<Application*>(arg)->touch_debounce_queue_, &region, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR exit_TOUCH_3_isr_handler(void *arg)
{
    TouchRegion region = TouchRegion::CHEST;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(static_cast<Application*>(arg)->touch_debounce_queue_, &region, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR exit_TOUCH_4_isr_handler(void *arg)
{
    TouchRegion region = TouchRegion::TAIL;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(static_cast<Application*>(arg)->touch_debounce_queue_, &region, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

/*****************
 * 触摸传感器的中断注册
 *****************/
void Application::TOUCH_1_init(void)
{
    gpio_config_t gpio_init_struct;

    /* 配置BOOT引脚和外部中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT;                    /* 选择为输入模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;           /* 上拉使能 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;      /* 下拉失能 */
    gpio_init_struct.intr_type = GPIO_INTR_POSEDGE;             /* 上升沿触发 */
    gpio_init_struct.pin_bit_mask = 1ull << TOUCH_1_GPIO_PIN;   /* 设置的引脚的位掩码 */
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));            /* 配置使能 */
    
    /* 注册中断服务 */
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    /* 设置BOOT的中断回调函数 */
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_1_GPIO_PIN, exit_TOUCH_1_isr_handler, (void*) this));
}

void Application::TOUCH_2_init(void)
{
    gpio_config_t gpio_init_struct;

    /* 配置BOOT引脚和外部中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT;                    /* 选择为输入模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;           /* 上拉使能 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;      /* 下拉失能 */
    gpio_init_struct.intr_type = GPIO_INTR_POSEDGE;             /* 上升沿触发 */
    gpio_init_struct.pin_bit_mask = 1ull << TOUCH_2_GPIO_PIN;   /* 设置的引脚的位掩码 */
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));            /* 配置使能 */
    
    /* 设置BOOT的中断回调函数 */
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_2_GPIO_PIN, exit_TOUCH_2_isr_handler, (void*) this));
}

void Application::TOUCH_3_init(void)
{
    gpio_config_t gpio_init_struct;

    /* 配置BOOT引脚和外部中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT;                    /* 选择为输入模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;           /* 上拉使能 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;      /* 下拉失能 */
    gpio_init_struct.intr_type = GPIO_INTR_POSEDGE;             /* 上升沿触发 */
    gpio_init_struct.pin_bit_mask = 1ull << TOUCH_3_GPIO_PIN;   /* 设置的引脚的位掩码 */
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));            /* 配置使能 */
    
    /* 设置BOOT的中断回调函数 */
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_3_GPIO_PIN, exit_TOUCH_3_isr_handler, (void*) this));
}

void Application::TOUCH_4_init(void)
{
    gpio_config_t gpio_init_struct;

    /* 配置BOOT引脚和外部中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT;                    /* 选择为输入模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;           /* 上拉使能 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;      /* 下拉失能 */
    gpio_init_struct.intr_type = GPIO_INTR_POSEDGE;             /* 上升沿触发 */
    gpio_init_struct.pin_bit_mask = 1ull << TOUCH_4_GPIO_PIN;   /* 设置的引脚的位掩码 */
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));            /* 配置使能 */
    
    /* 设置BOOT的中断回调函数 */
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_4_GPIO_PIN, exit_TOUCH_4_isr_handler, (void*) this));
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    // display->SetStatus(Lang::Strings::LOADING_PROTOCOL);
#if 0
    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }
#endif
    protocol_ = std::make_unique<WebsocketProtocol>();

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
#if 0
        /****************************
         * 为了解决第二次不能
         * 正常唤醒的问题
         ****************************/
        if (device_state_ == kDeviceStateConnecting) {
            Schedule([this]() {
                if (listening_mode_ == kListeningModeManualStop) {
                    SetDeviceState(kDeviceStateListening);
                }
            });
        }
#endif
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            // auto display = Board::GetInstance().GetDisplay();
            // display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        // display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    // display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    // display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    // display->SetStatus(status);
    display->SetEmotion(emotion);
    // display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        // display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        // display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }
        }

        if (GloableVar::mode_realtime == 1) {
            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        }
        else {
            SetListeningMode(kListeningModeAutoStop);
        }
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }
        }

        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        if (GloableVar::mode_realtime == 1) {
            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        }
        else {
            SetListeningMode(kListeningModeAutoStop);
        }
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

/*****************************
 * 专门为了响应用户切换实时打断模式
 *****************************/
void Application::SetModeRealtime() {
    if (GloableVar::mode_realtime == 1) {
        listening_mode_ = aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
    }
    else {
        listening_mode_ = kListeningModeAutoStop;
    }
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            // display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            // display->SetChatMessage("system", "");

            /****************************
             * 为了解决第二次不能
             * 正常唤醒的问题
             ****************************/
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        case kDeviceStateListening:
            // display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            // display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    VoiceCall::get_instance()->stop();
    Board::GetInstance().GetMusic()->Stop();
    AlarmManager::GetInstance().stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    // display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [display](int progress, size_t speed) {
        std::thread([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            // display->SetChatMessage("system", buffer);
        }).detach();
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        // display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        if (GloableVar::mode_realtime == 1) {
            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        }
        else {
            SetListeningMode(kListeningModeAutoStop);
        }
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        // auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            // display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            // display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            // display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

void Application::AddAudioData(AudioStreamPacket&& packet) {
    // 检查设备状态和编解码器状态
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || state_machine_.GetState() != kDeviceStateIdle || !codec->output_enabled()) {
        ESP_LOGD(TAG, "Skip audio data: device state=%d, codec output=%s", 
                state_machine_.GetState(), codec->output_enabled() ? "enabled" : "disabled");
        return;
    }

    // packet.payload包含的是原始PCM数据（int16_t）
    if (packet.payload.size() < 2) {
        ESP_LOGE(TAG, "Invalid audio packet size: %d bytes", packet.payload.size());
        return;
    }

    size_t num_samples = packet.payload.size() / sizeof(int16_t);
    
    // 使用静态缓冲区复用内存，避免频繁分配
    static std::vector<int16_t> pcm_buffer;
    static std::vector<int16_t> resampled_buffer;
    static size_t last_capacity_pcm = 0;
    static size_t last_capacity_resampled = 0;
    
    // 检查采样率有效性
    if (packet.sample_rate <= 0) {
        ESP_LOGE(TAG, "Invalid source sample rate: %d", packet.sample_rate);
        return;
    }
    
    int target_sample_rate = codec->output_sample_rate();
    if (target_sample_rate <= 0) {
        ESP_LOGE(TAG, "Invalid target sample rate: %d", target_sample_rate);
        return;
    }
    
    try {
        // 复用pcm_buffer，避免重新分配
        if (pcm_buffer.capacity() < num_samples) {
            // 如果容量不足，调整容量（稍大一些避免频繁调整）
            size_t new_capacity = num_samples * 2; // 2倍容量
            pcm_buffer.reserve(new_capacity);
            ESP_LOGD(TAG, "PCM buffer expanded: %d -> %d samples", 
                    last_capacity_pcm, new_capacity);
            last_capacity_pcm = new_capacity;
        }
        
        // 设置正确大小并复制数据
        pcm_buffer.resize(num_samples);
        memcpy(pcm_buffer.data(), packet.payload.data(), packet.payload.size());
        
        std::vector<int16_t>* data_to_play = &pcm_buffer;
        
        // 检查采样率是否匹配，如果不匹配则进行重采样
        if (packet.sample_rate != target_sample_rate) {
            // 计算重采样比率
            float ratio = static_cast<float>(packet.sample_rate) / target_sample_rate;
            size_t target_size = 0;
            
            if (packet.sample_rate > target_sample_rate) {
                // 降采样
                target_size = static_cast<size_t>(pcm_buffer.size() / ratio + 0.5f);
            } else {
                // 上采样
                float upsample_ratio = static_cast<float>(target_sample_rate) / packet.sample_rate;
                target_size = static_cast<size_t>(pcm_buffer.size() * upsample_ratio + 0.5f);
            }
            
            // 验证目标大小
            if (target_size == 0) {
                ESP_LOGE(TAG, "Invalid target size: %d", target_size);
                return;
            }
            
            // 复用重采样缓冲区
            if (resampled_buffer.capacity() < target_size) {
                // 如果容量不足，调整容量（稍大一些避免频繁调整）
                size_t new_capacity = target_size * 2; // 2倍容量
                resampled_buffer.reserve(new_capacity);
                ESP_LOGD(TAG, "Resampled buffer expanded: %d -> %d samples", 
                        last_capacity_resampled, new_capacity);
                last_capacity_resampled = new_capacity;
            }
            
            // 设置正确大小
            resampled_buffer.resize(target_size);
            
            // 根据不同的采样率比例选择不同的重采样策略
            if (packet.sample_rate > target_sample_rate) {
                // 降采样：跳跃采样
                for (size_t i = 0; i < target_size; ++i) {
                    float src_index = i * ratio;
                    size_t src_index_rounded = static_cast<size_t>(src_index + 0.5f);
                    
                    if (src_index_rounded < pcm_buffer.size()) {
                        resampled_buffer[i] = pcm_buffer[src_index_rounded];
                    } else {
                        // 越界处理，使用最后一个样本
                        resampled_buffer[i] = pcm_buffer.back();
                    }
                }
                
                ESP_LOGD(TAG, "Downsampled %d -> %d samples (ratio: %.3f)", 
                        pcm_buffer.size(), resampled_buffer.size(), ratio);
                        
            } else {
                // 上采样：线性插值
                float upsample_ratio = static_cast<float>(target_sample_rate) / packet.sample_rate;
                
                for (size_t i = 0; i < pcm_buffer.size(); ++i) {
                    size_t dest_index = static_cast<size_t>(i * upsample_ratio + 0.5f);
                    
                    // 确保索引在范围内
                    if (dest_index >= resampled_buffer.size()) {
                        dest_index = resampled_buffer.size() - 1;
                    }
                    
                    // 直接复制，后续进行插值
                    resampled_buffer[dest_index] = pcm_buffer[i];
                }
                
                // 填充插值
                for (size_t i = 1; i < resampled_buffer.size(); ++i) {
                    if (resampled_buffer[i] == 0) {
                        // 找到前一个非零样本
                        size_t prev_index = i - 1;
                        while (prev_index > 0 && resampled_buffer[prev_index] == 0) {
                            --prev_index;
                        }
                        
                        // 找到后一个非零样本
                        size_t next_index = i + 1;
                        while (next_index < resampled_buffer.size() && resampled_buffer[next_index] == 0) {
                            ++next_index;
                        }
                        
                        // 线性插值
                        if (prev_index > 0 && next_index < resampled_buffer.size()) {
                            float t = static_cast<float>(i - prev_index) / (next_index - prev_index);
                            int16_t prev = resampled_buffer[prev_index];
                            int16_t next = resampled_buffer[next_index];
                            resampled_buffer[i] = static_cast<int16_t>(prev + (next - prev) * t);
                        } else if (prev_index > 0) {
                            // 没有后一个样本，使用前一个样本
                            resampled_buffer[i] = resampled_buffer[prev_index];
                        } else if (next_index < resampled_buffer.size()) {
                            // 没有前一个样本，使用后一个样本
                            resampled_buffer[i] = resampled_buffer[next_index];
                        }
                    }
                }
                
                ESP_LOGD(TAG, "Upsampled %d -> %d samples (ratio: %.2f)", 
                        pcm_buffer.size(), resampled_buffer.size(), upsample_ratio);
            }
            
            data_to_play = &resampled_buffer;
        }
        
        // 确保音频输出已启用
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
            ESP_LOGD(TAG, "Audio output enabled");
        }
        
        // 验证要播放的数据
        if (data_to_play->empty()) {
            ESP_LOGE(TAG, "No audio data to play");
            return;
        }
        
        // 发送PCM数据到音频编解码器
        codec->OutputData(*data_to_play);
        
        // 更新最后输出时间，防止OnAudioOutput自动禁用音频
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_service_.last_output_time_ = std::chrono::steady_clock::now();
        }
        
        // 记录统计信息（可选，调试用）
        static size_t total_played = 0;
        total_played += data_to_play->size() * sizeof(int16_t);
        if (total_played % (1024 * 1024) == 0) { // 每1MB记录一次
            ESP_LOGD(TAG, "Total audio data played: %.2f MB", 
                    total_played / (1024.0 * 1024.0));
            
            // 定期检查内存状态
            size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGD(TAG, "Heap status: free=%d, min_free=%d", free_heap, min_free_heap);
        }
        
    } catch (const std::bad_alloc& e) {
        ESP_LOGE(TAG, "Memory allocation failed in AddAudioData: %s", e.what());
        
        // 内存不足，尝试清理缓冲区
        pcm_buffer.clear();
        resampled_buffer.clear();
        
        // 强制内存整理
        pcm_buffer.shrink_to_fit();
        resampled_buffer.shrink_to_fit();
        
        // 记录内存状态
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "Memory emergency: free heap = %d bytes", free_heap);
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in AddAudioData: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "Unknown exception in AddAudioData");
    }
}
