#include <math.h>
#include <algorithm>
#include <vector>

#include "AudioNoiseSuppressor.h"

AudioNoiseSuppressor::AudioNoiseSuppressor(int sample_rate, int frame_size)
    : noise_estimate_(0.0f),
      noise_variance_(0.0f),
      signal_estimate_(0.0f),
      noise_initialized_(false),
      frames_count_(0),
      alpha_slow_(0.99f),      // const成员必须在初始化列表初始化
      alpha_fast_(0.9f),       // const成员必须在初始化列表初始化
      noise_floor_(0.01f),     // const成员必须在初始化列表初始化
      snr_threshold_(2.0f),    // const成员必须在初始化列表初始化
      min_gain_(0.1f),         // const成员必须在初始化列表初始化
      max_gain_(5.0f),         // const成员必须在初始化列表初始化
      fft_size_(512),          // 现在按照声明顺序，在sample_rate之前
      sample_rate_(sample_rate),
      fir_length_(32),
      mu_(0.01f) {
    
    // 初始化频谱估计向量（依赖于fft_size_，所以必须在构造函数体中）
    noise_spectrum_.resize(fft_size_ / 2 + 1, 0.0f);
    signal_spectrum_.resize(fft_size_ / 2 + 1, 0.0f);
    smoothed_spectrum_.resize(fft_size_ / 2 + 1, 0.0f);
    
    // 初始化自适应滤波器向量
    fir_coeffs_.resize(fir_length_, 0.0f);
    fir_buffer_.resize(fir_length_, 0.0f);
    fir_coeffs_[fir_length_ / 2] = 1.0f;  // 初始化为单位脉冲
}
  
// 改进的动态AGC，集成噪声抑制
void AudioNoiseSuppressor::ProcessWithNoiseSuppression(int16_t* data, int size) {
    if (size <= 0) return;
    
    // 转换为浮点数处理
    std::vector<float> float_data(size);
    for (int i = 0; i < size; i++) {
        float_data[i] = data[i] / 32768.0f;
    }
    
    // 1. 应用自适应滤波（回声和噪声抑制）
    AdaptiveFilter(float_data);
    
    // 2. 计算当前帧的统计信息
    float energy = 0.0f;
    float max_amplitude = 0.0f;
    for (int i = 0; i < size; i++) {
        float abs_val = fabsf(float_data[i]);
        energy += abs_val * abs_val;
        if (abs_val > max_amplitude) {
            max_amplitude = abs_val;
        }
    }
    energy = sqrtf(energy / size);  // RMS能量
    
    // 3. 更新噪声估计
    UpdateNoiseEstimate(energy, max_amplitude);
    
    // 4. 计算信噪比(SNR)
    float snr = 1.0f;
    if (noise_estimate_ > 0.0f) {
        snr = energy / (noise_estimate_ + 0.0001f);
    }
    
    // 5. 计算自适应增益
    float adaptive_gain = CalculateAdaptiveGain(snr, energy);
    
    // 6. 应用谱减法（可选，需要FFT）
    // SpectralSubtraction(float_data);
    
    // 7. 应用最终增益和限制器
    ApplyGainWithLimiter(float_data, adaptive_gain);
    
    // 8. 转换回int16_t
    for (int i = 0; i < size; i++) {
        int32_t sample = static_cast<int32_t>(float_data[i] * 32767.0f);
        
        // 软限制器，防止削波
        if (sample > 32767) {
            sample = 32767 - (sample - 32767) / 10;
        }
        if (sample < -32768) {
            sample = -32768 - (sample + 32768) / 10;
        }
        
        data[i] = static_cast<int16_t>(sample);
    }
}
    
// LMS自适应滤波器
void AudioNoiseSuppressor::AdaptiveFilter(std::vector<float>& signal) {
    if (signal.size() < fir_length_) return;
    
    std::vector<float> output(signal.size(), 0.0f);
    
    for (size_t n = 0; n < signal.size(); n++) {
        // 更新滤波器缓冲区
        std::rotate(fir_buffer_.rbegin(), fir_buffer_.rbegin() + 1, fir_buffer_.rend());
        fir_buffer_[0] = signal[n];
        
        // 计算滤波器输出
        float y = 0.0f;
        for (int i = 0; i < fir_length_; i++) {
            y += fir_coeffs_[i] * fir_buffer_[i];
        }
        
        // 误差信号
        float error = signal[n] - y;
        
        // LMS更新系数
        for (int i = 0; i < fir_length_; i++) {
            fir_coeffs_[i] += mu_ * error * fir_buffer_[i];
        }
        
        // 限制系数以防止发散
        float coeff_norm = 0.0f;
        for (int i = 0; i < fir_length_; i++) {
            coeff_norm += fir_coeffs_[i] * fir_coeffs_[i];
        }
        coeff_norm = sqrtf(coeff_norm);
        if (coeff_norm > 2.0f) {
            for (int i = 0; i < fir_length_; i++) {
                fir_coeffs_[i] *= 2.0f / coeff_norm;
            }
        }
        
        output[n] = y;
    }
    
    signal = std::move(output);
}
    
