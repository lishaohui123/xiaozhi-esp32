#include "voice_call.h"
#include <esp_log.h>
#include "esp_err.h"
#include "string.h"
#include "stdlib.h"
#include "time.h"
#include "sys/time.h"
#include "driver/i2s.h"
#include "esp_netif.h"
#include "alarm_manager.h"
#include "voice_call_client.h"
#include "gloable_var.h"
#include "settings.h"
#include "alarm_manager.h"
#include <string>
#include <sstream>
#include <iomanip>
#include "esp_ns.h"
#include "esp_agc.h"
#include "audio_codec.h"
#include "board.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "constants.h"

// 使用extern "C"包含OPUS头文件，解决C++调用C库的问题
extern "C" {
#include "opus.h"
}

// 日志标签
#define TAG "VoiceCall"

// 单例实例
VoiceCall* VoiceCall::s_instance = nullptr;

static std::string urlEncode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
            continue;
        }
        
        escaped << std::uppercase;
        escaped << '%' << std::setw(2) << int((unsigned char)c);
        escaped << std::nouppercase;
    }

    return escaped.str();
}

// 生成随机验证码
static std::string generate_verification_code(int length) {
    const char charset[] = "0123456789";
    std::string code;
    code.reserve(length);  // 预分配空间，提高性能
    
    for (int i = 0; i < length; i++) {
        code.push_back(charset[esp_random() % (sizeof(charset) - 1)]);
    }
    
    return code;  // std::string 自动管理内存，无需手动添加 '\0'
}

// 获取当前时间戳（秒）
static time_t get_current_time() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec;
}

static uint64_t get_current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 将通话状态转换为字符串
static const char* get_call_state_str(call_state_t state) {
    switch (state) {
        case CALL_STATE_IDLE:
            return "idle";
        case CALL_STATE_WAITING:
            return "waiting";
        case CALL_STATE_CONNECTING:
            return "connecting";
        case CALL_STATE_IN_CALL:
            return "in_call";
        case CALL_STATE_ENDING:
            return "ending";
        default:
            return "unknown";
    }
}

// VoiceCall类实现
VoiceCall::VoiceCall() {
    // 通话信息初始化

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            VoiceCall* voiceCall = (VoiceCall*)arg;
			voiceCall->mqtt_.reset();
            
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Reconnecting to MQTT server");

            app.Schedule([voiceCall]() {
                std::string mqtt_url = MQTT_URL;
                voiceCall->initMqtt(GloableVar::device_id, "KidAI_Robot", mqtt_url);
            });
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);


    // 检测通话状态的定时器
    esp_timer_create_args_t call_status_timer_args = {
        .callback = [](void* arg) {
            VoiceCall* voiceCall = (VoiceCall*)arg;
            
            auto& app = Application::GetInstance();

            app.Schedule([voiceCall]() {
                voiceCall->handle_call_end();
            });
        },
        .arg = this,
    };
    esp_timer_create(&call_status_timer_args, &call_status_timer_);

    
    m_udp_receive_task_handle = nullptr;
    m_udp_send_task_handle = nullptr;

    // 🔥 初始化静态任务分配相关成员
    m_udp_receive_task_stack_ = nullptr;
    m_udp_receive_task_tcb_ = nullptr;
    m_udp_receive_task_static_allocated_ = false;

    m_udp_send_task_stack_ = nullptr;
    m_udp_send_task_tcb_ = nullptr;
    m_udp_send_task_static_allocated_ = false;
    m_phone_call_stack_ = nullptr;
    m_phone_call_tcb_ = nullptr;
    m_phone_call_task_handle_ = nullptr;

    // UDP状态初始化
    m_udp_receive_task_running = false;
    m_udp_send_task_running = false;
    
    // 确保设备信息有合理的默认值
    m_device_info.device_id = "unknown";
    m_device_info.device_name = "Unknown Device";
    m_device_info.ip_address = "0.0.0.0";

    event_group_ = xEventGroupCreate();

    websocket_reconnect_failed_ = false;
}

VoiceCall::~VoiceCall() {

    // MQTT相关
    mqtt_.reset();

    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }
        
    // 停止 UDP 相关任务
    m_udp_receive_task_running = false;
    m_udp_send_task_running = false;
    
    // 等待任务退出
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 删除任务
    if (m_udp_receive_task_handle != nullptr) {
        if (m_udp_receive_task_static_allocated_) {
            vTaskDelete(m_udp_receive_task_handle);
            m_udp_receive_task_static_allocated_ = false;
        }
        m_udp_receive_task_handle = nullptr;
    }
    
    if (m_udp_send_task_handle != nullptr) {
        if (m_udp_send_task_static_allocated_) {
            vTaskDelete(m_udp_send_task_handle);
            m_udp_send_task_static_allocated_ = false;
        }
        m_udp_send_task_handle = nullptr;
    }
    
    // 🔥 释放任务栈和控制块内存
    if (m_udp_receive_task_stack_ != nullptr) {
        heap_caps_free(m_udp_receive_task_stack_);
        m_udp_receive_task_stack_ = nullptr;
    }
    if (m_udp_receive_task_tcb_ != nullptr) {
        heap_caps_free(m_udp_receive_task_tcb_);
        m_udp_receive_task_tcb_ = nullptr;
    }
    if (m_udp_send_task_stack_ != nullptr) {
        heap_caps_free(m_udp_send_task_stack_);
        m_udp_send_task_stack_ = nullptr;
    }
    if (m_udp_send_task_tcb_ != nullptr) {
        heap_caps_free(m_udp_send_task_tcb_);
        m_udp_send_task_tcb_ = nullptr;
    }
    // 🔥 释放电话异步任务的内存
    if (m_phone_call_stack_ != nullptr) {
        heap_caps_free(m_phone_call_stack_);
        m_phone_call_stack_ = nullptr;
    }
    if (m_phone_call_tcb_ != nullptr) {
        heap_caps_free(m_phone_call_tcb_);
        m_phone_call_tcb_ = nullptr;
    }
}

