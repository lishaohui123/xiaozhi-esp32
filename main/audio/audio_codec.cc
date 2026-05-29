#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

void AudioCodec::DynamicAGCWithNoiseSuppression(int16_t* data, int size) {
    if (size <= 0) return;
    
    // 1. 应用噪声抑制
    // noise_suppressor_.ProcessWithNoiseSuppression(data, size);
    
    // 2. 应用最终增益
    float gain = input_gain_/ 100.0f;
    for (int i = 0; i < size; i++) {
        int32_t sample = static_cast<int32_t>(data[i] * gain);
        
        // 软限制器
        if (sample > 32767) {
            sample = 32767 - (sample - 32767) / 16;  // 更柔和的限制
        }
        if (sample < -32768) {
            sample = -32768 - (sample + 32768) / 16;
        }
        
        data[i] = static_cast<int16_t>(sample);
    }
}

void AudioCodec::DynamicAGC(int16_t* data, int size) {
    DynamicAGCWithNoiseSuppression(data, size);
}

void AudioCodec::ApplyOutputGain(int16_t* data, int samples) {
    float gain = output_gain_ / 100.0f; // 假设output_gain_是百分比
    for (int i = 0; i < samples; i++) {
        int32_t sample = data[i] * gain;

        // 防止溢出
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        data[i] = (int16_t)sample;
    }
}


AudioCodec::AudioCodec() : noise_suppressor_(16000, 320) {
    input_gain_ = 300.0;
    output_gain_ = 300.0;
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    ApplyOutputGain(data.data(), data.size());
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        // 在读取音频数据后应用 AGC
        DynamicAGC(data.data(), samples);
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    }

    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    }

    EnableInput(true);
    EnableOutput(true);
    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

#if 1
    // 当输出状态改变时，通知上层（比如重置AFE）
    if (hw_state_change_callback_) {
        hw_state_change_callback_();
    }
#endif
    
#if 0 // lsh    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
#endif
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");

#if 0
    // 当输出状态改变时，通知上层（比如重置AFE）
    if (hw_state_change_callback_) {
        hw_state_change_callback_();
    }
#endif
}
