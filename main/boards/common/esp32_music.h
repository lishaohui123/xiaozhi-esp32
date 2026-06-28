#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "http_client.h"
#include "protocol.h"
#include "music.h"

// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t *d, size_t s) : data(d), size(s) {}
};

struct AudioInfo {
    std::string id;
    std::string play_url;
    int status;
};

struct WorkInfo {
    uint64_t id;
    std::string deviceId;
    std::string workId;
    std::string audioId;
    uint32_t completed;
};

struct categorie {
    uint64_t id;
    std::string name;
};

struct works {
    std::string id;
    std::string name;
};

// 音频块内存池结构
struct AudioChunkPoolItem {
    uint8_t* data;            // 数据指针
    size_t capacity;          // 容量
    size_t used_size;         // 已使用大小
    bool in_use;              // 是否在使用中
    uint32_t timestamp;       // 分配时间戳（用于监控）
};

// 音频包内存池结构
struct AudioPacketPoolItem {
    AudioStreamPacket packet; // 音频包对象
    bool in_use;              // 是否在使用中
    uint32_t timestamp;       // 分配时间戳
};

// 事件组
#define PLAY_EVENT_SCHEDULE (1 << 0)
#define WIFI_EVENT_SCHEDULE (1 << 1)

class Esp32Music : public Music {
public:
    Esp32Music();
    ~Esp32Music();

    // 共用函数
    std::string urlencode(const std::string &value);

    // 作品相关的一些接口函数
    bool FindWork(const std::string& work_name);
	WorkInfo GetWorkInfo(const std::string &work_id);
    std::string ParseWorkId();

    bool FindWorkDetail(const std::string& work_id);
    std::vector<AudioInfo> ParseWorkDetail();
    
    int InsertWorkInfo(WorkInfo workInfo);
    int UpdateWorkInfo(WorkInfo workInfo);
    int deleteWorkInfo(WorkInfo workInfo);

	// work and audio paly
    void PlayAudiosImpl(const std::string& work_id, std::vector<AudioInfo> audio_infos, int32_t cur);
    void PlayAudioImpl(const std::string& music_url);

	// audio paly functions
    int FindMp3SyncWordRobust(uint8_t* data, int data_size);
    bool RecoverFromDecodeError(uint8_t*& data, int* data_size);
    bool ProcessAndSendPCMData(int16_t* pcm_data, int sample_count);

    void ForceStopAllTasks();

    bool IsPlaying();

	// impliments functions
    virtual bool Download(const std::string& song_name, bool restart) override;
    virtual bool Play() override;
    virtual bool Play(const std::string& music_url, int repeat) override;
    virtual bool PlayVoice(const std::string& music_url, int repeat) override;
    virtual bool PlayHdAudio(const std::string &audio_url)override;
    virtual bool Play2(const std::string &music_url) override;
    virtual bool Stop() override;
    virtual std::string GetDownloadResult() override;
    virtual std::string GetWorkName() override;
    virtual std::string getStoryCategoriesWithExamples() override;
    virtual std::string getSongCategoriesWithExamples() override;
    virtual bool PlayRandomByCategory(std::string content) override;
    
    // 新增方法
    virtual void PlayAudioStream();
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming() override;  // 停止流式播放
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual bool IsPlaying() const override { return is_playing_; }

private:
    std::string last_downloaded_data_;
    std::string work_name_;
    std::string current_music_url_;
    std::string current_song_name_;
    bool song_name_displayed_;
    
    // 歌词相关
    std::string current_lyric_url_;
    std::vector<std::pair<int, std::string>> lyrics_;  // 时间戳和歌词文本
    std::mutex lyrics_mutex_;  // 保护lyrics_数组的互斥锁
    std::atomic<int> current_lyric_index_;
    std::atomic<bool> is_lyric_running_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::atomic<int> is_state_completed_;
    std::atomic<int> is_play_audios_status_;
    int64_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int64_t last_frame_time_ms_;    // 上一帧的时间戳
    int total_frames_decoded_;      // 已解码的帧数

    // 任务线程句柄
    TaskHandle_t download_task_handle_ = nullptr;
    TaskHandle_t play_task_handle_ = nullptr;
    TaskHandle_t play_audios_task_handle_ = nullptr;
    TaskHandle_t play_audio_task_handle_ = nullptr;
    TaskHandle_t lyric_task_handle_ = nullptr;

    // 静态任务函数
    static void DownloadAudioStreamTask(void* arg);
    static void PlayAudioStreamTask(void* arg);
    static void PlayAudiosTask(void* arg);
    static void PlayAudioTask(void* arg);
    static void LyricDisplayTask(void* arg);

    // 播放作品的线程
    StackType_t* play_audios_task_stack_;
    StaticTask_t* play_audios_task_tcb_;
    bool play_audios_task_static_allocated_;

    // 播放音频的线程
    StackType_t* play_audio_task_stack_;
    StaticTask_t* play_audio_task_tcb_;
    bool play_audio_task_static_allocated_;

    volatile std::atomic<bool> is_downloaded;
    volatile std::atomic<bool> is_played;

    // 静态play_task任务分配相关成员
    static constexpr uint32_t PLAY_TASK_STACK_SIZE = 1024 * 8;  // 8KB栈空间
    StackType_t* play_task_stack_ = nullptr;                    // 任务栈指针
    StaticTask_t* play_task_tcb_ = nullptr;                     // 任务控制块指针
    bool play_task_static_allocated_ = false;                   // 标记是否已静态分配