void VoiceCall::stop() {

   // MQTT相关
    mqtt_.reset();

    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }
        
    // 停止 UDP 相关任务
    m_udp_receive_task_running = false;
    m_udp_send_task_running = false;
    
    // 等待任务退出
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 删除任务
    if (m_udp_receive_task_handle != nullptr) {
        if (m_udp_receive_task_static_allocated_) {
            vTaskDelete(m_udp_receive_task_handle);
            m_udp_receive_task_static_allocated_ = false;
        }
        m_udp_receive_task_handle = nullptr;
    }
    
    if (m_udp_send_task_handle != nullptr) {
        if (m_udp_send_task_static_allocated_) {
            vTaskDelete(m_udp_send_task_handle);
            m_udp_send_task_static_allocated_ = false;
        }
        m_udp_send_task_handle = nullptr;
    }
#if 0
    // 🔥 释放任务栈和控制块内存
    if (m_udp_receive_task_stack_ != nullptr) {
        heap_caps_free(m_udp_receive_task_stack_);
        m_udp_receive_task_stack_ = nullptr;
    }
    if (m_udp_receive_task_tcb_ != nullptr) {
        heap_caps_free(m_udp_receive_task_tcb_);
        m_udp_receive_task_tcb_ = nullptr;
    }
    if (m_udp_send_task_stack_ != nullptr) {
        heap_caps_free(m_udp_send_task_stack_);
        m_udp_send_task_stack_ = nullptr;
    }
    if (m_udp_send_task_tcb_ != nullptr) {
        heap_caps_free(m_udp_send_task_tcb_);
        m_udp_send_task_tcb_ = nullptr;
    }
    // 🔥 释放电话异步任务的内存
    if (m_phone_call_stack_ != nullptr) {
        heap_caps_free(m_phone_call_stack_);
        m_phone_call_stack_ = nullptr;
    }
    if (m_phone_call_tcb_ != nullptr) {
        heap_caps_free(m_phone_call_tcb_);
        m_phone_call_tcb_ = nullptr;
    }
#endif
}

VoiceCall* VoiceCall::get_instance() {
    if (s_instance == nullptr) {
        s_instance = new VoiceCall();
    }
    return s_instance;
}

/****************************************
 * 在初始化函数中
 * 1、初始化小智设备的信息；
 * 2、初始化MQTT，创建回调等；
 *****************************************/
esp_err_t VoiceCall::initMqtt(const std::string& device_id, const std::string& device_name, const std::string& mqtt_broker_uri) {
    ESP_LOGI(TAG, "Initializing VoiceCall");

    // 初始化设备信息
    m_device_info.device_id = device_id;
    m_device_info.device_name = device_name;

    // 初始化 MQTT - 使用改进的配置
    int keepalive_interval = 60;

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    // 设置回调
    mqtt_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
      esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        handle_mqtt_message(topic.c_str(), payload.c_str(), 0);
    });

    // 连接MQTT
    if (!mqtt_->Connect(MQTT_HOST, 1883, m_device_info.device_id, "", "")) {
		esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
		
        ESP_LOGE(TAG, "Failed to connect to endpoint, code=%d", mqtt_->GetLastError());
        return ESP_FAIL;
    }

    // 订阅主题
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/call/request");
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/call/accept");
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/call/end");
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/call/response");
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/alarm");
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/works");
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/audio");
    mqtt_->Subscribe("social/" + m_device_info.device_id + "/hd");
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/set");
    mqtt_->Subscribe("device/" + m_device_info.device_id + "/update");
    
    ESP_LOGI(TAG, "VoiceCall initialized successfully");
    return ESP_OK;
}

