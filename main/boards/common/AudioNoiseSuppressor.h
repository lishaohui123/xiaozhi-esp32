#ifndef AUDIO_NOISE_H
#define AUDIO_NOISE_H

#include <math.h>
#include <algorithm>
#include <vector>

// 噪声抑制类
class AudioNoiseSuppressor {
private:
    // 1. 噪声估计参数
    float noise_estimate_;
    float noise_variance_;
    float signal_estimate_;
    bool noise_initialized_;
    int frames_count_;
    
    // 2. 滤波器参数（const 成员必须在初始化列表中初始化）
    const float alpha_slow_;      // 慢速更新系数 (用于噪声估计)
    const float alpha_fast_;      // 快速更新系数 (用于信号估计)
    const float noise_floor_;     // 噪声基底
    const float snr_threshold_;   // SNR阈值
    const float min_gain_;        // 最小增益
    const float max_gain_;        // 最大增益
    
    // 3. 其他整型参数
    int fft_size_;
    int sample_rate_;
    int fir_length_;
    float mu_;  // LMS步长
    
    // 4. 谱减法参数
    std::vector<float> noise_spectrum_;
    std::vector<float> signal_spectrum_;
    std::vector<float> smoothed_spectrum_;
    
    // 5. 自适应滤波器状态
    std::vector<float> fir_coeffs_;
    std::vector<float> fir_buffer_;
    
public:
    AudioNoiseSuppressor(int sample_rate, int frame_size);
    
    // 改进的动态AGC，集成噪声抑制
    void ProcessWithNoiseSuppression(int16_t* data, int size);
    
private:
    // LMS自适应滤波器
    void AdaptiveFilter(std::vector<float>& signal);
    
    // 更新噪声估计
    void UpdateNoiseEstimate(float energy, float max_amplitude);
    
    // 计算自适应增益
    float CalculateAdaptiveGain(float snr, float energy);
    
    // 谱减法（需要FFT库支持）
    void SpectralSubtraction(std::vector<float>& signal);
    
    // 应用增益和限制器
    void ApplyGainWithLimiter(std::vector<float>& signal, float gain);
    
    // 简单的噪声门
    void ApplyNoiseGate(std::vector<float>& signal, float threshold);
};

#endif // AUDIO_NOISE_H
