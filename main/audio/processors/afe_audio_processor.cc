#include "afe_audio_processor.h"
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

#if 0  // 原有代码，支持2通道
void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;
#if 0
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
#endif
    std::string input_format = "MR";

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 800;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    
    // 使用静态任务创建方式
    const size_t stack_size = 1024 * 8;
    
    // 分配任务栈内存
    task_stack_ = (StackType_t*)heap_caps_malloc(stack_size * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    assert(task_stack_ != nullptr);
    
    // 分配任务控制块内存
    task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    assert(task_buffer_ != nullptr);
    
    // 创建静态任务
    task_handle_ = xTaskCreateStatic(
        [](void* arg) {
            auto this_ = (AfeAudioProcessor*)arg;
            this_->AudioProcessorTask();
            vTaskDelete(NULL);
        },
        "audio_communication",
        stack_size,
        this,
        3,
        task_stack_,
        task_buffer_
    );

    assert(task_handle_ != nullptr);
}
#endif

#if 1  // 支持2通道，增加了更多的功能
void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;
#if 0
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
#endif
    std::string input_format = "MR";

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 2000;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = true;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    /******************************************
     * 为了让小智的麦克风更加的灵敏，增加了下面的功能设置
     * 补充：
     * 没有启用双麦克风波束成形，现在采用的是MR
     ******************************************/
    // 禁用唤醒词检测（重要！）
    afe_config->wakenet_init = false;
    afe_config->wakenet_model_name = NULL;

    // 启用自动增益控制（AGC）并调高增益
    afe_config->agc_init = true;
    afe_config->agc_mode = AFE_AGC_MODE_WEBRTC;      // 使用 WebRTC AGC
    afe_config->agc_compression_gain_db = 15;        // 提高增益（默认9，远场可12~18）
    afe_config->agc_target_level_dbfs = -6;           // 目标电平 -3 dBFS

    // 噪声抑制：改用温和的 WebRTC NS，避免神经网络抑制弱语音
    afe_config->ns_init = true;
    afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    afe_config->ns_model_name = NULL;

    // 调整 VAD 参数，降低远场语音触发的门槛
    afe_config->vad_min_speech_ms = 64;   // 语音最短持续150ms，避免短语音被丢弃
    afe_config->vad_min_noise_ms = 2000;
    afe_config->vad_delay_ms = 1200;        // 延迟200ms，让VAD能缓存语音开头

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    
    // 使用静态任务创建方式
    const size_t stack_size = 1024 * 32;
    
    // 分配任务栈内存
    task_stack_ = (StackType_t*)heap_caps_malloc(stack_size * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    assert(task_stack_ != nullptr);
    
    // 分配任务控制块内存
    task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    assert(task_buffer_ != nullptr);
    
    // 创建静态任务
    task_handle_ = xTaskCreateStatic(
        [](void* arg) {
            auto this_ = (AfeAudioProcessor*)arg;
            this_->AudioProcessorTask();
            vTaskDelete(NULL);
        },
        "audio_communication",
        stack_size,
        this,
        3,
        task_stack_,
        task_buffer_
    );

    assert(task_handle_ != nullptr);
}
#endif


AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    
    // 释放静态任务相关内存
    if (task_stack_ != nullptr) {
        heap_caps_free(task_stack_);
        task_stack_ = nullptr;
    }
    
    if (task_buffer_ != nullptr) {
        heap_caps_free(task_buffer_);
        task_buffer_ = nullptr;
    }
    
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
                // 为了防止采集过快，后续任务不能及时发送，这里加延时
                // vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->enable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}

void AfeAudioProcessor::Reset() {
    if (afe_data_) {
        afe_iface_->reset_buffer(afe_data_);
        ESP_LOGI(TAG, "AFE audio processor buffer reset");
    }
}