void VoiceCall::initUdp() {
    // 关闭之前的套接字（防止泄漏）
    if (client_sock_ >= 0) {
        close(client_sock_);
    }
    
    // 创建套接字
    client_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_sock_ < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return;
    }

    // 设置socket选项（可选：允许地址重用）
    int reuse = 1;
    if (setsockopt(client_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_REUSEADDR");
    }

    // 绑定本地端口（系统自动分配）
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = 0; // 自动分配
    if (bind(client_sock_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind UDP socket");
        close(client_sock_);
        client_sock_ = -1;
        return;
    }

    // 获取实际分配的端口（可选）
    socklen_t len = sizeof(local_addr);
    if (getsockname(client_sock_, (struct sockaddr*)&local_addr, &len) == 0) {
        ESP_LOGI(TAG, "UDP socket bound to port %d", ntohs(local_addr.sin_port));
    }

    // 【可选】设置为非阻塞模式，以便在 recvfrom 时立即返回
    int flags = fcntl(client_sock_, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGE(TAG, "fcntl F_GETFL failed");
    } else {
        if (fcntl(client_sock_, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGE(TAG, "fcntl F_SETFL O_NONBLOCK failed");
        } else {
            ESP_LOGI(TAG, "UDP socket set to non-blocking mode");
        }
    }

    // 初始化服务器地址（通话开始时会根据 m_call_info 更新）
    memset(&server_addr_, 0, sizeof(server_addr_));
    // memset(&from_addr_, 0, sizeof(from_addr_));
}

/******************************************
 * 主要启动语音通话相关的进程
 * 1、发送语音进程；
 * 2、接收语音进程；
 *******************************************/
void VoiceCall::start_audio_processing() {
    ESP_LOGI(TAG, "Starting audio processing");

    {
        m_udp_receive_task_running = true;
        m_udp_send_task_running = true;

        websocket_reconnect_failed_ = false;
    }

    // 1、启动通话状态查询的任务
    {
        esp_timer_start_once(call_status_timer_, CALL_STATUS_INTERVAL_MS * 500);
    }

    // 2、接收语音进程；
    {
        // 🔥 为接收任务分配内存并创建静态任务
        if (m_udp_receive_task_stack_ == nullptr) {
            m_udp_receive_task_stack_ = (StackType_t*)heap_caps_malloc(
                UDP_RECEIVE_TASK_STACK_SIZE * sizeof(StackType_t), 
                MALLOC_CAP_SPIRAM
            );
            assert(m_udp_receive_task_stack_ != nullptr);
        }
        
        if (m_udp_receive_task_tcb_ == nullptr) {
            m_udp_receive_task_tcb_ = (StaticTask_t*)heap_caps_malloc(
                sizeof(StaticTask_t), 
                MALLOC_CAP_INTERNAL
            );
            assert(m_udp_receive_task_tcb_ != nullptr);
        }

        // 使用静态分配创建接收任务
        m_udp_receive_task_handle = xTaskCreateStatic(
            [](void* arg) {
                VoiceCall* inst = (VoiceCall*)arg;
                inst->udp_receive_task();
            },
            "ws_receive_task",
            UDP_RECEIVE_TASK_STACK_SIZE,
            this,
            4,
            m_udp_receive_task_stack_,
            m_udp_receive_task_tcb_
        );

        assert(m_udp_receive_task_handle != nullptr);

        m_udp_receive_task_static_allocated_ = true;
        ESP_LOGI(TAG, "UDP receive task created (static)");
    }

    // 1、发送语音进程；
    {
        // 🔥 为发送任务分配内存并创建静态任务
        if (m_udp_send_task_stack_ == nullptr) {
            m_udp_send_task_stack_ = (StackType_t*)heap_caps_malloc(
                UDP_SEND_TASK_STACK_SIZE * sizeof(StackType_t), 
                MALLOC_CAP_SPIRAM
            );
            assert(m_udp_send_task_stack_ != nullptr);
        }
        
        if (m_udp_send_task_tcb_ == nullptr) {
            m_udp_send_task_tcb_ = (StaticTask_t*)heap_caps_malloc(
                sizeof(StaticTask_t), 
                MALLOC_CAP_INTERNAL
            );
            assert(m_udp_send_task_tcb_ != nullptr);
        }

        // 使用静态分配创建发送任务
        m_udp_send_task_handle = xTaskCreateStatic(
            [](void* arg) {
                VoiceCall* inst = (VoiceCall*)arg;
                inst->udp_send_task();
            },
            "ws_send_task",
            UDP_SEND_TASK_STACK_SIZE,
            this,
            8,
            m_udp_send_task_stack_,
            m_udp_send_task_tcb_
        );

        assert(m_udp_send_task_handle != nullptr);

        m_udp_send_task_static_allocated_ = true;
        ESP_LOGI(TAG, "UDP send task created (static)");
    }

    ESP_LOGI(TAG, "Audio processing started");
}

void VoiceCall::stop_audio_processing() {
    ESP_LOGI(TAG, "Stopping audio processing");

    {
        m_udp_receive_task_running = false;
        m_udp_send_task_running = false;
        m_call_info.state = CALL_STATE_IDLE;
        
        // 等待音频任务自然退出
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // 🔥 等待任务退出并清理任务句柄
    if (m_udp_receive_task_handle != nullptr) {
        // 等待接收任务退出
        while (!udp_receive_task_exit) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        if (m_udp_receive_task_static_allocated_) {
            vTaskDelete(m_udp_receive_task_handle);
            m_udp_receive_task_static_allocated_ = false;
        }
        m_udp_receive_task_handle = nullptr;
    }
    
    if (m_udp_send_task_handle != nullptr) {
        // 等待发送任务退出
        while (!udp_send_task_exit) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        if (m_udp_send_task_static_allocated_) {
            vTaskDelete(m_udp_send_task_handle);
            m_udp_send_task_static_allocated_ = false;
        }
        m_udp_send_task_handle = nullptr;
    }

    ESP_LOGI(TAG, "Audio processing stopped");
}

/*******************************************************************
 * 设备主动呼叫小程序的流程
 * 1、调用后端接口获取拨打对象(爸爸、妈妈)的微信id；
 * 2、调用后台同步接口，等待家长的接听；
 *******************************************************************/
void VoiceCall::make_call(std::string member) {
  
    ESP_LOGI(TAG, "开始给宝贝家长打电话");

    {
        // 让设备处于idle状态下
        auto& application = Application::GetInstance();

        Protocol* protocol = application.GetProtocol();
        if (protocol && protocol->IsAudioChannelOpened()) {
            protocol->CloseAudioChannel();
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        application.SetDeviceState(kDeviceStateIdle);

        /***********************
        * 启用麦克风，为了解决后端没有
        * 响应时，麦克风缓存满了，告警的问题
        ***********************/
        ESP_LOGI(TAG, "Configuring audio codec...");
        auto audio_codec = Board::GetInstance().GetAudioCodec();
        audio_codec->EnableInput(false);
        vTaskDelay(pdMS_TO_TICKS(200));

        application.GetAudioService().EnableVoiceProcessing(false);
        ESP_LOGI(TAG, "Audio input enabled successfully");
    }
        
    {
        // 1、调用后端接口获取拨打对象(爸爸、妈妈)的微信id；
        auto& voiceCallClient = VoiceCallClient::GetInstance();
        std::string server_url = OTA_URI;
        voiceCallClient.Initialize(server_url, GloableVar::device_id);

        VoiceCallClient::ChatResult chat_rel = voiceCallClient.getChatId(GloableVar::device_id, urlEncode(member));
        
        // 用户没有绑定微信
        if (chat_rel.chat_id == "") {
            std::string s = "小主人，给" + member + "的微信打电话失败了，快提醒他绑定微信吧！";
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);

            ESP_LOGE(TAG, "宝贝的家长没有绑定微信");
            return;
        }

        m_call_info.parent_openid = chat_rel.chat_id;
    }

    {
        // 为了快速的响应用户的打电话请求
        std::string s = std::format("好嘞，小主人，正在给 {} 打电话，请稍后！", member);
        auto& alarm_manager = AlarmManager::GetInstance();
        alarm_manager.PlayTtsAudioStreamVoice(s, 1);
    }
  
    {
        // 2、调用后台同步接口，等待家长的接听；
        auto& voiceCallClient = VoiceCallClient::GetInstance();
        std::string server_url = OTA_URI;
        voiceCallClient.Initialize(server_url, GloableVar::device_id);
        VoiceCallClient::CallRequest rel_callrequest = voiceCallClient.getCallRequest(GloableVar::device_id, m_call_info.parent_openid);

        if (rel_callrequest.type == "call_rejected") {
            std::string s = std::format("小主人，{} 拒绝了电话，可能 {} 在忙呦，过一会再给他打电话吧！", member, member);
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);

            ESP_LOGE(TAG, "宝贝的家长拒绝了电话");
            return;
        }

        // 为了应对请求超时的情况，没有任何的返回
        if (rel_callrequest.type != "call_accepted") {
            std::string s = std::format("小主人，{} 拒绝了电话，可能 {} 在忙呦，过一会再给他打电话吧！", member, member);
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);

            ESP_LOGE(TAG, "宝贝的家长拒绝了电话");
            return;
        }
        
        m_call_info.state = CALL_STATE_IN_CALL;
        m_call_info.roomSn = rel_callrequest.roomSn;
        m_call_info.server_ip = rel_callrequest.roomIp;
        m_call_info.server_port = rel_callrequest.roomPort;
        m_call_info.roomToken = rel_callrequest.roomToken;

        // 开始音频处理
        {
            m_udp_send_task_running = true;
            m_udp_receive_task_running = true;
            start_audio_processing();
        }
    }

    ESP_LOGI(TAG, "给家长接通电话 parent_openid %s", m_call_info.parent_openid.c_str());
    return;
}

void VoiceCall::make_call_async(const std::string& member) {
    // 杀掉上一次的 phone_call 任务
    if (m_phone_call_task_handle_ != nullptr) {
        vTaskDelete(m_phone_call_task_handle_);
        m_phone_call_task_handle_ = nullptr;
    }

    // 与 start_audio_processing 模式一致：只申请一次内存，一直持有
    if (m_phone_call_stack_ == nullptr) {
        m_phone_call_stack_ = (StackType_t*)heap_caps_malloc(
            PHONE_CALL_STACK_SIZE * sizeof(StackType_t),
            MALLOC_CAP_SPIRAM
        );
        assert(m_phone_call_stack_ != nullptr);
    }

    if (m_phone_call_tcb_ == nullptr) {
        m_phone_call_tcb_ = (StaticTask_t*)heap_caps_malloc(
            sizeof(StaticTask_t),
            MALLOC_CAP_INTERNAL
        );
        assert(m_phone_call_tcb_ != nullptr);
    }

    this->phone_member_ = member;

    m_phone_call_task_handle_ = xTaskCreateStatic(
        [](void* arg) {
        VoiceCall* inst = (VoiceCall*)arg;
        inst->make_call(inst->phone_member_);
        while (true) vTaskDelay(portMAX_DELAY);  // 防止任务退出
    }, "phone_call", PHONE_CALL_STACK_SIZE, this, 2, m_phone_call_stack_, m_phone_call_tcb_);

    assert(m_phone_call_task_handle_ != nullptr);
}

std::string VoiceCall::mcp_generate_verification_code() {
    // 重新生成验证码
    regenerate_verification_code();
    
    // 回复命令执行结果
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "device_id", m_device_info.device_id.c_str());
    cJSON_AddStringToObject(response, "command", "regenerate_verification_code");
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "verification_code", m_device_info.verification_code.c_str());
    cJSON_AddNumberToObject(response, "verification_code_expiry", m_device_info.verification_code_expiry);    
    char* response_payload = cJSON_PrintUnformatted(response);

    std::string device_command_topic = MQTT_TOPIC_DEVICE_COMMAND;
    send_mqtt_message(device_command_topic.c_str(), response_payload);
    
    cJSON_Delete(response);
    free(response_payload);

    return m_device_info.verification_code;
}