// 更新噪声估计
void AudioNoiseSuppressor::UpdateNoiseEstimate(float energy, float max_amplitude) {
    frames_count_++;
    
    // 前50帧用于初始化噪声估计
    if (frames_count_ < 50) {
        noise_estimate_ = (noise_estimate_ * (frames_count_ - 1) + energy) / frames_count_;
        noise_variance_ = (noise_variance_ * (frames_count_ - 1) + 
                            (energy - noise_estimate_) * (energy - noise_estimate_)) / frames_count_;
        
        if (frames_count_ == 50) {
            noise_initialized_ = true;
            signal_estimate_ = noise_estimate_ * snr_threshold_;
        }
        return;
    }
    
    if (!noise_initialized_) return;
    
    // VAD (Voice Activity Detection)
    bool is_speech = energy > (noise_estimate_ * snr_threshold_);
    
    // 更新噪声估计（仅在非语音时更新）
    if (!is_speech) {
        // 慢速更新噪声估计
        noise_estimate_ = alpha_slow_ * noise_estimate_ + (1.0f - alpha_slow_) * energy;
        
        // 更新噪声方差
        float diff = energy - noise_estimate_;
        noise_variance_ = alpha_slow_ * noise_variance_ + (1.0f - alpha_slow_) * diff * diff;
    }
    
    // 更新信号估计（仅在语音时更新）
    if (is_speech) {
        signal_estimate_ = alpha_fast_ * signal_estimate_ + (1.0f - alpha_fast_) * energy;
    }
}
    
// 计算自适应增益
float AudioNoiseSuppressor::CalculateAdaptiveGain(float snr, float energy) {
    float gain = 1.0f;
    
    if (noise_initialized_) {
        // 基于SNR的非线性增益控制
        if (snr < 1.0f) {
            // 低SNR，大幅衰减
            gain = min_gain_;
        } else if (snr < snr_threshold_) {
            // 中等SNR，平滑过渡
            gain = min_gain_ + (1.0f - min_gain_) * 
                    (1.0f - expf(-(snr - 1.0f) / (snr_threshold_ - 1.0f)));
        } else {
            // 高SNR，正常增益
            float target_gain = 2.0f;  // 基础增益
            
            // 根据信号强度微调增益
            if (signal_estimate_ > 0.0f) {
                float signal_ratio = energy / signal_estimate_;
                if (signal_ratio > 1.5f) {
                    target_gain *= 0.8f;  // 强信号，降低增益防止削波
                } else if (signal_ratio < 0.5f) {
                    target_gain *= 1.2f;  // 弱信号，提高增益
                }
            }
            
            gain = std::min(std::max(target_gain, min_gain_), max_gain_);
        }
        
        // 添加噪声基底，避免完全静音
        float noise_floor_gain = std::max(0.1f, noise_floor_ / (noise_estimate_ + 0.0001f));
        gain = std::max(gain, noise_floor_gain);
    }
    
    return gain;
}
    
// 谱减法（需要FFT库支持）
void AudioNoiseSuppressor::SpectralSubtraction(std::vector<float>& signal) {
    // 这里简化实现，实际需要FFT变换
    // 可以使用ESP-DSP库中的FFT函数
    
    /*
    // 伪代码：
    // 1. 对信号进行FFT变换
    // 2. 估计噪声谱
    // 3. 从信号谱中减去噪声谱
    // 4. 进行IFFT变换恢复时域信号
    */
}
    
// 应用增益和限制器
void AudioNoiseSuppressor::ApplyGainWithLimiter(std::vector<float>& signal, float gain) {
    const float threshold = 0.9f;  // 限制器阈值
    const float release_rate = 0.999f;  // 释放速率
    const float attack_rate = 0.99f;    // 攻击速率
    
    static float envelope = 0.0f;
    
    for (size_t i = 0; i < signal.size(); i++) {
        // 应用增益
        signal[i] *= gain;
        
        // 计算包络
        float abs_val = fabsf(signal[i]);
        if (abs_val > envelope) {
            envelope = attack_rate * envelope + (1.0f - attack_rate) * abs_val;
        } else {
            envelope = release_rate * envelope + (1.0f - release_rate) * abs_val;
        }
        
        // 应用软限制器
        if (envelope > threshold) {
            float limiter_gain = threshold / (envelope + 0.0001f);
            signal[i] *= limiter_gain;
            envelope *= limiter_gain;
        }
    }
}
    
// 简单的噪声门
void AudioNoiseSuppressor::ApplyNoiseGate(std::vector<float>& signal, float threshold) {
    // 先计算平均能量
    float energy = 0.0f;
    for (float sample : signal) {
        energy += fabsf(sample);
    }
    energy /= signal.size();
    
    // 如果能量低于阈值，衰减信号
    if (energy < threshold) {
        float attenuation = energy / threshold;
        attenuation = attenuation * attenuation;  // 平方衰减更自然
        for (float& sample : signal) {
            sample *= attenuation;
        }
    }
}