    // 静态download_task任务分配相关成员
    static constexpr uint32_t DOWNLOAD_TASK_STACK_SIZE = 1024 * 8;  // 8KB栈空间
    StackType_t* download_task_stack_ = nullptr;                    // 任务栈指针
    StaticTask_t* download_task_tcb_ = nullptr;                     // 任务控制块指针
    bool download_task_static_allocated_ = false;                   // 标记是否已静态分配

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    std::atomic<long> buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 512 * 1024;  // 512KB缓冲区
    static constexpr size_t MIN_BUFFER_SIZE = 1 * 1024;   // 64KB最小播放缓冲
    
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;
    
    // 私有方法
    void DownloadAudioStreamImpl(const std::string& music_url);
    void PlayAudioStreamImpl();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();  // 重置采样率到原始值
    bool ValidateMp3Data(const uint8_t* data, size_t size);
    void ResetMp3Decoder();
    bool RecoverFromDecodeError(uint8_t*& data, int& data_size);
    bool IsLikelyValidMp3Header(uint8_t* data, int data_size);
    
    // 歌词相关私有方法
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);
    
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    std::vector<categorie> GetCategories(long catId);
    std::vector<works> GetWorksByCategory(long storyCategoryId, int maxExamples);
    std::string GetWorkByCategory(long storyCategoryId);

    void StopWorksAudio();

        // 🔥 添加以下成员变量用于统计
    long total_downloaded_in_mp3_bytes = 0;  // 实际下载的MP3字节数
    long total_mp3_data_consumed = 0;        // 实际解码消耗的MP3字节数
    std::mutex stats_mutex_;                 // 用于保护统计变量的互斥锁



private:
    // 内存池配置
    static constexpr size_t MAX_CONCURRENT_CHUNKS = 128;          // 最大并发音频块数
    static constexpr size_t MAX_CONCURRENT_PACKETS = 64;         // 最大并发音频包数
    static constexpr size_t DEFAULT_CHUNK_SIZE = 1024;           // 默认音频块大小
    static constexpr size_t MP3_INPUT_BUFFER_SIZE = 8192;        // MP3输入缓冲区大小
    static constexpr size_t PCM_BUFFER_SIZE = 2304 * 2 * 2;          // PCM缓冲区大小(最大MP3帧2倍)
    
    // 成员变量
    AudioChunkPoolItem audio_chunk_pool_[MAX_CONCURRENT_CHUNKS];  // 音频块内存池
    AudioPacketPoolItem audio_packet_pool_[MAX_CONCURRENT_PACKETS]; // 音频包内存池
    uint8_t mp3_input_buffer_[MP3_INPUT_BUFFER_SIZE];             // 静态MP3输入缓冲区
    int16_t pcm_buffer_[PCM_BUFFER_SIZE];                         // 静态PCM缓冲区
    int16_t mono_buffer_[PCM_BUFFER_SIZE / 2];                    // 静态单声道缓冲区
    char download_buffer_[DEFAULT_CHUNK_SIZE];  // 静态下载缓冲区
    
    // 内存池管理变量
    size_t chunk_pool_index_;             // 音频块池索引（循环使用）
    size_t packet_pool_index_;            // 音频包池索引（循环使用）
    std::mutex pool_mutex_;               // 内存池互斥锁
    bool memory_pool_initialized_;        // 内存池初始化标志
    
        // 内存池统计
    static constexpr size_t MAX_CHUNK_SIZE = 16 * 1024;  // 16KB最大块大小
    size_t total_chunks_allocated_ = 0;
    size_t chunks_reclaimed_ = 0;
    size_t bytes_allocated_ = 0;


    // 内存池管理方法
    void InitializeMemoryPool();
    void CleanupMemoryPool();
    uint8_t* AllocateAudioChunk(size_t size);
    void FreeAudioChunk(uint8_t* data);
    AudioStreamPacket* AllocateAudioPacket();
    void FreeAudioPacket(AudioStreamPacket* packet);
    void ResetMemoryPool();
    void LogMemoryPoolStatus();

private:
        // 事件组相关
    EventGroupHandle_t streaming_event_group_;

    EventGroupHandle_t event_group_ = nullptr;
    
    // 事件位定义
    static constexpr EventBits_t EVENT_DOWNLOAD_COMPLETE = (1 << 0);
    static constexpr EventBits_t EVENT_PLAY_COMPLETE = (1 << 1);
    static constexpr EventBits_t EVENT_STREAMING_ERROR = (1 << 2);
    static constexpr EventBits_t EVENT_STREAMING_STOPPED = (1 << 3);
    
    // 所有完成位
    static constexpr EventBits_t EVENT_ALL_COMPLETE = 
        EVENT_DOWNLOAD_COMPLETE | EVENT_PLAY_COMPLETE;
    
    // 任何终止位
    static constexpr EventBits_t EVENT_ANY_TERMINATION = 
        EVENT_STREAMING_ERROR | EVENT_STREAMING_STOPPED;
    
    // 所有等待位
    static constexpr EventBits_t EVENT_WAIT_ALL = 
        EVENT_ALL_COMPLETE | EVENT_ANY_TERMINATION;

    std::shared_ptr<HttpClient> http_client_; // 复用 HTTP 客户端
                                              
    // 流式播放结果
    enum class StreamingResult {
        SUCCESS,
        ERROR,
        STOPPED,
        TIMEOUT
    };

    // 内部辅助函数
    void resetStreamingEvents();
    StreamingResult waitForStreamingCompletion(uint32_t timeout_ms);
    void signalDownloadComplete();
    void signalPlayComplete();
    void signalStreamingError();
    void signalStreamingStopped();

    void resetEvents();
    void signalDownload();
    void signalWIFIError();
};

#endif // ESP32_MUSIC_H