void VoiceCall::SetWebsocketSendAudio() {
    xEventGroupSetBits(event_group_, WEBSOCKET_SEND_AUDIO);
}

void VoiceCall::handle_mqtt_message(const char* topic, const char* payload, int payload_len) {
    if (!topic || !payload) {
        ESP_LOGE(TAG, "Invalid MQTT message");
        return;
    }
    
    ESP_LOGI(TAG, "Handling MQTT message: topic=%s, payload=%s", topic, payload);
    
    // 检查是否是通话请求
    char device_call_request_topic[64];
    snprintf(device_call_request_topic, sizeof(device_call_request_topic), MQTT_TOPIC_DEVICE_CALL_REQUEST, m_device_info.device_id.c_str());
    if (strcmp(topic, device_call_request_topic) == 0) {
        handle_incoming_call(payload);
        return;
    }

    char device_alarm_topic[64];
    snprintf(device_alarm_topic, sizeof(device_alarm_topic), TOPIC_DEVICE_ALARM_TEMPLATE, m_device_info.device_id.c_str());
    if (strcmp(topic, device_alarm_topic) == 0) {
        handle_device_alarm(payload);
        return;
    }

    char device_works_topic[64];
    snprintf(device_works_topic, sizeof(device_works_topic), TOPIC_DEVICE_WORKS_TEMPLATE, m_device_info.device_id.c_str());
    if (strcmp(topic, device_works_topic) == 0) {
        handle_device_works(payload);
        return;
    }

    char device_audio_topic[64];
    snprintf(device_audio_topic, sizeof(device_audio_topic), TOPIC_DEVICE_AUDIO_TEMPLATE, m_device_info.device_id.c_str());
    if (strcmp(topic, device_audio_topic) == 0) {
        handle_device_audio(payload);
        return;
    }

    char social_hd_topic[64];
    snprintf(social_hd_topic, sizeof(social_hd_topic), TOPIC_SOCIAL_HD_TEMPLATE, m_device_info.device_id.c_str());
    if (strcmp(topic, social_hd_topic) == 0) {
        handle_social_hd(payload);
        return;
    }

    char device_set_topic[64];
    snprintf(device_set_topic, sizeof(device_set_topic), TOPIC_DEVICE_SET_TEMPLATE, m_device_info.device_id.c_str());
    if (strcmp(topic, device_set_topic) == 0) {
        handle_device_set(payload);
        return;
    }

    char device_update_topic[64];
    snprintf(device_update_topic, sizeof(device_update_topic), TOPIC_DEVICE_UPDATE_TEMPLATE, m_device_info.device_id.c_str());
    if (strcmp(topic, device_update_topic) == 0) {
        handle_device_update(payload);
        return;
    }


    ESP_LOGW(TAG, "Unhandled MQTT topic: %s", topic);
}

