#ifndef MUSIC_H
#define MUSIC_H

#include <string>

class Music {
public:
    virtual ~Music() = default;  // 添加虚析构函数
    
    virtual bool Download(const std::string& song_name, bool restart) = 0;
    virtual bool Play() = 0;
    virtual bool Play(const std::string& music_url, int repeat) = 0;
    virtual bool PlayVoice(const std::string& music_url, int repeat) = 0;
    virtual bool PlayHdAudio(const std::string &audio_url) = 0;
    virtual bool PlayTouchAudio(const std::string &touch_url) = 0;
    virtual bool Play2(const std::string& music_url) = 0;
    virtual bool Stop() = 0;
    virtual std::string GetDownloadResult() = 0;
    virtual std::string GetWorkName() = 0;
    virtual std::string getStoryCategoriesWithExamples() = 0;
    virtual std::string getSongCategoriesWithExamples() = 0;
    virtual bool PlayRandomByCategory(std::string content) = 0;
    
    // 新增流式播放相关方法
    virtual void PlayAudioStream() = 0;
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool StopStreaming() = 0;  // 停止流式播放
    virtual size_t GetBufferSize() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual bool IsPlaying() const = 0;
};

#endif // MUSIC_H 