// 在发送 MQTT 消息时检查连接状态
esp_err_t VoiceCall::send_mqtt_message(const char* topic, const char* payload) {
    if (!mqtt_->Publish(topic, payload)) {
        ESP_LOGE(TAG, "MQTT message published failed: topic=%s", topic);
        return ESP_FAIL;
    }
  
    ESP_LOGI(TAG, "MQTT message published successfully: topic=%s", topic);
    return ESP_OK;
}

/**************************************************************************************
 * 收到微信小程序的通话请求
 * 1、解析通话请求；
 * 2、启动通话；
 *************************************************************************************/
void VoiceCall::handle_incoming_call(const char* payload) {
    /********************
     * 通话状态判断
     *********************/
    if (m_call_info.state != CALL_STATE_IDLE) {
        cJSON* root = cJSON_Parse(payload);
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse incoming call payload");
            return;
        }
        
        std::string roomSn = std::string(cJSON_GetObjectItem(root, "roomSn")->valuestring);
        cJSON_Delete(root);

        // 调用接口通知小程序
        auto& voiceCallClient = VoiceCallClient::GetInstance();
        std::string server_url = OTA_URI;
        voiceCallClient.Initialize(server_url, GloableVar::device_id);

        voiceCallClient.setResponse(m_device_info.device_id, roomSn, "0");

        ESP_LOGE(TAG, "当前状态不是空闲状态，不能响应通话请求");
        return;
    }

    if (!payload) {
        ESP_LOGE(TAG, "Invalid incoming call payload");
        return;
    }
    
    ESP_LOGI(TAG, "Handling incoming call: %s", payload);
    
    /*******************
     * 解析通话请求
     ********************/
    {
        cJSON* root = cJSON_Parse(payload);
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse incoming call payload");
            return;
        }
        
        m_call_info.roomSn = std::string(cJSON_GetObjectItem(root, "roomSn")->valuestring);
        m_call_info.server_ip = std::string(cJSON_GetObjectItem(root, "roomIp")->valuestring);
        m_call_info.server_port = std::stoi(std::string(cJSON_GetObjectItem(root, "roomPort")->valuestring));
        m_call_info.roomToken = std::string(cJSON_GetObjectItem(root, "roomToken")->valuestring);    
        m_call_info.parent_openid = std::string(cJSON_GetObjectItem(root, "parent_openid")->valuestring);
        m_call_info.state = CALL_STATE_IN_CALL;

        cJSON_Delete(root);
    }

    /******************************************************
     * 语音提醒来电话
     * 如果正在进行音乐播放，则中止音乐播放
     ******************************************************/
    {
        auto& voiceCallClient = VoiceCallClient::GetInstance();
        std::string server_url = OTA_URI;
        voiceCallClient.Initialize(server_url, GloableVar::device_id);

        VoiceCallClient::RelationResult rel_relation = voiceCallClient.getRelation(GloableVar::device_id, m_call_info.parent_openid);
        if (rel_relation.success == true) {
            std::string s = std::format("小主人，{} 来电话了，快点接电话吧！", rel_relation.relation);
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);
        }
        else {
            std::string s = std::format("小主人，来电话了，快点接电话吧！");
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);

            // ESP_LOGW(TAG, "获取用户关系失败 call_id %s", m_call_info.call_id.c_str());
        }
    }

    {
        // 调用接口通知小程序
        auto& voiceCallClient = VoiceCallClient::GetInstance();
        std::string server_url = OTA_URI;
        voiceCallClient.Initialize(server_url, GloableVar::device_id);

        voiceCallClient.setResponse(m_device_info.device_id, m_call_info.roomSn, "1");
    }

        
    /**********************
     * 启动通话
     **********************/
    {
        m_udp_send_task_running = true;
        m_udp_receive_task_running = true;
        m_call_info.state = CALL_STATE_IN_CALL;
        start_audio_processing();
    }
}

/*******************************
 * 通话时轮询通话的状态，结束通话
 *******************************/
void VoiceCall::handle_call_end() {
    // 1、轮询通话的状态
    auto& voiceCallClient = VoiceCallClient::GetInstance();
    std::string server_url = OTA_URI;
    voiceCallClient.Initialize(server_url, GloableVar::device_id);

    // 2、停止音频、复位状态；
    if (voiceCallClient.getCallStatus(GloableVar::device_id, m_call_info.roomSn)) {
        // 结束通话
        ESP_LOGI(TAG, "Ending call...");
    
        // 停止音频处理
        stop_audio_processing();
        
        // 重置通话信息
        reset_call_state();

        esp_timer_stop(call_status_timer_);

        ESP_LOGI(TAG, "Call ended");
    }
    else {
        esp_timer_start_once(call_status_timer_, CALL_STATUS_INTERVAL_MS * 500);
    }
}

/******************************************
 * 收到后端的闹钟更新通知，即刻
 * 获取新的闹钟列表
 ******************************************/
void VoiceCall::handle_device_alarm(const char* payload) {
    AlarmManager::GetInstance().SyncWithDb();
}

/************************************************
 * 接收小程序家长端点播的作品的播放，
 * 从头开始播放
 *************************************************/
void VoiceCall::handle_device_works(const char* payload) {
    if (!payload) {
        ESP_LOGE(TAG, "Invalid call end payload");
        return;
    }

    ESP_LOGI(TAG, "Handling play works: %s", payload);

    cJSON* root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse call end payload");
        return;
    }

    std::string work_name = std::string(cJSON_GetObjectItem(root, "workName")->valuestring);
    cJSON_Delete(root);

    if (m_udp_receive_task_running || m_udp_send_task_running) {
        ESP_LOGI(TAG, "正在通话中，音乐播放不响应");
    }
    else {
        auto music = Board::GetInstance().GetMusic();
        music->Download(work_name, false);
    }
}

/************************************************
 * 接收小程序家长端点播的音频的播放，
 * 从头开始播放
 *************************************************/
void VoiceCall::handle_device_audio(const char* payload) {
    if (!payload) {
        ESP_LOGE(TAG, "Invalid call end payload");
        return;
    }

    ESP_LOGI(TAG, "Handling play audio: %s", payload);

    cJSON* root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse call end payload");
        return;
    }

    std::string audio_url = std::string(cJSON_GetObjectItem(root, "audioUrl")->valuestring);
    cJSON_Delete(root);

    if (m_udp_receive_task_running || m_udp_send_task_running) {
        ESP_LOGI(TAG, "正在通话中，音乐播放不响应");
    }
    else {
        auto music = Board::GetInstance().GetMusic();
        music->Play2(audio_url);
    }
}

/*********************************************
* 处理小程序上对设备的操作，比如 音量、实时打断等操作
*********************************************/
void VoiceCall::handle_device_set(const char* payload) {
    if (!payload) {
        ESP_LOGE(TAG, "Invalid  device set payload");
        return;
    }

    ESP_LOGI(TAG, "Handling device set: %s", payload);

    cJSON* root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse device set payload");
        return;
    }

    std::string set_type = std::string(cJSON_GetObjectItem(root, "setType")->valuestring);

    if (set_type == "volume") {
        int volume = cJSON_GetObjectItem(root, "number")->valueint;

        auto codec = Board::GetInstance().GetAudioCodec();
        codec->SetOutputVolume(volume);
    }

    if (set_type == "modeRealtime") {
        int mode_realtime = cJSON_GetObjectItem(root, "number")->valueint;
        GloableVar::mode_realtime = mode_realtime;

        Application::GetInstance().SetModeRealtime();
    }

    cJSON_Delete(root);
}

/*********************************************
* 进行固件的自动升级
*********************************************/
void VoiceCall::handle_device_update(const char* payload) {

    Application::GetInstance().UpdateFirmwareTask();

}

/*********************************************
* 处理小龙的朋友之间的互动，戳一下、送爱心、留言等操作
*********************************************/
void VoiceCall::handle_social_hd(const char* payload) {
    if (!payload) {
        ESP_LOGE(TAG, "Invalid  social hu dong payload");
        return;
    }

    ESP_LOGI(TAG, "Handling play audio: %s", payload);

    cJSON* root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse social hu dong payload");
        return;
    }

    std::string hd_type = std::string(cJSON_GetObjectItem(root, "hdType")->valuestring);

    if (m_udp_receive_task_running || m_udp_send_task_running) {
        ESP_LOGI(TAG, "正在通话中，朋友之间的互动不响应");
    }
    else {
        std::string friend_name = std::string(cJSON_GetObjectItem(root, "friendName")->valuestring);
        std::string city = std::string(cJSON_GetObjectItem(root, "city")->valuestring);

        if (hd_type == "chuo") {
            std::string s = "小主人，你来自" + city + "的好朋友" + friend_name + "戳了你一下！快去给他回复吧。";
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);
        }

        if (hd_type == "love") {
            std::string s = "小主人，你来自" + city + "的好朋友" + friend_name + "给你送了一个爱心！快去给他回复吧。";
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);
        }

        if (hd_type == "audio") {
            std::string s = "小主人，你来自" + city + "的好朋友" + friend_name + "给你留言了！留言的内容如下。";
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.PlayTtsAudioStreamVoice(s, 1);

            vTaskDelay(pdMS_TO_TICKS(3000));

            long recordId = (long)(cJSON_GetObjectItem(root, "id")->valueint);
            std::string audio_url = std::format("{}audio?recordId={}", OTA_URI, recordId);

            auto music = Board::GetInstance().GetMusic();
            music->PlayHdAudio(audio_url);
        }
    }

    cJSON_Delete(root);
}

/***********************************************************************************************
 * UDP接收任务：
 * 处理从妙月的天枢服务器接收到的音频数据
 ***********************************************************************************************/
void VoiceCall::udp_receive_task() {
    /*************************
     * 进行音频相关设置
     *************************/
    udp_receive_task_exit = false;

    auto& board = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    auto& application = Application::GetInstance();

    // 设置为待命状态
    Protocol* protocol = application.GetProtocol();
    if (protocol && protocol->IsAudioChannelOpened()) {
        protocol->CloseAudioChannel();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    application.SetDeviceState(kDeviceStateIdle);

    /*************************
     * 启用喇叭
     *************************/
    ESP_LOGI(TAG, "Configuring audio codec...");
    audio_codec->EnableOutput(true);
    application.GetAudioService().is_voice_out_ = true;
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Audio output enabled successfully");

    /*************************
     * 主循环
     *************************/
    long packets = 0;
    while (m_udp_receive_task_running) {
        from_len_ = sizeof(from_addr_);  // 每次调用前重置
        ssize_t recv_len = recvfrom(client_sock_, recv_buf_, MAX_PACKET_SIZE, 0, (struct sockaddr*)&from_addr_, &from_len_);

        if (recv_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));  // 非阻塞时短暂等待
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(10));  // 非阻塞时短暂等待
            continue;
        }

        packets++;
        if (packets % 50 == 0) {
            ESP_LOGI(TAG, "udp_receive_task receive UDP packet %d", packets);
        }

        // 解析数据包头部和负载
        CustomHeader header;
        uint8_t* payload = nullptr;
        if (!parse_packet(recv_buf_, recv_len, &header, &payload)) {
            ESP_LOGW(TAG, "Failed to parse packet");
            continue;
        }

        // 仅处理音频数据（payload_type == 1）
        if (header.payload_type != 1) {
            continue;
        }

        // 构造 AudioStreamPacket 对象
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = header.sample_rate;
        // 注意：timestamp 在协议中是 uint64_t 微秒，AudioStreamPacket 中为 uint32_t，
        // 这里取低 32 位或根据实际定义调整
        packet->timestamp = (uint32_t)header.timestamp;
        // frame_duration 可根据 payload_size、采样率、声道数等推算，此处设为默认 0
        packet->frame_duration = 20;
        // 复制负载数据
        packet->payload.assign(payload, payload + header.payload_size);

        application.GetAudioService().PushPacketToDecodeQueue(std::move(packet));
    }

    ESP_LOGI(TAG, "UDP receive task exiting");

    /*************************
     * 停止音频处理器
     *************************/
    // audio_codec->EnableOutput(false);
    application.GetAudioService().is_voice_out_ = false;

    /*************************
     * 进行音频相关设置
     *************************/
    udp_receive_task_exit = true;
    while (true) vTaskDelay(portMAX_DELAY);  // 防止任务退出
}

/*********************************************
 * 通过UDP发送音频数据到妙月的天枢服务器，
 * 重用板级的音频采集、opus编码等。
 *********************************************/
void VoiceCall::udp_send_task() {
    /***********************
     * 设置音频相关变量
     ***********************/
    udp_send_task_exit = false;

    auto& board = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    auto& application = Application::GetInstance();

    // 设置为待命状态
    Protocol* protocol = application.GetProtocol();
    if (protocol && protocol->IsAudioChannelOpened()) {
        protocol->CloseAudioChannel();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    application.SetDeviceState(kDeviceStateIdle);

    /***********************
     * 启用麦克风
     ***********************/
    ESP_LOGI(TAG, "Configuring audio codec...");
    audio_codec->EnableInput(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Audio input enabled successfully");

    /******************************
     * 关闭唤醒词，启动语音
     ******************************/
    application.GetAudioService().EnableWakeWordDetection(false);
    application.GetAudioService().EnableVoiceProcessing(true);

    /***********************
     * 主循环
     ***********************/
    server_addr_.sin_family = AF_INET;
    server_addr_.sin_addr.s_addr = inet_addr(m_call_info.server_ip.c_str());
    server_addr_.sin_port = htons(m_call_info.server_port);
    inet_pton(AF_INET, m_call_info.server_ip.c_str(), &server_addr_.sin_addr);

    long packets = 0;
    while (m_udp_send_task_running) {
        auto bits = xEventGroupWaitBits(event_group_, WEBSOCKET_SEND_AUDIO, pdTRUE, pdFALSE, pdMS_TO_TICKS(20));

        if (bits & WEBSOCKET_SEND_AUDIO) {
            while (auto packet = application.GetAudioService().PopPacketFromSendQueue()) {
                send_audio_frame(client_sock_, packet->sample_rate, 1, packet->timestamp, packet->payload.data(), packet->payload.size());

                // 每发送一帧后稍微喘息，让网络栈有时间处理
                vTaskDelay(pdMS_TO_TICKS(1));

                packets++;
                if (packets % 50 == 0) {
                    ESP_LOGI(TAG, "udp_send_task send UDP packet %d", packets);
                }
            }
        } 
    }

    ESP_LOGI(TAG, "UDP sending task exiting");

    /***********************
     * 停止音频处理器
     ***********************/
    audio_codec->EnableInput(false);
    vTaskDelay(pdMS_TO_TICKS(200));

    /******************************
     * 启动唤醒词，关闭语音
     ******************************/
    application.GetAudioService().EnableWakeWordDetection(true);
    application.GetAudioService().EnableVoiceProcessing(false);

    /***********************
     * 设置音频相关变量
     ***********************/
    udp_send_task_exit = true;
    while (true) vTaskDelay(portMAX_DELAY);  // 防止任务退出
}

/*****************************************************************
 * 当设备的网络断开时，在web socket重连失败的情况下，
 * 主动结束通话
*****************************************************************/
void VoiceCall::call_end() {
    ESP_LOGI(TAG, "主动结束通话");

    // 停止音频处理
    stop_audio_processing();
    
    // 重置通话信息
    reset_call_state();

    // 返回通话结束应答
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "device_id", m_device_info.device_id.c_str());
    // cJSON_AddStringToObject(response, "call_id", m_call_info.call_id.c_str());  
    char* response_payload = cJSON_PrintUnformatted(response);

    std::string device_call_end_ack = MQTT_TOPIC_CALL_END_ACK;
    send_mqtt_message(device_call_end_ack.c_str(), response_payload);

    cJSON_Delete(response);
    free(response_payload);
}

void VoiceCall::reset_call_state() {
    ESP_LOGI(TAG, "Resetting call state to idle");
    
    // 正确重置 call_info_t 中的每个成员
    m_call_info.state = CALL_STATE_IDLE;
}

void VoiceCall::regenerate_verification_code() {
    m_device_info.verification_code = generate_verification_code(6);
    m_device_info.verification_code_expiry = get_current_time() + 600; // 10分钟
    ESP_LOGI(TAG, "Verification code regenerated: %s",  m_device_info.verification_code.c_str());
}

void VoiceCall::send_audio_frame(int sockfd, uint32_t sample_rate, uint8_t  channels, uint64_t timestamp, const uint8_t* audio_data, size_t audio_size) {
    CustomHeader header = {
        .mac = {0 }, // 替换设备MAC地址
        .token = {0 },
        .magic = 0xABCD1234,
        .version = 0x0102,
        .payload_type = 1, // 音频
        .codec_id = CODEC_ID_OPUS,     // OPUS
        .sample_rate = sample_rate,
        .channels = channels,
        .timestamp = timestamp,
        .payload_size = audio_size,
        .header_size = CUSTOM_HEADER_SIZE
    };

    // 赋值设备的MAC
    std::string device_id = m_device_info.device_id;
    device_id.erase(std::remove(device_id.begin(), device_id.end(), ':'), device_id.end());

    unsigned long long number = std::stoull(device_id, nullptr, 16);

    for (int i = 0; i < 6; ++i) {
        // 提取第 i 个字节（从高位到低位）
        header.mac[i] = (number >> (8 * (5 - i))) & 0xFF;
    }

    // memcpy(header.token, m_call_info.roomToken.c_str(), m_call_info.roomToken.size());

    send_packet(sockfd, &header, audio_data, audio_size);
}

void VoiceCall::send_packet(int sockfd, const CustomHeader* header, const uint8_t* rtp_data, int data_len) {

    // if (!g_client_addr_valid) {
    //     return;
    // }

    // 计算需要分多少片
    uint8_t total_frags = (data_len + MAX_DATA_PER_PACKET - 1) / MAX_DATA_PER_PACKET;
    // pthread_mutex_lock(&queue_mutex_);
    for (int frag_idx = 0; frag_idx < total_frags; ++frag_idx) {
        // 1. 序列化头部（主机字节序 -> 网络字节序）
        CustomHeader net_header = *header;
        net_header.mac[0] = header->mac[0];
        net_header.mac[1] = header->mac[1];
        net_header.mac[2] = header->mac[2];
        net_header.mac[3] = header->mac[3];
        net_header.mac[4] = header->mac[4];
        net_header.mac[5] = header->mac[5];
        memcpy(net_header.token, header->token, CUSTOM_TOKEN_LEN);
        net_header.magic = htonl(header->magic);
        net_header.version = htons(header->version);
        net_header.width = htons(header->width);
        net_header.height = htons(header->height);
        net_header.sample_rate = htonl(header->sample_rate);
        net_header.timestamp = htonll(header->timestamp); // 需自定义htonll函数
        net_header.total_frags = total_frags;
        net_header.current_frag = frag_idx;

        // 修改 payload_size 为当前分片实际数据长度
        int data_offset = frag_idx * MAX_DATA_PER_PACKET;
        int frag_data_len = (frag_idx == total_frags - 1) ? 
                          (data_len - data_offset) : MAX_DATA_PER_PACKET;
        net_header.payload_size = htonl(frag_data_len);
        net_header.header_size = htons(header->header_size);

        char send_buf[MAX_PACKET_SIZE];
        memcpy(send_buf, &net_header, sizeof(net_header));
        memcpy(send_buf + CUSTOM_HEADER_SIZE, 
            rtp_data + data_offset, frag_data_len);

        // 4. 发送单个分片
        int actual_len = CUSTOM_HEADER_SIZE + frag_data_len;
        int ret = sendto(sockfd, send_buf, actual_len, 0, (struct sockaddr*)&server_addr_, sizeof(server_addr_));

        if (ret < 0) {
            perror("send fail");
        } else {
            if (net_header.payload_type == 0) {
                //printf("Sent frag %d/%d, size=%d\n", frag_idx+1, total_frags, ret);
            }
        }
    }
    // pthread_mutex_unlock(&queue_mutex_);
}

/* 解析数据包（线程安全）*/
// 返回值：true=解析成功，false=数据不完整或非法
bool VoiceCall::parse_packet(const uint8_t* data, size_t len, CustomHeader* header, uint8_t** payload) {
    // 1. 基础长度校验（必须包含两个头部）
    if (len < CUSTOM_HEADER_SIZE) {
        fprintf(stdin, "Packet too small: %zu < %d\n", 
               len, CUSTOM_HEADER_SIZE);
        return false;
    }
    // 1. 拷贝头部数据
    header->mac[0] = *(uint8_t*)(data + 0);
    header->mac[1] = *(uint8_t*)(data + 1);
    header->mac[2] = *(uint8_t*)(data + 2);
    header->mac[3] = *(uint8_t*)(data + 3);
    header->mac[4] = *(uint8_t*)(data + 4);
    header->mac[5] = *(uint8_t*)(data + 5);
    memcpy(header->token, data + 6, CUSTOM_TOKEN_LEN);
    header->magic = *(uint32_t*)(data + 18);
    header->version = *(uint16_t*)(data + 22);
    header->payload_type = *(uint8_t*)(data + 24);
    header->codec_id = *(uint8_t*)(data + 25);
    header->width = *(uint16_t*)(data + 26);
    header->height = *(uint16_t*)(data + 28);
    header->sample_rate = *(uint32_t*)(data + 30);
    header->channels = *(uint8_t*)(data + 34);
    header->reserved = *(uint8_t*)(data + 35);
    header->timestamp = *(uint64_t*)(data + 36);
    header->payload_size = *(uint32_t*)(data + 44);
    header->total_frags = *(uint8_t*)(data + 48);
    header->current_frag = *(uint8_t*)(data + 49);
    header->header_size = *(uint16_t*)(data + 50);

    // 2. 字节序转换（网络 -> 主机）
    header->magic = ntohl(header->magic);
    header->version = ntohs(header->version);
    header->width = ntohs(header->width);
    header->height = ntohs(header->height);
    header->sample_rate = ntohl(header->sample_rate);
    header->timestamp = ntohll(header->timestamp);
    header->payload_size = ntohl(header->payload_size);
    header->header_size = ntohs(header->header_size);
    // 3. 校验魔数和长度
    if (header->magic != 0xABCD1234 || header->header_size != CUSTOM_HEADER_SIZE) {
        return false;
    }

    // 6. 提取RTP负载
    *payload = (uint8_t*)(data + CUSTOM_HEADER_SIZE);
    return true;
}
