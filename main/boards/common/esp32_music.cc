#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "gloable_var.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>  // 为isdigit函数
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include "constants.h"

#define TAG "Esp32Music"

// 创建任务参数结构
struct DownloadTaskParams {
    Esp32Music* self;
    std::string music_url;
};

// 创建任务参数结构
struct PlayAudiosTaskParams {
    Esp32Music* self;
    std::string work_id;
    std::vector<AudioInfo> audio_infos;
    int32_t cur;
};

struct PlayAudioTaskParam {
    Esp32Music* self;
    std::string music_url;
};

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

Esp32Music::Esp32Music() : 
    last_downloaded_data_(),
    work_name_(),
    current_music_url_(), 
    current_song_name_(),
    song_name_displayed_(false), 
    current_lyric_url_(), 
    lyrics_(), 
    lyrics_mutex_(),
    current_lyric_index_(-1), 
    is_lyric_running_(false),
    is_playing_(false), 
    is_downloading_(false), 
    is_state_completed_(0),
    is_play_audios_status_(0),
    current_play_time_ms_(0),
    last_frame_time_ms_(0),
    total_frames_decoded_(0),
    download_task_handle_(nullptr),
    play_task_handle_(nullptr),
    play_audios_task_handle_(nullptr),
    play_audio_task_handle_(nullptr),
    lyric_task_handle_(nullptr),
    play_audios_task_stack_(nullptr),
    play_audios_task_tcb_(nullptr),
    play_audios_task_static_allocated_(false),
    play_audio_task_stack_(nullptr),
    play_audio_task_tcb_(nullptr),
    play_audio_task_static_allocated_(false),
    is_downloaded(false),
    is_played(false),
    play_task_stack_(nullptr),
    play_task_tcb_(nullptr),
    play_task_static_allocated_(false),
    download_task_stack_(nullptr),
    download_task_tcb_(nullptr),
    download_task_static_allocated_(false),
    audio_buffer_(), 
    buffer_mutex_(), 
    buffer_cv_(), 
    buffer_size_(0), 
    mp3_decoder_(nullptr), 
    mp3_frame_info_(), 
    mp3_decoder_initialized_(false),
    streaming_event_group_(nullptr) {
    
    ESP_LOGI(TAG, "Music player initialized");
    
    // 初始化MP3解码器
    InitializeMp3Decoder();

    // 添加内存池初始化
    InitializeMemoryPool();

    // 创建事件组
    streaming_event_group_ = xEventGroupCreate();
    if (!streaming_event_group_) {
        ESP_LOGE(TAG, "Failed to create event group for streaming");
    }

    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(TAG, "Failed to create event group for streaming");
    }
}

Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    
    // 清理事件组
    if (streaming_event_group_) {
        vEventGroupDelete(streaming_event_group_);
        streaming_event_group_ = nullptr;
    }

    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }

    // 1. 先设置停止标志
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    is_play_audios_status_ = 0;
    is_state_completed_ = 2; // 标记为强制完成
    
    // 2. 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 3. 等待一段时间让任务有机会自然退出
    const int max_wait_ticks = 50; // 等待最多500ms
    for (int i = 0; i < max_wait_ticks; i++) {
        // 检查是否还有任务在运行
        bool tasks_running = false;
        if (download_task_handle_ != nullptr && 
            eTaskGetState(download_task_handle_) != eDeleted) {
            tasks_running = true;
        }
        if (play_task_handle_ != nullptr && 
            eTaskGetState(play_task_handle_) != eDeleted) {
            tasks_running = true;
        }
        if (play_audios_task_handle_ != nullptr && 
            eTaskGetState(play_audios_task_handle_) != eDeleted) {
            tasks_running = true;
        }
        if (play_audio_task_handle_ != nullptr && 
            eTaskGetState(play_audio_task_handle_) != eDeleted) {
            tasks_running = true;
        }
        if (lyric_task_handle_ != nullptr && 
            eTaskGetState(lyric_task_handle_) != eDeleted) {
            tasks_running = true;
        }
        
        if (!tasks_running) {
            ESP_LOGI(TAG, "All tasks stopped naturally after %d ms", i * 10);
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // 每次等待10ms
    }
    
    // 4. 强制删除所有任务（如果还在运行）
    // 注意：删除顺序应该是从依赖低的到依赖高的
    
    // 先删除播放音频任务
    if (play_audio_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Force deleting play_audio task");
        vTaskDelete(play_audio_task_handle_);
        play_audio_task_handle_ = nullptr;
        play_audio_task_static_allocated_ = false;
    }
    
    // 删除播放作品任务
    if (play_audios_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Force deleting play_audios task");
        vTaskDelete(play_audios_task_handle_);
        play_audios_task_handle_ = nullptr;
        play_audios_task_static_allocated_ = false;
    }
    
    // 删除歌词任务
    if (lyric_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Force deleting lyric task");
        vTaskDelete(lyric_task_handle_);
        lyric_task_handle_ = nullptr;
    }
    
    // 删除播放任务（依赖于下载任务的数据）
    if (play_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Force deleting play task");
        vTaskDelete(play_task_handle_);
        play_task_handle_ = nullptr;
        play_task_static_allocated_ = false;
    }
    
    // 最后删除下载任务
    if (download_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Force deleting download task");
        vTaskDelete(download_task_handle_);
        download_task_handle_ = nullptr;
        download_task_static_allocated_ = false;
    }
    
    // 5. 清理音频缓冲区（确保在任务删除后进行）
    ClearAudioBuffer();
    
    // 6. 清理MP3解码器
    CleanupMp3Decoder();
    
    // 7. 清理静态分配的任务内存（按照分配的反顺序释放）
#if 0
    // 清理播放音频任务内存
    if (play_audio_task_tcb_ != nullptr) {
        heap_caps_free(play_audio_task_tcb_);
        play_audio_task_tcb_ = nullptr;
        ESP_LOGI(TAG, "Play audio task TCB memory freed");
    }
    
    if (play_audio_task_stack_ != nullptr) {
        heap_caps_free(play_audio_task_stack_);
        play_audio_task_stack_ = nullptr;
        ESP_LOGI(TAG, "Play audio task stack memory freed");
    }
    
    // 清理播放作品任务内存
    if (play_audios_task_tcb_ != nullptr) {
        heap_caps_free(play_audios_task_tcb_);
        play_audios_task_tcb_ = nullptr;
        ESP_LOGI(TAG, "Play audios task TCB memory freed");
    }
    
    if (play_audios_task_stack_ != nullptr) {
        heap_caps_free(play_audios_task_stack_);
        play_audios_task_stack_ = nullptr;
        ESP_LOGI(TAG, "Play audios task stack memory freed");
    }
    
    // 清理播放任务内存
    if (play_task_tcb_ != nullptr) {
        heap_caps_free(play_task_tcb_);
        play_task_tcb_ = nullptr;
        ESP_LOGI(TAG, "Play task TCB memory freed");
    }
    
    if (play_task_stack_ != nullptr) {
        heap_caps_free(play_task_stack_);
        play_task_stack_ = nullptr;
        ESP_LOGI(TAG, "Play task stack memory freed");
    }
    
    // 清理下载任务内存
    if (download_task_tcb_ != nullptr) {
        heap_caps_free(download_task_tcb_);
        download_task_tcb_ = nullptr;
        ESP_LOGI(TAG, "Download task TCB memory freed");
    }
    
    if (download_task_stack_ != nullptr) {
        heap_caps_free(download_task_stack_);
        download_task_stack_ = nullptr;
        ESP_LOGI(TAG, "Download task stack memory freed");
    }
#endif  
    // 8. 清理条件变量和互斥锁（虽然不是必需的，但可以确保状态正确）
    // 注意：在C++中，std::condition_variable和std::mutex的析构函数会自动清理
    // 但我们需要确保没有线程在等待
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 9. 清理歌词相关资源
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        lyrics_.clear();
        current_lyric_index_ = -1;
    }
    
    // 10. 重置所有原子变量
    current_lyric_index_ = -1;
    is_state_completed_ = 0;
    is_play_audios_status_ = 0;
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    // 11. 清理字符串成员
    last_downloaded_data_.clear();
    work_name_.clear();
    current_music_url_.clear();
    current_song_name_.clear();
    current_lyric_url_.clear();
    
    // 12. 重置布尔标志
    song_name_displayed_ = false;
    mp3_decoder_initialized_ = false;
    is_downloaded = false;
    is_played = false;
    
    ESP_LOGI(TAG, "Music player destroyed successfully");
}

std::string Esp32Music::ParseWorkId() {
    std::string work_id_str;
    if (!last_downloaded_data_.empty()) {
        cJSON* response_json = cJSON_Parse(last_downloaded_data_.c_str());
        if (response_json) {
            // 检查retcode
            cJSON* retcode = cJSON_GetObjectItem(response_json, "retcode");
            if (cJSON_IsNumber(retcode) && retcode->valueint == 0) {
                // 提取id
                cJSON* data = cJSON_GetObjectItem(response_json, "data");
                if (data) {
                    cJSON* works = cJSON_GetObjectItem(data, "works");
                    if (works && cJSON_IsArray(works) && cJSON_GetArraySize(works) > 0) {
                        cJSON* first_work = cJSON_GetArrayItem(works, 0);
                        cJSON* work_id = cJSON_GetObjectItem(first_work, "id");
                        cJSON* work_name = cJSON_GetObjectItem(first_work, "name");
                        
                        if (cJSON_IsString(work_id) && work_id->valuestring) {
                            work_id_str = work_id->valuestring;
                            ESP_LOGI(TAG, "Successfully extracted work ID: %s", work_id_str.c_str());                            
                        }

                        work_name_.clear();
                        if (cJSON_IsString(work_name) && work_name->valuestring) {
                            work_name_ = work_name->valuestring;
                            ESP_LOGI(TAG, "Successfully extracted work name: %s", work_name_.c_str());                            
                        }

                        cJSON_Delete(response_json);
                        return work_id_str;
                    }
                }
            } else {
                ESP_LOGE(TAG, "API error, retcode: %d", cJSON_IsNumber(retcode) ? retcode->valueint : -1);
            }
            
            cJSON_Delete(response_json);
        }
    }
    return work_id_str;
}

bool Esp32Music::FindWork(const std::string& work_name) { 
    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/wechat/work/query?deviceId=" + GloableVar::device_id + "&workName=" + urlencode(work_name);
    } else {
        url += "wechat/work/query?deviceId=" + GloableVar::device_id + "&workName=" + urlencode(work_name);
    }

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive
    
    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return false;
    }

    // 检查响应状态码
    int status_code = http_client_->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http_client_->Close();
        return false;
    }

    // 获取内容长度（如果有的话）
    size_t content_length = http_client_->GetBodyLength();
    ESP_LOGI(TAG, "Content length: %u bytes", content_length);
    
    // 按照Upgrade函数的方式分块读取
    char buffer[512]; // 使用较小的缓冲区，与Upgrade函数保持一致
    std::string response_data;
    size_t total_read = 0;
    size_t recent_read = 0;
    int64_t last_calc_time = esp_timer_get_time();
    bool read_error = false;
    
    // 清空之前的数据
    last_downloaded_data_.clear();
    response_data.clear();

    // 开始读取循环
    while (true) {
        int ret = http_client_->Read(buffer, sizeof(buffer));
        
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %d", ret);
            read_error = true;
            break;
        }
        
        // 计算读取进度
        if (ret > 0) {
            response_data.append(buffer, ret);
            total_read += ret;
            recent_read += ret;
            
            // 每秒打印一次进度（避免日志过多）
            if (esp_timer_get_time() - last_calc_time >= 1000000) { // 1秒
                ESP_LOGD(TAG, "Progress: %u bytes read", total_read);
                last_calc_time = esp_timer_get_time();
                recent_read = 0;
            }
        }
        
        // 检查是否读取完成
        if (ret == 0) {
            ESP_LOGI(TAG, "HTTP read completed, total: %u bytes", total_read);
            break;
        }
    }
    
    http_client_->Close();

    // 保存数据
    last_downloaded_data_ = response_data;
    ESP_LOGI(TAG, "Successfully received and validated response, length: %u bytes", 
            last_downloaded_data_.length());
    ESP_LOGI(TAG, "response_data: %s", response_data.c_str());

    return true;
}

#include <cctype>
#include <iomanip>
#include <sstream>

std::string Esp32Music::urlencode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        // 保留字母数字字符和一些特殊字符
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            // 对其他字符进行百分比编码
            escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
        }
    }

    return escaped.str();
}

bool Esp32Music::FindWorkDetail(const std::string& work_id) {
    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/wechat/work/detail?deviceId=" + GloableVar::device_id + "&workId=" + work_id;
    } else {
        url += "wechat/work/detail?deviceId=" + GloableVar::device_id + "&workId=" + work_id;
    }

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return false;
    }

    // 检查响应状态码
    int status_code = http_client_->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http_client_->Close();
        return false;
    }

    // 获取内容长度（如果有的话）
    size_t content_length = http_client_->GetBodyLength();
    ESP_LOGI(TAG, "Content length: %u bytes", content_length);
    
    // 按照Upgrade函数的方式分块读取
    char buffer[512]; // 使用较小的缓冲区，与Upgrade函数保持一致
    std::string response_data;
    size_t total_read = 0;
    size_t recent_read = 0;
    int64_t last_calc_time = esp_timer_get_time();
    bool read_error = false;
    
    // 清空之前的数据
    last_downloaded_data_.clear();
    response_data.clear();

    // 开始读取循环
    while (true) {
        int ret = http_client_->Read(buffer, sizeof(buffer));
        
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %d", ret);
            read_error = true;
            break;
        }
        
        // 计算读取进度
        if (ret > 0) {
            response_data.append(buffer, ret);
            total_read += ret;
            recent_read += ret;
            
            // 每秒打印一次进度（避免日志过多）
            if (esp_timer_get_time() - last_calc_time >= 1000000) { // 1秒
                ESP_LOGD(TAG, "Progress: %u bytes read", total_read);
                last_calc_time = esp_timer_get_time();
                recent_read = 0;
            }
        }
        
        // 检查是否读取完成
        if (ret == 0) {
            ESP_LOGI(TAG, "HTTP read completed, total: %u bytes", total_read);
            break;
        }
    }
    
    http_client_->Close();

    // 保存数据
    last_downloaded_data_ = response_data;
    ESP_LOGI(TAG, "Successfully received and validated response, length: %u bytes", 
            last_downloaded_data_.length());
    ESP_LOGI(TAG, "response_data: %s", response_data.c_str());

    return true;
}

std::vector<AudioInfo> Esp32Music::ParseWorkDetail() {
    std::vector<AudioInfo> audios;
    
    cJSON* root = cJSON_Parse(last_downloaded_data_.c_str());
    if (!root) return audios;
    
    cJSON* retcode = cJSON_GetObjectItem(root, "retcode");
    if (cJSON_IsNumber(retcode) && retcode->valueint == 0) {
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (data) {
            cJSON* works = cJSON_GetObjectItem(data, "works");
            if (works && cJSON_IsArray(works)) {
                for (int i = 0; i < cJSON_GetArraySize(works); i++) {
                    cJSON* work = cJSON_GetArrayItem(works, i);
                    cJSON* work_audios = cJSON_GetObjectItem(work, "audios");
                    
                    if (work_audios && cJSON_IsArray(work_audios)) {
                        for (int j = 0; j < cJSON_GetArraySize(work_audios); j++) {
                            cJSON* audio = cJSON_GetArrayItem(work_audios, j);
                            
                            AudioInfo info;
                            cJSON* id = cJSON_GetObjectItem(audio, "id");
                            cJSON* url = cJSON_GetObjectItem(audio, "play_url");
                            cJSON* status = cJSON_GetObjectItem(audio, "status");
                            
                            if (cJSON_IsString(id)) info.id = id->valuestring;
                            if (cJSON_IsString(url)) info.play_url = url->valuestring;
                            if (cJSON_IsString(status)) info.status = url->valueint;
                            
                            audios.push_back(info);
                        }
                    }
                }
            }
        }
    }
    else {
        ESP_LOGE(TAG, "API error, retcode: %d", cJSON_IsNumber(retcode) ? retcode->valueint : -1);
    }
    
    cJSON_Delete(root);
    return audios;
}

WorkInfo Esp32Music::GetWorkInfo(const std::string& work_id) {
    WorkInfo workInfo;

    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/work/" + GloableVar::device_id + "/" + work_id;
    } else {
        url += "work/" + GloableVar::device_id + "/" + work_id;
    }

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return workInfo;
    }

    auto status_code = http_client_->GetStatusCode();
    if (status_code == 202) {
        return workInfo;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d", status_code);
        return workInfo;
    }

    std::string data = http_client_->ReadAll();
    http_client_->Close();
    ESP_LOGI(TAG, "response=%s", data.c_str());

    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return workInfo;
    }

    // 解析id
    cJSON* id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id) && id->valuestring) {
        workInfo.id = id->valueint;
    }

    // 解析deviceId
    cJSON* deviceId = cJSON_GetObjectItem(root, "deviceId");
    if (cJSON_IsString(deviceId) && deviceId->valuestring) {
        workInfo.deviceId = deviceId->valuestring;
    }

    // 解析workId
    cJSON* workId = cJSON_GetObjectItem(root, "workId");
    if (cJSON_IsString(workId) && workId->valuestring) {
        workInfo.workId = workId->valuestring;
    }

    // 解析audioId
    cJSON* audioId = cJSON_GetObjectItem(root, "audioId");
    if (cJSON_IsString(audioId) && audioId->valuestring) {
        workInfo.audioId = audioId->valuestring;
    }

    // 解析completed
    cJSON* completed = cJSON_GetObjectItem(root, "completed");
    if (cJSON_IsString(completed) && completed->valuestring) {
        workInfo.completed = completed->valueint;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Get work info successful");
    return workInfo;
}

int Esp32Music::InsertWorkInfo(WorkInfo workInfo) {
    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/work/insert";
    } else {
        url += "work/insert";
    }

    std::string json = "{";
    json += "\"id\":" + std::to_string(workInfo.id) + ",";
    json += "\"deviceId\":\"" + workInfo.deviceId + "\",";
    json += "\"workId\":\"" + workInfo.workId + "\",";
    json += "\"audioId\":\"" + workInfo.audioId + "\",";
    json += "\"completed\":" + std::to_string(workInfo.completed);
    json += "}";

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    http_client_->SetContent(std::move(json));
    if (!http_client_->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return ESP_FAIL;
    }

    http_client_->Close();
    return ESP_OK;
}

int Esp32Music::UpdateWorkInfo(WorkInfo workInfo) {
    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/work/insert";
    } else {
        url += "work/insert";
    }

    std::string json = "{";
    json += "\"id\":" + std::to_string(workInfo.id) + ",";
    json += "\"deviceId\":\"" + workInfo.deviceId + "\",";
    json += "\"workId\":\"" + workInfo.workId + "\",";
    json += "\"audioId\":\"" + workInfo.audioId + "\",";
    json += "\"completed\":" + std::to_string(workInfo.completed);
    json += "}";

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    http_client_->SetContent(std::move(json));
    if (!http_client_->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return ESP_FAIL;
    }

    http_client_->Close();
    return ESP_OK;
}

int Esp32Music::deleteWorkInfo(WorkInfo workInfo) {
    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/work/delete";
    } else {
        url += "work/delete";
    }

    std::string json = "{";
    json += "\"id\":" + std::to_string(workInfo.id) + ",";
    json += "\"deviceId\":\"" + workInfo.deviceId + "\",";
    json += "\"workId\":\"" + workInfo.workId + "\",";
    json += "\"audioId\":\"" + workInfo.audioId + "\",";
    json += "\"completed\":" + std::to_string(workInfo.completed);
    json += "}";

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    http_client_->SetContent(std::move(json));
    if (!http_client_->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return ESP_FAIL;
    }

    http_client_->Close();
    return ESP_OK;
}

/*************************************************************************************
 * [该任务是在一个线程中执行]
 * 先等待小智把话说完，切换到listen模式，然后强制切换到idle模式；
 * 在函数中播放一集音频，则检测是否播放完成，播放完成之后将
 * 集数更新到后台的数据库中；
 ***************************************************************************************/
void Esp32Music::PlayAudiosImpl(const std::string& work_id, std::vector<AudioInfo> audio_infos, int32_t cur) {
    ESP_LOGI(TAG, "音频开始播放");

    // 标记播放的状态
    is_play_audios_status_ = 1;
#if 1      
    // 等小智把话说完了，变成聆听状态之后，马上转成待机状态，进入音乐播放
    while(true) {
        if (is_play_audios_status_ == 0) {
            return;
        }

        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        if (current_state == kDeviceStateListening) {
            ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");

            // 1. 首先获取协议对象
            Protocol* protocol = app.GetProtocol();
            if (protocol && protocol->IsAudioChannelOpened()) {
                // 2. 直接关闭音频通道（同步操作）
                protocol->CloseAudioChannel();
                ESP_LOGI(TAG, "Audio channel closed for music playback");
                
                // 3. 等待状态切换完成（增加延迟）
                int wait_count = 0;
                const int max_wait = 20; // 最多等待2秒
                
                while (wait_count < max_wait) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    current_state = app.GetDeviceState();
                    
                    if (current_state == kDeviceStateIdle) {
                        ESP_LOGI(TAG, "Successfully switched to idle state after %d ms", wait_count * 100);
                        break;
                    }
                    wait_count++;
                }
                
                if (current_state != kDeviceStateIdle) {
                    ESP_LOGW(TAG, "Failed to switch to idle state after %d ms, current state: %d", 
                            wait_count * 100, current_state);
                }
            } else {
                // 如果音频通道未打开，直接设置为空闲状态
                ESP_LOGI(TAG, "Audio channel already closed, setting to idle state");
                app.SetDeviceState(kDeviceStateIdle);
                break;
            }
            
            continue;
        }
        else if (current_state != kDeviceStateIdle) { // 不是待机状态，就一直卡在这里，不让播放音乐
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            // 如果不是空闲状态，暂停播放
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        else if (current_state == kDeviceStateIdle) {
            break;
        }
    }
#endif    

    for (int32_t i = cur; i < audio_infos.size();  i++) {
        if (!audio_infos.at(i).play_url.empty()) {
            if (audio_infos.size() > 1) {
                // 更新讲到当前集
                WorkInfo workInfo;
                workInfo.deviceId = GloableVar::device_id;
                workInfo.workId = work_id;
                workInfo.audioId = audio_infos.at(i).id;
                workInfo.completed = 0;
                InsertWorkInfo(workInfo);
            }

            // 其他中断线程修改了状态位
            if (is_play_audios_status_ == 0) {
                break;
            }

            ESP_LOGI(TAG, "播放音频: %s", audio_infos.at(i).play_url.c_str());
            StartStreaming(audio_infos.at(i).play_url);         // 已经改为同步函数

            // 其他中断线程修改了状态位
            if (is_play_audios_status_ == 0) {
                break;
            }

            if (is_state_completed_ == 2) {
                break;
            }

            if (audio_infos.size() > 1) {
                // 标记这一集完成
                WorkInfo workInfo;
                workInfo.deviceId = GloableVar::device_id;
                workInfo.workId = work_id;
                workInfo.audioId = audio_infos.at(i).id;
                workInfo.completed = 1;
                UpdateWorkInfo(workInfo);

                // 最后一集完成的话，删除播放记录
                if (i == (audio_infos.size() - 1)) {
                    deleteWorkInfo(workInfo);
                }
            }
        }
    }
    is_play_audios_status_ = 0; // 复位播放的状态
}

/****************************************
 * 根据作品名字查询作品和音频
 * 查询后台已经播放的集数
 * 交给线程取执行
 ****************************************/
bool Esp32Music::Download(const std::string& work_name, bool restart) {
    ESP_LOGI(TAG, "Starting to get works for: %s", work_name.c_str());
    
    // 清空之前的下载数据
    last_downloaded_data_.clear();

    // 查询作品信息
    if (!FindWork(work_name)) {
        return false;
    }

    if (!last_downloaded_data_.empty()) {
        std::string work_id = ParseWorkId();
        if (!work_id.empty()) {
            // 查询作品详情，获取音频url
            if (FindWorkDetail(work_id)) {
                std::vector<AudioInfo> audio_infos = ParseWorkDetail();
                if (audio_infos.size() != 0) {
                    // 查询当前作品讲到了第几集
                    int cur = 0;
                    if (audio_infos.size() > 1) {
                        if (!restart) {
                            WorkInfo workInfo = GetWorkInfo(work_id);
                            std::string audio_id = workInfo.audioId;
                            int32_t completed = workInfo.completed;

                            if (!audio_id.empty()) {
                                for (int32_t i = 0; i < audio_infos.size();  i++) {
                                    if (audio_id == audio_infos.at(i).id) {
                                        if ((i == (audio_infos.size() - 1)) && (completed == 1)) {
                                            // 最后一集讲完，从头开始讲
                                            cur = 0;
                                            break;
                                        }
                                        cur = i;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    ESP_LOGI(TAG, "作品第几集 %d", cur);

                    // 把讲故事交给一个线程
                    PlayAudiosTaskParams* params = new PlayAudiosTaskParams{
                        this, work_id, audio_infos, cur
                    };
                    
                    /******************************************
                     * 如果处于音频播放，则结束播放
                     *******************************************/
                    StopWorksAudio();

                    // 停止之前的音频播放任务（如果有）
                    if (play_audios_task_handle_ != nullptr) {
                        if (play_audios_task_static_allocated_) {
                            vTaskDelete(play_audios_task_handle_);
                            play_audios_task_static_allocated_ = false;
                            ESP_LOGI(TAG, "Deleted previous static play audio task");
                        } 
                        play_audios_task_handle_ = nullptr;
                    }
#if 0
                    // 改为静态分配内存
                    if (play_audios_task_stack_ == nullptr) {
                        play_audios_task_stack_ = (StackType_t*)heap_caps_malloc(3072 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
                        assert(play_audios_task_stack_ != nullptr);
                    }

                    if (play_audios_task_tcb_ == nullptr) {
                        play_audios_task_tcb_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
                        assert(play_audios_task_tcb_ != nullptr);
                    }
#endif
#if 1
                    xTaskCreatePinnedToCore(PlayAudiosTask, "play_audios", 3072, params, 2, &play_audios_task_handle_, 1);
#endif
#if 0
                    // 创建播放音频任务
                    play_audios_task_handle_ = xTaskCreateStatic(
                        PlayAudiosTask,
                        "play_audios",
                        3072,
                        params,
                        2,
                        play_audios_task_stack_,
                        play_audios_task_tcb_
                    );
#endif
                    assert(play_audios_task_handle_ != nullptr);

                    play_audios_task_static_allocated_ = true;
                    ESP_LOGI(TAG, "Static play audios task created successfully");
                    return true;
                }
                else {
                    ESP_LOGE(TAG, "作品下面没有音频");
                    return false;
                }
            }
            else {
                ESP_LOGE(TAG, "作品下面没有音频"); 
                return false;
            }
        }
        else {
            ESP_LOGE(TAG, "没有找到作品 '%s'", work_name.c_str());
            return false;
        }
    }
    else {
        ESP_LOGE(TAG, "Empty response from works API");
        return false;
    }  
    return false;
}

/**************************************
 * 允许接收通过url来调用，进行
 * 语音的播报
 * 补充：
 * 5种状态之间的转换，闹钟期间
 * 可以唤醒打断
 ***************************************/
bool Esp32Music::Play(const std::string& music_url, int repeat) {
    /**************************************************
     * 通话期间，闹钟不响，通过表情来提醒
     **************************************************/
    VoiceCall *voiceCall = VoiceCall::get_instance();
    if (voiceCall->m_udp_receive_task_running || voiceCall->m_udp_send_task_running) {
        ESP_LOGI(TAG, "通话期间，闹钟不响，通过表情来提醒");
        return true;
    }

    /************************************
     * 关掉当前的对话，进入待命状态
     ************************************/
    auto& application = Application::GetInstance();

    Protocol* protocol = application.GetProtocol();
    if (protocol && protocol->IsAudioChannelOpened()) {
        protocol->CloseAudioChannel();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    application.SetDeviceState(kDeviceStateIdle);

    /******************************************
     * 如果处于音频、闹钟播放，则结束播放
     *******************************************/
    StopWorksAudio();

    /*********************
     * 流式的语音播报
     *********************/
    for (int i = 0; i < repeat; i++) {
        StartStreaming(music_url);
        if (is_state_completed_ == 2) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return true;
}

/**************************************
 * 允许接收通过url来调用，进行
 * 语音的播报
 * 补充：
 * 5种状态之间的转换，闹钟期间
 * 可以唤醒打断
 ***************************************/
bool Esp32Music::PlayVoice(const std::string& music_url, int repeat) {
    /************************************
     * 关掉当前的对话，进入待命状态
     ************************************/
    auto& application = Application::GetInstance();

    Protocol* protocol = application.GetProtocol();
    if (protocol && protocol->IsAudioChannelOpened()) {
        protocol->CloseAudioChannel();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    application.SetDeviceState(kDeviceStateIdle);

    /******************************************
     * 如果处于音频、闹钟播放，则结束播放
     *******************************************/
    StopWorksAudio();

    /*********************
     * 流式的语音播报
     *********************/
    for (int i = 0; i < repeat; i++) {
        StartStreaming(music_url);
        if (is_state_completed_ == 2) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return true;
}

/******************************************
 * 如果处于音频、闹钟播放，则结束播放
 *******************************************/
void Esp32Music::StopWorksAudio() {
    is_state_completed_ = 2;

    if (is_play_audios_status_ == 1) {
        is_play_audios_status_ = 0;
        ESP_LOGI(TAG, "Stop works level play.");
    }
    if (is_downloading_ || is_playing_) {
        is_downloading_ = false;
        is_playing_ = false;

        while(true) {
            if (is_downloaded && is_played) {
                break;
            }
            else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        ESP_LOGI(TAG, "Stop audio level play.");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

/*****************************************
 * 支持家长端的歌曲点播
 * 防止阻塞mqtt，采用异步的方式
 *****************************************/
bool Esp32Music::Play2(const std::string& music_url) {

    // 把讲故事交给一个线程
    PlayAudioTaskParam* param = new PlayAudioTaskParam{
        this, music_url
    };

    /******************************************
     * 如果处于音频、闹钟播放，则结束播放
     *******************************************/
    StopWorksAudio();

    // 停止之前的音频播放任务（如果有）
    if (play_audio_task_handle_ != nullptr) {
        if (play_audio_task_static_allocated_) {
            vTaskDelete(play_audio_task_handle_);
            play_audio_task_static_allocated_ = false;
            ESP_LOGI(TAG, "Deleted previous static play audio task");
        } 
        play_audio_task_handle_ = nullptr;
    }
#if 0
    // 分配PlayAudioTask的静态任务内存
    if (play_audio_task_stack_ == nullptr) {
        play_audio_task_stack_ = (StackType_t*)heap_caps_malloc(3072 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        assert(play_audio_task_stack_ != nullptr);
        ESP_LOGI(TAG, "Allocated play audio task stack: %p", play_audio_task_stack_);
    }

    if (play_audio_task_tcb_ == nullptr) {
        play_audio_task_tcb_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(play_audio_task_tcb_ != nullptr);
        ESP_LOGI(TAG, "Allocated play audio task TCB: %p", play_audio_task_tcb_);
    }
#endif
#if 1

    xTaskCreatePinnedToCore(PlayAudioTask, "play_audio", 3072, param, 2, &play_audio_task_handle_, 1);
#endif
#if 0
    // 创建静态播放音频任务
    play_audio_task_handle_ = xTaskCreateStatic(
        PlayAudioTask,
        "play_audio",
        3072,
        param,
        2,
        play_audio_task_stack_,
        play_audio_task_tcb_
    );
#endif

    assert(play_audio_task_handle_ != nullptr);

    play_audio_task_static_allocated_ = true;
    ESP_LOGI(TAG, "Static play audio task created successfully, stack size: 3072");
    return true;
}

void Esp32Music::PlayAudioTask(void* arg) {
    ESP_LOGI(TAG, "PlayAudioTask started");

    auto* param = static_cast<PlayAudioTaskParam*>(arg);
    if (param) {
        param->self->PlayAudioImpl(param->music_url);
        delete param;
    }

    ESP_LOGI(TAG, "PlayAudioTask finished");

    // vTaskDelete(nullptr);
    while (true) vTaskDelay(portMAX_DELAY);
}

void Esp32Music::PlayAudioImpl(const std::string& music_url) {
    /************************************
     * 关掉当前的对话，进入待命状态
     ************************************/
    auto& application = Application::GetInstance();

    Protocol* protocol = application.GetProtocol();
    if (protocol && protocol->IsAudioChannelOpened()) {
        protocol->CloseAudioChannel();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    application.SetDeviceState(kDeviceStateIdle);

    /*********************
     * 流式的语音播报
     *********************/
    StartStreaming(music_url);
}

bool Esp32Music::Play() {
    return true;
}

bool Esp32Music::Stop() {
    return true;
}

std::string Esp32Music::GetDownloadResult() {
    return last_downloaded_data_;
}

std::string Esp32Music::GetWorkName() {
    return work_name_;
}

/**************************************************
 * 在开始中的任务之前，关闭以前的任务
 **************************************************/
void Esp32Music::ForceStopAllTasks() {
    ESP_LOGI(TAG, "Force stopping all tasks");
    
    // 设置停止标志
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
    // 🔥 关键：通知流式播放已被停止
    // signalStreamingStopped();

    vTaskDelay(pdMS_TO_TICKS(100));

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 删除所有任务句柄（按照依赖顺序）
    if (download_task_handle_ != nullptr) {
        if (download_task_static_allocated_) {
            vTaskDelete(download_task_handle_);
            download_task_static_allocated_ = false;
        }
        download_task_handle_ = nullptr;
    }
    
    if (play_task_handle_ != nullptr) {
        if (play_task_static_allocated_) {
            vTaskDelete(play_task_handle_);
            play_task_static_allocated_ = false;
        }
        play_task_handle_ = nullptr;
    }

    // 等待一小段时间确保任务已停止
    vTaskDelay(pdMS_TO_TICKS(100));
}

// 添加内存池重置函数
void Esp32Music::ResetMemoryPool() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // 将所有内存池块标记为未使用
    for (size_t i = 0; i < MAX_CONCURRENT_CHUNKS; ++i) {
        audio_chunk_pool_[i].in_use = false;
    }
    
    for (size_t i = 0; i < MAX_CONCURRENT_PACKETS; ++i) {
        audio_packet_pool_[i].in_use = false;
    }
    
    chunk_pool_index_ = 0;
    packet_pool_index_ = 0;
    
    ESP_LOGI(TAG, "Memory pool reset");
}

/****************************************************
 * 开始流式播放，只要一个url，返回的
 * mp3流
 ***************************************************/
bool Esp32Music::StartStreaming(const std::string& music_url) {

    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());
    
    // 重置事件组状态
    resetStreamingEvents();

    {
        ForceStopAllTasks();
        vTaskDelay(pdMS_TO_TICKS(100));

        // 停止之前的播放和下载
        is_downloading_ = false;
        is_playing_ = false;
        
        // 重置内存池状态
        ResetMemoryPool();

        // 清空缓冲区
        ClearAudioBuffer();

        // 重置mp3解码器
        CleanupMp3Decoder();
        InitializeMp3Decoder();
    }

    is_downloading_ = true;
    is_playing_ = true;

    /*********************************
     * 下载任务采用静态分配内存
     *********************************/
    if (download_task_handle_ != nullptr) {
        if (download_task_static_allocated_) {
            vTaskDelete(download_task_handle_);
            download_task_static_allocated_ = false;
        }
        download_task_handle_ = nullptr;
    }
#if 0
    if (download_task_stack_ == nullptr) {
        download_task_stack_ = (StackType_t*)heap_caps_malloc(DOWNLOAD_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        assert(download_task_stack_ != nullptr);
    }
    
    if (download_task_tcb_ == nullptr) {
        download_task_tcb_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(download_task_tcb_ != nullptr);
    }
#endif
    DownloadTaskParams* download_params = new DownloadTaskParams{this, music_url};
#if 1
    xTaskCreatePinnedToCore(DownloadAudioStreamTask, "audio_download", 1024 * 12, download_params, 2, &download_task_handle_, 1);
#endif
#if 0
    download_task_handle_ = xTaskCreateStatic(
        DownloadAudioStreamTask,
        "audio_download",
        DOWNLOAD_TASK_STACK_SIZE,
        download_params,
        2,
        download_task_stack_,
        download_task_tcb_
    );
 #endif   

    assert(download_task_handle_ != nullptr);

    is_downloaded = false;
    download_task_static_allocated_ = true;
    ESP_LOGI(TAG, "Static download task created successfully, stack size: %d", DOWNLOAD_TASK_STACK_SIZE);

    /*********************************
     * 播放任务采用静态分配内存
     *********************************/
    if (play_task_handle_ != nullptr) {
        if (play_task_static_allocated_) {
            vTaskDelete(play_task_handle_);
            play_task_static_allocated_ = false;
        }
        play_task_handle_ = nullptr;
    }
#if 0
    if (play_task_stack_ == nullptr) {
        play_task_stack_ = (StackType_t*)heap_caps_malloc(PLAY_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        assert(play_task_stack_ != nullptr);
    }
    
    if (play_task_tcb_ == nullptr) {
        play_task_tcb_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(play_task_tcb_ != nullptr);
    }
#endif
#if 1
    xTaskCreatePinnedToCore(PlayAudioStreamTask, "audio_play", 1024 * 24, this, 2, &play_task_handle_, 1);
#endif
#if 0
    play_task_handle_ = xTaskCreateStatic(
        PlayAudioStreamTask,
        "audio_play",
        PLAY_TASK_STACK_SIZE,
        this,
        2,
        play_task_stack_,
        play_task_tcb_ 
    );
#endif    

    assert(play_task_handle_ != nullptr);

    play_task_static_allocated_ = true;
    is_played = false;
    ESP_LOGI(TAG, "Static play task created successfully, stack size: %d", PLAY_TASK_STACK_SIZE);
    
  
    ESP_LOGI(TAG, "Streaming threads started successfully");

    /***************************************************************
     * 流式播放改为同步的方式，在播放结束之后，函数
     * 才退出
     ***************************************************************/
    /***************************************************************
     * 🔥 关键修改：使用事件组进行阻塞等待
     * 原来的忙等待循环替换为事件组等待
     ***************************************************************/
    StreamingResult result = waitForStreamingCompletion(0);
    
    // 根据结果进行处理
    switch (result) {
        case StreamingResult::SUCCESS:
            ESP_LOGI(TAG, "Streaming completed successfully");
            break;
            
        case StreamingResult::ERROR:
            ESP_LOGE(TAG, "Streaming failed with error");
            break;
            
        case StreamingResult::STOPPED:
            ESP_LOGW(TAG, "Streaming stopped by external request");
            break;
            
        case StreamingResult::TIMEOUT:
            ESP_LOGW(TAG, "Streaming timeout, forcing stop");
            break;
            
        default:
            ESP_LOGE(TAG, "Unknown streaming result");
            break;
    }

    // delete download_params;
    {
        ForceStopAllTasks();
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 重置内存池状态
        ResetMemoryPool();

        // 清空缓冲区
        ClearAudioBuffer();
    }

    ESP_LOGI(TAG, "Streaming play successfully");
    return true;
}

/******************************************
 * 对应下载线程的任务，希望这个
 * 任务是独占式的
 *******************************************/
void Esp32Music::DownloadAudioStreamTask(void* arg) {
    ESP_LOGI(TAG, "DownloadAudioStreamTask started");
    
    auto* params = static_cast<DownloadTaskParams*>(arg);

    params->self->is_downloaded = false;
    params->self->DownloadAudioStreamImpl(params->music_url);
    
    ESP_LOGI(TAG, "DownloadAudioStreamTask finished");

    params->self->is_downloaded = true;

    // 🔥 关键：通知下载完成
    params->self->signalDownloadComplete();

    delete params;

    // vTaskDelete(nullptr);
    while (true) vTaskDelay(portMAX_DELAY);
}

/******************************************
 * 对应播放线程的任务，希望这个
 * 任务是独占式的
 *******************************************/
void Esp32Music::PlayAudioStreamTask(void* arg) {
    ESP_LOGI(TAG, "PlayAudioStreamTask started");

    // 🔥 音乐开始前，通知 AudioService 旁路 AEC 参考信号
    Application::GetInstance().GetAudioService().SetBypassAecReference(true);

    auto* self = static_cast<Esp32Music*>(arg);
    self->is_played = false;
    if (self) {
        self->PlayAudioStreamImpl();
    }
    
    ESP_LOGI(TAG, "PlayAudioStreamTask finished");

    self->is_played = true;

    // 🔥 音乐结束后恢复 AEC 参考信号
    Application::GetInstance().GetAudioService().SetBypassAecReference(false);
    
    // 🔥 关键：通知播放完成
    self->signalPlayComplete();

    // vTaskDelete(nullptr);
    while (true) vTaskDelay(portMAX_DELAY);
}

/******************************************
 * 对应作品级线程的任务，希望这个
 * 任务是独占式的
 *******************************************/
void Esp32Music::PlayAudiosTask(void* arg) {
    ESP_LOGI(TAG, "PlayAudiosTask started");
    
    auto* params = static_cast<PlayAudiosTaskParams*>(arg);
    if (params) {
        params->self->PlayAudiosImpl(params->work_id, params->audio_infos, params->cur);
        delete params;
    }
    
    ESP_LOGI(TAG, "PlayAudiosTask finished");

    // vTaskDelete(nullptr);
    while (true) vTaskDelay(portMAX_DELAY);
}

bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d", 
            is_downloading_.load(), is_playing_.load());

    // 重置采样率到原始值
    ResetSampleRate();
    
    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "No streaming in progress");
        return true;
    }
    
    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待任务结束（非阻塞方式）
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 删除播放任务（处理静态分配）
    if (play_task_handle_ != nullptr) {
        if (play_task_static_allocated_) {
            vTaskDelete(play_task_handle_);
            play_task_static_allocated_ = false;
            ESP_LOGI(TAG, "Deleted static play task");
        } else {
            vTaskDelete(play_task_handle_);
            ESP_LOGI(TAG, "Deleted dynamic play task");
        }
        play_task_handle_ = nullptr;
    }
    
    // 删除下载任务
    if (download_task_handle_ != nullptr) {
        if (download_task_static_allocated_) {
            vTaskDelete(download_task_handle_);
            download_task_static_allocated_ = false;
            ESP_LOGI(TAG, "Deleted static download task");
        } else {
            vTaskDelete(download_task_handle_);
            ESP_LOGI(TAG, "Deleted dynamic download task");
        }
        download_task_handle_ = nullptr;
    }

    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// ==================== 内存池初始化 ====================
void Esp32Music::InitializeMemoryPool() {
    ESP_LOGI(TAG, "Initializing memory pool for audio streaming");
    
    // 初始化音频块内存池
    for (size_t i = 0; i < MAX_CONCURRENT_CHUNKS; ++i) {
        audio_chunk_pool_[i].data = nullptr;
        audio_chunk_pool_[i].capacity = 0;
        audio_chunk_pool_[i].used_size = 0;
        audio_chunk_pool_[i].in_use = false;
        audio_chunk_pool_[i].timestamp = 0;
    }
    
    // 初始化音频包内存池
    for (size_t i = 0; i < MAX_CONCURRENT_PACKETS; ++i) {
        audio_packet_pool_[i].packet.payload.reserve(4096);  // 预分配4KB
        audio_packet_pool_[i].in_use = false;
        audio_packet_pool_[i].timestamp = 0;
    }
    
    // 初始化索引
    chunk_pool_index_ = 0;
    packet_pool_index_ = 0;
    
    memory_pool_initialized_ = true;
    
    ESP_LOGI(TAG, "Memory pool initialized: %d chunks, %d packets", 
             MAX_CONCURRENT_CHUNKS, MAX_CONCURRENT_PACKETS);
}

// ==================== 内存池清理 ====================
void Esp32Music::CleanupMemoryPool() {
    ESP_LOGI(TAG, "Cleaning up memory pool");
    
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // 释放音频块内存池
    for (size_t i = 0; i < MAX_CONCURRENT_CHUNKS; ++i) {
        if (audio_chunk_pool_[i].data) {
            heap_caps_free(audio_chunk_pool_[i].data);
            audio_chunk_pool_[i].data = nullptr;
            audio_chunk_pool_[i].capacity = 0;
        }
        audio_chunk_pool_[i].in_use = false;
    }
    
    // 清理音频包内存池（保留vector容量）
    for (size_t i = 0; i < MAX_CONCURRENT_PACKETS; ++i) {
        audio_packet_pool_[i].packet.payload.clear();
        audio_packet_pool_[i].packet.payload.shrink_to_fit();
        audio_packet_pool_[i].in_use = false;
    }
    
    memory_pool_initialized_ = false;
    ESP_LOGI(TAG, "Memory pool cleaned up");
}

// ==================== 音频块分配 ====================
uint8_t* Esp32Music::AllocateAudioChunk(size_t size) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // 策略1：优先查找完全空闲的块
    for (size_t i = 0; i < MAX_CONCURRENT_CHUNKS; ++i) {
        size_t index = (chunk_pool_index_ + i) % MAX_CONCURRENT_CHUNKS;
        
        if (!audio_chunk_pool_[index].in_use) {
            // 检查容量是否足够
            if (!audio_chunk_pool_[index].data) {
                // 首次分配，分配足够容量
                size_t allocate_size = std::max(size, DEFAULT_CHUNK_SIZE * 2);
                
                audio_chunk_pool_[index].data = (uint8_t*)heap_caps_malloc(
                    allocate_size, MALLOC_CAP_SPIRAM);
                
                if (!audio_chunk_pool_[index].data) {
                    ESP_LOGE(TAG, "Failed to allocate chunk memory (%d bytes)", allocate_size);
                    return nullptr;
                }
                
                audio_chunk_pool_[index].capacity = allocate_size;
                ESP_LOGD(TAG, "Allocated new chunk %d: %d bytes", index, allocate_size);
            }
            else if (audio_chunk_pool_[index].capacity < size) {
                // 容量不足，重新分配
                heap_caps_free(audio_chunk_pool_[index].data);
                
                size_t allocate_size = std::max(size, DEFAULT_CHUNK_SIZE * 2);
                
                audio_chunk_pool_[index].data = (uint8_t*)heap_caps_malloc(
                    allocate_size, MALLOC_CAP_SPIRAM);
                
                if (!audio_chunk_pool_[index].data) {
                    ESP_LOGE(TAG, "Failed to reallocate chunk memory (%d bytes)", allocate_size);
                    return nullptr;
                }
                
                audio_chunk_pool_[index].capacity = allocate_size;
                ESP_LOGD(TAG, "Reallocated chunk %d to %d bytes", index, allocate_size);
            }
            
            // 标记为使用中
            audio_chunk_pool_[index].in_use = true;
            audio_chunk_pool_[index].used_size = size;
            audio_chunk_pool_[index].timestamp = esp_log_timestamp();
            
            chunk_pool_index_ = (index + 1) % MAX_CONCURRENT_CHUNKS;
            
            ESP_LOGD(TAG, "Allocated chunk %d: %d/%d bytes", 
                    index, size, audio_chunk_pool_[index].capacity);
            
            return audio_chunk_pool_[index].data;
        }
    }
    
    // 策略2：如果池中所有块都在使用，尝试等待并重试
    // ESP_LOGW(TAG, "Memory pool exhausted, waiting for free chunk...");
    
    // 短暂等待后返回null，让上层处理
    return nullptr;
}

// ==================== 音频块释放 ====================
void Esp32Music::FreeAudioChunk(uint8_t* data) {
    if (!data) return;
    
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // 在内存池中查找
    for (size_t i = 0; i < MAX_CONCURRENT_CHUNKS; ++i) {
        if (audio_chunk_pool_[i].data == data) {
            audio_chunk_pool_[i].in_use = false;
            ESP_LOGD(TAG, "Freed chunk %d (in use for %u ms)", 
                    i, esp_log_timestamp() - audio_chunk_pool_[i].timestamp);
            return;
        }
    }
    
    // 如果不在内存池中，直接释放
    ESP_LOGW(TAG, "Chunk not in memory pool, freeing directly");
    heap_caps_free(data);
}

// ==================== 音频包分配 ====================
AudioStreamPacket* Esp32Music::AllocateAudioPacket() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // 查找空闲包
    for (size_t attempts = 0; attempts < MAX_CONCURRENT_PACKETS * 2; ++attempts) {
        size_t index = packet_pool_index_ % MAX_CONCURRENT_PACKETS;
        packet_pool_index_ = (packet_pool_index_ + 1) % MAX_CONCURRENT_PACKETS;
        
        if (!audio_packet_pool_[index].in_use) {
            audio_packet_pool_[index].in_use = true;
            audio_packet_pool_[index].timestamp = esp_log_timestamp();
            
            // 重置包内容（保留容量）
            audio_packet_pool_[index].packet.payload.clear();
            audio_packet_pool_[index].packet.sample_rate = 0;
            audio_packet_pool_[index].packet.frame_duration = 0;
            audio_packet_pool_[index].packet.timestamp = 0;
            
            ESP_LOGD(TAG, "Allocated packet %d", index);
            return &audio_packet_pool_[index].packet;
        }
    }
    
    // 如果找不到空闲包，创建新对象
    ESP_LOGW(TAG, "Packet pool exhausted, creating new packet");
    auto* packet = new AudioStreamPacket();
    packet->payload.reserve(4096);
    return packet;
}

// ==================== 音频包释放 ====================
void Esp32Music::FreeAudioPacket(AudioStreamPacket* packet) {
    if (!packet) return;
    
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // 在内存池中查找
    for (size_t i = 0; i < MAX_CONCURRENT_PACKETS; ++i) {
        if (&audio_packet_pool_[i].packet == packet) {
            audio_packet_pool_[i].in_use = false;
            
            // 清空数据但保留容量（减少内存碎片）
            if (audio_packet_pool_[i].packet.payload.capacity() > 8192) {
                // 如果容量太大，缩减到合理大小
                audio_packet_pool_[i].packet.payload.shrink_to_fit();
                audio_packet_pool_[i].packet.payload.reserve(4096);
            }
            
            ESP_LOGD(TAG, "Freed packet %d (in use for %u ms)", 
                    i, esp_log_timestamp() - audio_packet_pool_[i].timestamp);
            return;
        }
    }
    
    // 如果不在内存池中，直接删除
    ESP_LOGW(TAG, "Packet not in memory pool, deleting directly");
    delete packet;
}

/****************************************
* 具体执行下载音乐流的函数，在指定的URL进行下载
****************************************/
void Esp32Music::DownloadAudioStreamImpl(const std::string& music_url) {
    ESP_LOGI(TAG, "开始音频流下载 - 添加统计信息");
  
    // 验证URL有效性
    if (music_url.empty() || music_url.find("http") != 0) {
        ESP_LOGE(TAG, "无效的URL格式: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    // 初始化统计信息
    long local_downloaded_bytes = 0;  // 本地下载字节统计（用于兼容现有逻辑）
    std::lock_guard<std::mutex> lock(stats_mutex_);
    total_downloaded_in_mp3_bytes = 0;  // 重置全局下载统计
    
    // 清理之前的HTTP连接
    int connection_id = esp_random() % 10000;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(connection_id);

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    // 设置请求头
    http_client_->SetHeader("Accept", "*/*");

    if (!http_client_->Open("GET", music_url)) {
        ESP_LOGE(TAG, "无法连接到音乐流URL: %s", music_url.c_str());
        http_client_->Close();
        is_downloading_ = false;
        return;
    }

    int status_code = http_client_->GetStatusCode();
    if (status_code != 200 && status_code != 206) {  // 206 for partial content
        ESP_LOGE(TAG, "HTTP GET失败，状态码: %d", status_code);
        http_client_->Close();
        is_downloading_ = false;
        return;
    }

    ESP_LOGI(TAG, "开始下载音频流，状态: %d", status_code);
    
    // 分块读取音频数据 - 使用固定大小的缓冲区
    const size_t chunk_size = DEFAULT_CHUNK_SIZE;  // 使用内存池默认大小
    
    // 下载统计
    size_t pool_allocations = 0;
    size_t dynamic_allocations = 0;
    size_t allocation_failures = 0;
    size_t chunks_dropped = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    // 🔥 添加下载统计日志
    ESP_LOGI(TAG, "========== 下载统计开始 ==========");
    ESP_LOGI(TAG, "URL: %s", music_url.c_str());
    ESP_LOGI(TAG, "HTTP状态码: %d", status_code);
    ESP_LOGI(TAG, "开始时间: %lld", (long long)start_time.time_since_epoch().count());

    // 限制下载线程的流量
    const size_t MAX_BYTES_PER_SECOND = 10 * 1024;  // 10 KB/s
    size_t bytes_this_second = 0;
    auto second_start = std::chrono::steady_clock::now();

    // 重置事件组
    resetEvents();

    while (is_downloading_ && is_playing_) {
        // 🔥 速率控制检查
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time).count();

        // 每10秒打印一次统计信息（如果下载时间较长）
        if (elapsed > 0 && elapsed % 10000 == 0) {
            ESP_LOGI(TAG, "下载进行中: 已下载 %ld 字节，耗时 %lld 毫秒", 
                    local_downloaded_bytes, elapsed);
        }

        int bytes_read = http_client_->Read(download_buffer_, chunk_size);
        if (bytes_read < 0) {
            signalWIFIError();
          
            ESP_LOGE(TAG, "读取音频数据失败: 错误码 %d", bytes_read);
            break;
        }
        if (bytes_read == 0) {
            ESP_LOGI(TAG, "音频流下载完成，总共: %ld 字节", local_downloaded_bytes);
            break;
        }
        
        // 🔥 更新下载统计
        local_downloaded_bytes += bytes_read;
        total_downloaded_in_mp3_bytes += bytes_read; // 更新全局统计

        // 限制下载线程的流量
        {
            bytes_this_second += bytes_read;
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - second_start).count();

            if (elapsed_ms >= 1000) {
                // 每秒重置计数器
                bytes_this_second = 0;
                second_start = now;
            } else if (bytes_this_second >= MAX_BYTES_PER_SECOND) {
                // 本秒内已下载足够数据，休眠剩余时间
                int sleep_ms = 1000 - elapsed_ms;
                vTaskDelay(pdMS_TO_TICKS(sleep_ms));
            }
        }

        // 🔥 每下载128KB打印一次进度
        if (local_downloaded_bytes % (128 * 1024) == 0) {
            ESP_LOGI(TAG, "下载进度: %ld 字节", local_downloaded_bytes);
            
            // 计算下载速度
            auto current_time = std::chrono::steady_clock::now();
            auto time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                current_time - start_time).count();
            if (time_elapsed > 0) {
                float speed_kbps = (local_downloaded_bytes * 8.0f) / (time_elapsed * 1.024f);  // kbps
                ESP_LOGI(TAG, "下载速度: %.1f kbps", speed_kbps);
            }
        }
        
        // 使用新的内存池分配音频块
        uint8_t* chunk_data = nullptr;
        int retry_count = 0;
        const int max_retries = 1000000;  // 最多重试5次
        
        while (is_downloading_ && (retry_count < max_retries) && !chunk_data) {
            chunk_data = AllocateAudioChunk(bytes_read);
            
            if (!chunk_data) {
                taskYIELD();
                vTaskDelay(pdMS_TO_TICKS(20));  // 等待10ms再试
                retry_count++;
                ESP_LOGD(TAG, "块分配失败，重试 %d/%d", retry_count + 1, max_retries);
            }
        }
        
        if (!is_downloading_) {
            if (chunk_data) {
                FreeAudioChunk(chunk_data);
            }
            break;
        }

        if (!chunk_data) {
            // 分配失败，跳过这个数据块
            allocation_failures++;
            ESP_LOGW(TAG, "音频块内存分配失败，重试 %d 次后跳过 %d 字节", 
                     max_retries, bytes_read);
            
            // 检查是否需要等待缓冲区释放
            if (buffer_size_.load(std::memory_order_relaxed) > MAX_BUFFER_SIZE / 2) {
                // 缓冲区已超过一半，等待一段时间
                ESP_LOGD(TAG, "缓冲区大小 %d，等待消费者...", buffer_size_.load(std::memory_order_relaxed));
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            
            continue;  // 跳过这个数据块，继续下一个
        }
        
        // 记录分配类型
        if (memory_pool_initialized_) {
            pool_allocations++;
        } else {
            dynamic_allocations++;
        }
        
        // 复制数据
        memcpy(chunk_data, download_buffer_, bytes_read);
        
        // 修改等待缓冲区有空间的逻辑
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            
            // 使用超时避免永久等待
            bool wait_result = false;
            while(is_downloading_ && !wait_result) {
                wait_result = buffer_cv_.wait_for(
                    lock, 
                    std::chrono::milliseconds(20),  // 增加超时时间到20ms
                    [this] { 
                        return buffer_size_.load(std::memory_order_relaxed) < MAX_BUFFER_SIZE || !is_downloading_; 
                    }
                );
            }
            
            // 如果等待超时或者下载已停止
            if (!is_downloading_) {
                FreeAudioChunk(chunk_data);
                break;
            }
            
            // 检查缓冲区大小，避免内存耗尽
            if (buffer_size_.load(std::memory_order_relaxed) + bytes_read > MAX_BUFFER_SIZE) {
                chunks_dropped++;
                ESP_LOGW(TAG, "缓冲区已满 (%d + %d > %d)，丢弃块", 
                        buffer_size_.load(std::memory_order_relaxed), bytes_read, MAX_BUFFER_SIZE);
                FreeAudioChunk(chunk_data);
                
                // 如果缓冲区持续满，增加等待时间
                if (chunks_dropped % 10 == 0) {
                    ESP_LOGW(TAG, "多个块被丢弃 (%d)，消费者可能太慢", chunks_dropped);
                    taskYIELD();
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                continue;
            }
            
            // 添加到缓冲区队列
            if (wait_result) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));

                buffer_size_.fetch_add(bytes_read, std::memory_order_relaxed);
                
                // 🔥 更新缓冲区统计
                if (buffer_size_.load(std::memory_order_relaxed) % (256 * 1024) == 0) {
                    ESP_LOGI(TAG, "缓冲区状态: 大小=%lld 字节，队列长度=%d", 
                            buffer_size_.load(std::memory_order_relaxed), audio_buffer_.size());
                }
                
                // 通知播放线程有新数据
                buffer_cv_.notify_one();

                signalDownload();
            }
            
            // 每下载1MB打印一次详细状态
            if (local_downloaded_bytes % (1024 * 1024) == 0) {
                auto current_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time).count();
                float download_speed = (local_downloaded_bytes * 1000.0f) / (elapsed * 1024.0f); // KB/s
                
                ESP_LOGI(TAG, "下载统计: %ld 字节 (%.1f KB/s)，缓冲区: %d 字节", 
                        local_downloaded_bytes, download_speed, buffer_size_.load(std::memory_order_relaxed));
                
                // 打印内存池状态
                LogMemoryPoolStatus();
            }
        }

        // 🔥 每处理完一个 chunk 后让出 CPU
        taskYIELD();
    }
    
    http_client_->Close();
    is_downloading_ = false;
    
    // 🔥 下载完成统计
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    // 计算下载速度和效率
    float download_speed_kbps = 0;
    float download_speed_kBps = 0;
    if (elapsed > 0) {
        download_speed_kbps = (local_downloaded_bytes * 8.0f) / (elapsed * 1.024f);  // kbps
        download_speed_kBps = (local_downloaded_bytes * 1000.0f) / (elapsed * 1024.0f); // KB/s
    }
    
    // 🔥 详细下载统计报告
    ESP_LOGI(TAG, "========== 下载统计完成 ==========");
    ESP_LOGI(TAG, "1. 总下载字节数: %ld 字节 (%.2f MB)", 
            local_downloaded_bytes, local_downloaded_bytes / (1024.0f * 1024.0f));
    ESP_LOGI(TAG, "2. 下载耗时: %ld 毫秒 (%.1f 秒)", 
            elapsed, elapsed / 1000.0f);
    ESP_LOGI(TAG, "3. 平均下载速度: %.1f kbps (%.1f KB/s)", 
            download_speed_kbps, download_speed_kBps);
    ESP_LOGI(TAG, "4. 内存池分配: 池=%d, 动态=%d", 
            pool_allocations, dynamic_allocations);
    ESP_LOGI(TAG, "5. 分配失败: %d 次", allocation_failures);
    ESP_LOGI(TAG, "6. 丢弃块: %d 个", chunks_dropped);
    ESP_LOGI(TAG, "7. 最终缓冲区大小: %d 字节", buffer_size_.load(std::memory_order_relaxed));
    ESP_LOGI(TAG, "8. HTTP状态码: %d", status_code);
    
    // 🔥 重要：输出全局下载统计（用于播放线程检查）
    ESP_LOGI(TAG, "9. 全局下载统计 (total_downloaded_in_mp3_bytes): %ld 字节", 
            total_downloaded_in_mp3_bytes);
    
    // 检查下载数据完整性
    if (status_code == 206) {
        ESP_LOGW(TAG, "注意：服务器返回部分内容 (206)，可能不是完整音频");
    }
    
    if (allocation_failures > 0) {
        ESP_LOGW(TAG, "注意：有 %d 次内存分配失败", allocation_failures);
    }
    
    if (chunks_dropped > 0) {
        ESP_LOGW(TAG, "注意：有 %d 个数据块被丢弃", chunks_dropped);
    }
    
    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "音频流下载线程结束");
}

// 添加内存池状态监控
void Esp32Music::LogMemoryPoolStatus() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    size_t used_chunks = 0;
    size_t used_packets = 0;
    
    for (size_t i = 0; i < MAX_CONCURRENT_CHUNKS; ++i) {
        if (audio_chunk_pool_[i].in_use) used_chunks++;
    }
    
    for (size_t i = 0; i < MAX_CONCURRENT_PACKETS; ++i) {
        if (audio_packet_pool_[i].in_use) used_packets++;
    }
    
    ESP_LOGI(TAG, "Memory pool status: chunks %d/%d, packets %d/%d, buffer size: %d",
             used_chunks, MAX_CONCURRENT_CHUNKS,
             used_packets, MAX_CONCURRENT_PACKETS,
             buffer_size_.load(std::memory_order_relaxed));
}

// 成员函数实现 - 播放音频流
void Esp32Music::PlayAudioStreamImpl() {
    PlayAudioStream();
}

/************************************************************
 * 由于音乐播放和MQTT之间的网络冲突问题，决定
 * 使用事件组的方式来进行音乐的播放。
 ************************************************************/
// 加了taskYIELD
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "========== 开始音频流播放 ==========");
  
    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    auto codec = Board::GetInstance().GetAudioCodec();
    ESP_LOGI(TAG, "获取音频编解码器: %p", codec);

    if (!mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "MP3解码器未初始化");
        is_playing_ = false;
        return;
    }
    
    ESP_LOGI(TAG, "MP3解码器已初始化，等待缓冲区数据...");
    
    size_t total_played = 0;
    int bytes_left = 0;
    uint8_t* read_ptr = mp3_input_buffer_;  // 使用静态缓冲区
    
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;

    // 支持音乐播放中，小智唤醒之后，停止音乐播放
    is_state_completed_ = 0;
    ESP_LOGI(TAG, "播放状态标志初始化完成");
    
    // 播放统计
    size_t pool_packets = 0;
    size_t dynamic_packets = 0;
    size_t memory_pool_hits = 0;
    size_t memory_pool_misses = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    // 播放关键统计
    size_t total_buffer_fetches = 0;
    size_t total_decode_attempts = 0;
    size_t total_decode_success = 0;
    size_t total_decode_failures = 0;
    size_t total_pcm_bytes_sent = 0;
    
    ESP_LOGI(TAG, "开始主播放循环...");

    while (is_playing_) {
        auto bits = xEventGroupWaitBits(event_group_, PLAY_EVENT_SCHEDULE | WIFI_EVENT_SCHEDULE, pdTRUE, pdFALSE, pdMS_TO_TICKS(10));

        if (bits == 0) {
            // 超时：检查是否应该继续
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (audio_buffer_.empty() && !is_downloading_) {
                break; // 无数据且下载结束，退出播放
            }
        }
        
        if (bits & WIFI_EVENT_SCHEDULE) {
            break;      // 网络异常
        }
        
        if (bits & PLAY_EVENT_SCHEDULE) {
            total_decode_attempts++;

            // 检查设备状态
            auto& app = Application::GetInstance();
            DeviceState current_state = app.GetDeviceState();

            // 等小智把话说完了，变成聆听状态之后，马上转成待机状态，进入音乐播放
            if (current_state != kDeviceStateIdle) {
                ESP_LOGI(TAG, "⚠️ 检测到小智被唤醒 (状态: %d)，停止音乐播放", current_state);
        
                // 在播放状态下，小智被唤醒，停止下载和播放
                is_playing_ = false;
                is_downloading_ = false;
                is_state_completed_ = 2;
                
                // 打印中断时的统计
                auto interrupt_time = std::chrono::steady_clock::now();
                auto elapsed_interrupt = std::chrono::duration_cast<std::chrono::milliseconds>(interrupt_time - start_time).count();
                ESP_LOGI(TAG, "播放被中断 - 总播放: %d 字节，耗时: %ld ms", total_played, elapsed_interrupt);
                break;
            }

            // 在正常的播放状态下
            if (current_state == kDeviceStateIdle) {
                is_state_completed_ = 1;
                codec->EnableOutput(true);
                Application::GetInstance().GetAudioService().is_voice_out_ = true;
            }

            // 如果需要更多MP3数据，从缓冲区读取
            if (bytes_left < 4096) {  // 减少阈值，更频繁地填充缓冲区
                total_buffer_fetches++;
                
                AudioChunk chunk;
                
                // 从缓冲区获取音频数据
                {
                    std::unique_lock<std::mutex> lock(buffer_mutex_);
                    
                    // 记录缓冲区状态
                    if (total_buffer_fetches % 50 == 0) {  // 每50次获取打印一次状态
                        ESP_LOGI(TAG, "缓冲区状态 - 大小: %d 字节，队列长度: %d，下载状态: %d", 
                                buffer_size_.load(std::memory_order_relaxed), audio_buffer_.size(), is_downloading_);
                    }
                    
                    if (audio_buffer_.empty()) {
                        if (!is_downloading_) {
                            // 下载完成且缓冲区为空，播放结束
                            ESP_LOGI(TAG, "🎵 播放完成 - 总共播放: %d 字节", total_played);
                            break;
                        }
                        
                        // 记录等待状态
                        if (total_buffer_fetches % 20 == 0) {
                            ESP_LOGI(TAG, "🕐 等待新数据 (缓冲区空，下载进行中)...");
                        }
                        
                        // 等待新数据（缩短超时）
                        if (!buffer_cv_.wait_for(lock, std::chrono::milliseconds(10), 
                            [this] { return !audio_buffer_.empty() || !is_downloading_; })) {
                            // 超时记录
                            if (total_buffer_fetches % 100 == 0) {
                                ESP_LOGD(TAG, "缓冲区等待超时，继续尝试...");
                            }
                            continue;  // 超时继续尝试
                        }
                        if (audio_buffer_.empty()) {
                            continue;
                        }
                    }
                    
                    chunk = audio_buffer_.front();
                    audio_buffer_.pop();

                    buffer_size_.fetch_sub(chunk.size, std::memory_order_relaxed);
                    
                    // 通知下载线程缓冲区有空间
                    buffer_cv_.notify_one();
                    
                    // 记录获取数据详情
                    if (chunk.size > 0) {
                        ESP_LOGD(TAG, "从缓冲区获取数据块 - 大小: %d 字节，剩余缓冲区: %d 字节", 
                                chunk.size, buffer_size_.load(std::memory_order_relaxed));
                    }
                }
                
                // 将新数据添加到MP3输入缓冲区
                if (chunk.data && chunk.size > 0) {
                    // 移动剩余数据到缓冲区开头
                    if (bytes_left > 0 && read_ptr != mp3_input_buffer_) {
                        ESP_LOGD(TAG, "移动剩余 %d 字节到缓冲区开头", bytes_left);
                        memmove(mp3_input_buffer_, read_ptr, bytes_left);
                    }
                    
                    // 检查缓冲区空间
                    size_t space_available = MP3_INPUT_BUFFER_SIZE - bytes_left;
                    size_t copy_size = std::min(chunk.size, space_available);
                    
                    // 复制新数据
                    if (copy_size > 0) {
                        ESP_LOGD(TAG, "复制 %d 字节到MP3缓冲区 (可用空间: %d，剩余: %d)", 
                                copy_size, space_available, bytes_left);
                        
                        memcpy(mp3_input_buffer_ + bytes_left, chunk.data, copy_size);
                        bytes_left += copy_size;
                        read_ptr = mp3_input_buffer_;
                        
                        // 定期报告MP3缓冲区状态
                        if (total_buffer_fetches % 100 == 0) {
                            ESP_LOGI(TAG, "MP3缓冲区状态 - 总大小: %d 字节，指针位置: %p", 
                                    bytes_left, static_cast<void*>(read_ptr));
                        }
                    } else {
                        ESP_LOGW(TAG, "缓冲区空间不足! 可用: %d，需要: %d", space_available, chunk.size);
                    }
                    
                    // 检查并跳过ID3标签（仅在开始时处理一次）
                    if (!id3_processed && bytes_left >= 10) {
                        size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                        if (id3_skip > 0) {
                            ESP_LOGI(TAG, "跳过ID3标签: %u 字节", (unsigned int)id3_skip);
                            read_ptr += id3_skip;
                            bytes_left -= id3_skip;
                        }
                        id3_processed = true;
                    }
                    
                    // 释放chunk内存（使用内存池）
                    FreeAudioChunk(chunk.data);
                }
            }

            // 尝试找到MP3帧同步
            int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
            if (sync_offset < 0) {
                // 没有找到同步字，跳过部分数据
                memory_pool_misses++;
                
                if (bytes_left > 1) {
                    // 记录同步失败
                    if (memory_pool_misses % 100 == 0) {
                        ESP_LOGW(TAG, "未找到MP3同步字，跳过1字节 (失败次数: %d，剩余数据: %d 字节)", 
                                memory_pool_misses, bytes_left);
                    }
                    
                    read_ptr++;
                    bytes_left--;
                } else {
                    // 记录数据不足
                    if (bytes_left == 0 && total_buffer_fetches % 50 == 0) {
                        ESP_LOGI(TAG, "MP3缓冲区已空，等待更多数据...");
                    }
                    bytes_left = 0;
                }
                continue;
            }

            // 跳过到同步位置
            if (sync_offset > 0) {
                ESP_LOGD(TAG, "找到同步字偏移: %d 字节", sync_offset);
                read_ptr += sync_offset;
                bytes_left -= sync_offset;
            }
            
            // 解码前记录
            ESP_LOGD(TAG, "开始解码MP3帧 - 数据大小: %d 字节", bytes_left);
            
            // 解码MP3帧
            int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer_, 0);

            if (decode_result == 0) {
                // 解码成功，获取帧信息
                total_decode_success++;
                MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
                total_frames_decoded_++;
                
                // 记录解码成功
                if (total_decode_success % 100 == 0) {
                    ESP_LOGI(TAG, "✅ 成功解码 %d 帧，采样率: %d，通道数: %d，采样数: %d", 
                            total_decode_success, mp3_frame_info_.samprate, 
                            mp3_frame_info_.nChans, mp3_frame_info_.outputSamps);
                } else {
                    ESP_LOGD(TAG, "解码成功 - 帧 %d", total_decode_success);
                }
                
                // 基本的帧信息有效性检查
                if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                    ESP_LOGW(TAG, "⚠️ 无效的帧信息 - 采样率: %d，通道数: %d，跳过此帧", 
                            mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                    memory_pool_misses++;
                    continue;
                }
                
                // 计算当前帧的持续时间(毫秒)
                int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                       (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
                
                // 更新当前播放时间
                current_play_time_ms_ += frame_duration_ms;
                
                // 记录时间进度
                if (current_play_time_ms_ % 5000 == 0) {  // 每5秒打印一次
                    ESP_LOGI(TAG, "⏱️ 播放时间: %lld 秒", current_play_time_ms_ / 1000);
                }
                
                // 更新歌词显示
                int buffer_latency_ms = 600;
                UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
                
                // 将PCM数据发送到Application的音频解码队列
                if (mp3_frame_info_.outputSamps > 0) {
                    int16_t* final_pcm_data = pcm_buffer_;
                    int final_sample_count = mp3_frame_info_.outputSamps;
                    
                    // 如果是双通道，转换为单通道混合
                    if (mp3_frame_info_.nChans == 2) {
                        // 使用静态单声道缓冲区
                        int stereo_samples = mp3_frame_info_.outputSamps;
                        int mono_samples = stereo_samples / 2;
                        
                        // 记录通道转换
                        ESP_LOGD(TAG, "立体声转单声道 - 输入: %d 样本，输出: %d 样本", 
                                stereo_samples, mono_samples);
                        
                        // 混合左右声道 (L + R) / 2
                        for (int i = 0; i < mono_samples; ++i) {
                            int left = pcm_buffer_[i * 2];
                            int right = pcm_buffer_[i * 2 + 1];
                            mono_buffer_[i] = (int16_t)((left + right) / 2);
                        }
                        
                        final_pcm_data = mono_buffer_;
                        final_sample_count = mono_samples;
                        memory_pool_hits++;  // 使用静态缓冲区，命中内存池
                    } else if (mp3_frame_info_.nChans == 1) {
                        ESP_LOGD(TAG, "已经是单声道音频 - %d 样本", final_sample_count);
                        memory_pool_hits++;  // 已经是单声道，命中内存池
                    }
                    
                    // 创建AudioStreamPacket（使用内存池）
                    AudioStreamPacket* packet = AllocateAudioPacket();
                    if (!packet) {
                        ESP_LOGE(TAG, "❌ 音频包分配失败");
                        total_decode_failures++;
                        continue;
                    }
                    
                    // 记录分配类型
                    if (memory_pool_initialized_) {
                        pool_packets++;
                    } else {
                        dynamic_packets++;
                    }
                    
                    packet->sample_rate = mp3_frame_info_.samprate;
                    packet->frame_duration = 60;
                    packet->timestamp = 0;
                    
                    // 将int16_t PCM数据转换为uint8_t字节数组
                    size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                    total_pcm_bytes_sent += pcm_size_bytes;
                    
                    // 记录PCM数据详情
                    if (total_decode_success % 50 == 0) {
                        ESP_LOGI(TAG, "📦 PCM数据包 - 采样率: %d，样本数: %d，大小: %d 字节", 
                                packet->sample_rate, final_sample_count, pcm_size_bytes);
                    }
                    
                    // 复用payload缓冲区
                    if (packet->payload.capacity() < pcm_size_bytes) {
                        packet->payload.reserve(pcm_size_bytes * 2);  // 预分配2倍空间
                    }
                    packet->payload.resize(pcm_size_bytes);
                    memcpy(packet->payload.data(), final_pcm_data, pcm_size_bytes);

                    // 发送到Application的音频解码队列
                    app.AddAudioData(std::move(*packet));
                    total_played += pcm_size_bytes;
                    
                    // 记录发送到应用层
                    ESP_LOGD(TAG, "发送PCM数据到Application - 总播放: %d 字节", total_played);
                    
                    // 释放音频包（自动归还到内存池）
                    FreeAudioPacket(packet);
                    
                    // 打印播放进度（减少频率）
                    if (total_played % (256 * 1024) == 0) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
                        float play_speed = (total_played * 1000.0f) / (elapsed * 1024.0f); // KB/s
                        
                        ESP_LOGI(TAG, "📊 播放进度 - 已播放: %d 字节 (%.1f KB/s)", total_played, play_speed);
                        ESP_LOGI(TAG, "📊 缓冲区状态 - 大小: %d 字节，内存池包: %d，动态包: %d", 
                                buffer_size_.load(std::memory_order_relaxed), pool_packets, dynamic_packets);
                        
                        // 详细统计报告
                        ESP_LOGI(TAG, "📊 解码统计 - 成功: %d，失败: %d，尝试: %d", 
                                total_decode_success, total_decode_failures, total_decode_attempts);
                        ESP_LOGI(TAG, "📊 内存池统计 - 命中: %d，未命中: %d", 
                                memory_pool_hits, memory_pool_misses);
                    }
                } else {
                    ESP_LOGW(TAG, "⚠️ 解码成功但输出样本数为0");
                }
                
                // 🔥 新增：在成功处理完一帧后，若缓冲区数据充裕，主动让出 CPU
                if (buffer_size_.load(std::memory_order_relaxed) > 2048) {
                    taskYIELD();
                }
                
            } else {
                // 解码失败
                total_decode_failures++;
                memory_pool_misses++;
                
                ESP_LOGW(TAG, "❌ MP3解码失败，错误码: %d，剩余数据: %d 字节", decode_result, bytes_left);
                
                // 记录失败率
                if (total_decode_attempts > 0 && total_decode_attempts % 100 == 0) {
                    float failure_rate = (total_decode_failures * 100.0f) / total_decode_attempts;
                    ESP_LOGW(TAG, "解码失败率: %.1f%% (%d/%d)", 
                            failure_rate, total_decode_failures, total_decode_attempts);
                }
                
                // 跳过一些字节继续尝试
                if (bytes_left > 1) {
                    int skip_bytes = std::min(10, bytes_left / 2);  // 最多跳过10字节或一半数据
                    ESP_LOGW(TAG, "跳过 %d 字节继续尝试解码", skip_bytes);
                    read_ptr += skip_bytes;
                    bytes_left -= skip_bytes;
                } else {
                    bytes_left = 0;
                }
            }
        }

        // 在处理完一帧后，检查是否还有剩余数据可继续解码
        if (bytes_left > 0) {
            // 输入缓冲区还有数据，立即触发下一次处理
            signalDownload();
        }
    }

    // 播放结束时统计
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    // 计算播放速度
    float play_speed_kBps = 0;
    if (elapsed > 0) {
        play_speed_kBps = (total_played * 1000.0f) / (elapsed * 1024.0f); // KB/s
    }

    // 详细播放统计报告
    ESP_LOGI(TAG, "========== 音频流播放完成统计 ==========");
    ESP_LOGI(TAG, "1. 总播放字节数: %d 字节 (%.2f MB)", total_played, total_played / (1024.0f * 1024.0f));
    ESP_LOGI(TAG, "2. 播放耗时: %ld 毫秒 (%.1f 秒)", elapsed, elapsed / 1000.0f);
    ESP_LOGI(TAG, "3. 平均播放速度: %.1f KB/s", play_speed_kBps);
    ESP_LOGI(TAG, "4. 解码统计:");
    ESP_LOGI(TAG, "   - 总尝试解码: %d 次", total_decode_attempts);
    ESP_LOGI(TAG, "   - 成功解码: %d 次", total_decode_success);
    ESP_LOGI(TAG, "   - 解码失败: %d 次", total_decode_failures);
    ESP_LOGI(TAG, "   - 成功率: %.1f%%", 
            total_decode_attempts ? (total_decode_success * 100.0f) / total_decode_attempts : 0.0f);
    ESP_LOGI(TAG, "5. 缓冲区统计:");
    ESP_LOGI(TAG, "   - 缓冲区获取次数: %d 次", total_buffer_fetches);
    ESP_LOGI(TAG, "   - PCM字节发送: %d 字节", total_pcm_bytes_sent);
    ESP_LOGI(TAG, "6. 内存池统计:");
    ESP_LOGI(TAG, "   - 内存池包: %d", pool_packets);
    ESP_LOGI(TAG, "   - 动态分配包: %d", dynamic_packets);
    ESP_LOGI(TAG, "   - 内存池命中: %d", memory_pool_hits);
    ESP_LOGI(TAG, "   - 内存池未命中: %d", memory_pool_misses);
    ESP_LOGI(TAG, "7. 帧解码统计:");
    ESP_LOGI(TAG, "   - 总解码帧数: %d", total_frames_decoded_);
    ESP_LOGI(TAG, "   - 总播放时间: %lld 毫秒", current_play_time_ms_);
    ESP_LOGI(TAG, "8. 最终缓冲区大小: %d 字节", buffer_size_.load(std::memory_order_relaxed));
    ESP_LOGI(TAG, "9. 播放状态: %d", is_state_completed_.load());
    ESP_LOGI(TAG, "=========================================");
    
    Application::GetInstance().GetAudioService().is_voice_out_ = false;
    is_playing_ = false;
    
    ESP_LOGI(TAG, "🎵 音频流播放线程结束");
}


// 修改错误恢复函数
bool Esp32Music::RecoverFromDecodeError(uint8_t*& data, int& data_size) {
    if (data_size <= 1) return false;
    
    // 更激进的错误恢复：跳过更多字节
    int skip_amount = std::min(256, data_size / 2); // 跳过最多256字节或一半数据
    data += skip_amount;
    data_size -= skip_amount;
    
    ESP_LOGW(TAG, "Recovery: skipping %d bytes, remaining: %d", skip_amount, data_size);
    
    // 添加小延迟让系统恢复
    vTaskDelay(pdMS_TO_TICKS(10));
    
    return true;
}

// 添加更健壮的同步字查找
int Esp32Music::FindMp3SyncWordRobust(uint8_t* data, int data_size) {
    if (data_size < 4) return -1;
    
    // 首先尝试标准同步查找
    int sync_offset = MP3FindSyncWord(data, data_size);
    if (sync_offset >= 0) {
        return sync_offset;
    }
    
    // 如果标准查找失败，尝试更宽松的查找
    ESP_LOGW(TAG, "Standard sync find failed, trying robust search in %d bytes", data_size);
    
    // 查找任何可能的0xFF开头
    for (int i = 0; i < data_size - 3; i++) {
        if (data[i] == 0xFF && (data[i+1] & 0xE0) == 0xE0) {
            // 验证帧头的基本有效性
            if (IsLikelyValidMp3Header(data + i, data_size - i)) {
                ESP_LOGI(TAG, "Found robust sync at offset %d", i);
                return i;
            }
        }
    }
    
    return -1;
}

// 添加MP3帧头验证函数
bool Esp32Music::IsLikelyValidMp3Header(uint8_t* data, int data_size) {
    if (data_size < 4) return false;
    
    // 检查同步字
    if (data[0] != 0xFF || (data[1] & 0xE0) != 0xE0) {
        return false;
    }
    
    // 检查MPEG版本和层
    uint8_t version_bits = (data[1] >> 3) & 0x03;
    uint8_t layer_bits = (data[1] >> 1) & 0x03;
    
    // 有效的MPEG版本: 00=2.5, 01=reserved, 10=2, 11=1
    // 有效的层: 00=reserved, 01=3, 10=2, 11=1
    if (version_bits == 0x01 || layer_bits == 0x00) {
        return false; // 保留值，无效
    }
    
    // 检查采样率索引
    uint8_t sr_index = (data[2] >> 2) & 0x03;
    if (sr_index == 0x03) {
        return false; // 保留值
    }
    
    return true;
}




// 添加MP3解码器重置函数
void Esp32Music::ResetMp3Decoder() {
    if (mp3_decoder_initialized_ && mp3_decoder_) {
        // 如果MP3解码库支持重置函数，调用它
        // MP3Reset(mp3_decoder_);
        ESP_LOGI(TAG, "MP3 decoder reset");
    }
}

// 添加数据验证函数
bool Esp32Music::ValidateMp3Data(const uint8_t* data, size_t size) {
    if (size < 4) {
        ESP_LOGE(TAG, "Data too small for MP3 validation");
        return false;
    }
    
    // 检查MP3帧头
    for (size_t i = 0; i < size - 3; i++) {
        if (data[i] == 0xFF && (data[i+1] & 0xE0) == 0xE0) {
            ESP_LOGI(TAG, "Valid MP3 frame found at position %d", i);
            return true;
        }
    }
    
    ESP_LOGE(TAG, "No valid MP3 frame found in data");
    return false;
}


// 修改 ProcessAndSendPCMData 函数
bool Esp32Music::ProcessAndSendPCMData(int16_t* pcm_data, int sample_count) {
    if (!pcm_data || sample_count <= 0) {
        ESP_LOGE(TAG, "Invalid PCM data parameters");
        return false;
    }
    
    // 验证样本数量不会导致缓冲区溢出
    if (sample_count > 2304) {  // 最大MP3帧样本数
        ESP_LOGW(TAG, "Sample count too large: %d, truncating to 2304", sample_count);
        sample_count = 2304;
    }
    
    auto& app = Application::GetInstance();
    
    // 基本的帧信息有效性检查，防止除零错误
    if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
        ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                mp3_frame_info_.samprate, mp3_frame_info_.nChans);
        return false;
    }
    
    int16_t* final_pcm_data = pcm_data;
    int final_sample_count = sample_count;
    std::vector<int16_t> mono_buffer;

    // 如果是双通道，转换为单通道混合（与流式播放保持一致）
    if (mp3_frame_info_.nChans == 2) {
        // 双通道转单通道：将左右声道混合
        int stereo_samples = sample_count;  // 包含左右声道的总样本数
        int mono_samples = stereo_samples / 2;  // 实际的单声道样本数
        
        mono_buffer.resize(mono_samples);
        
        for (int i = 0; i < mono_samples; ++i) {
            // 混合左右声道 (L + R) / 2
            int left = pcm_data[i * 2];      // 左声道
            int right = pcm_data[i * 2 + 1]; // 右声道
            mono_buffer[i] = (int16_t)((left + right) / 2);
        }
        
        final_pcm_data = mono_buffer.data();
        final_sample_count = mono_samples;

        ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples", 
                stereo_samples, mono_samples);
    } else if (mp3_frame_info_.nChans == 1) {
        // 已经是单声道，无需转换
        ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
    } else {
        ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono", 
                mp3_frame_info_.nChans);
    }

    // 创建AudioStreamPacket（与流式播放保持一致）
    AudioStreamPacket packet;
    packet.sample_rate = mp3_frame_info_.samprate;
    packet.frame_duration = 60;  // 使用Application默认的帧时长
    packet.timestamp = 0;
    
    // 将int16_t PCM数据转换为uint8_t字节数组
    size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
    packet.payload.resize(pcm_size_bytes);

    memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);
    ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application", 
            final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);
    
    // 发送到Application的音频解码队列（使用流式播放中的相同方法）
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
    app.AddAudioData(std::move(packet));
    return true;
}
// 修改 ClearAudioBuffer 函数，确保安全清理
void Esp32Music::ClearAudioBuffer() {
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    
    ESP_LOGI(TAG, "Clearing audio buffer, current size: %ld, queue size: %d", 
             buffer_size_.load(std::memory_order_relaxed), audio_buffer_.size());
    
    // 安全地清理所有缓冲区
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        
        // if (chunk.data) {
        //     // 验证指针有效性（基本检查）
        //     if (chunk.size > 0 && chunk.size < 1024 * 1024) { // 合理的最大大小
        //         heap_caps_free(chunk.data);
        //         chunk.data = nullptr;
        //         chunk.size = 0;
        //     } else {
        //         ESP_LOGW(TAG, "Invalid chunk size during cleanup: %d", chunk.size);
        //     }
        // }
    }

    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared safely");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate() {
    // auto& board = Board::GetInstance();
    // auto codec = board.GetAudioCodec();
    // if (codec && codec->original_output_sample_rate() > 0 && 
    //     codec->output_sample_rate() != codec->original_output_sample_rate()) {
    //     ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz", 
    //             codec->output_sample_rate(), codec->original_output_sample_rate());
    //     if (codec->SetOutputSampleRate(-1)) {  // -1 表示重置到原始值
    //         ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
    //     } else {
    //         ESP_LOGW(TAG, "无法重置采样率到原始值");
    //     }
    // }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;
    
    // 确保不超过可用数据大小
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string& lyric_url) {
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());
    
    // 检查URL是否为空
    if (lyric_url.empty()) {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }
    
    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5;  // 最多允许5次重定向
    
    while (retry_count < max_retries && !success && redirect_count < max_redirects) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // 使用Board提供的HTTP客户端
        int connection_id = esp_random() % 10000;
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(connection_id);

        std::shared_ptr<Http> shared_http = std::move(http);
        http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
        http_client_->SetKeepAlive(true); // 启用 Keep-Alive

        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }
        
        // 设置请求头
        http_client_->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http_client_->SetHeader("Accept", "text/plain");
        
        // 打开GET连接
        if (!http_client_->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            retry_count++;
            continue;
        }
        
        // 检查HTTP状态码
        int status_code = http_client_->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);
        
        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http_client_->Close();
            retry_count++;
            continue;
        }
        
        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http_client_->Close();
            retry_count++;
            continue;
        }
        
        // 读取响应
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;
        
        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read lyric content");
        
        while (true) {
            bytes_read = http_client_->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Lyric HTTP read returned %d bytes", bytes_read); // 注释掉以减少日志输出
            
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;
                
                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0) {
                    ESP_LOGD(TAG, "Downloaded %d bytes so far", total_read);
                }
            } else if (bytes_read == 0) {
                // 正常结束，没有更多数据
                ESP_LOGD(TAG, "Lyric download completed, total bytes: %d", total_read);
                success = true;
                break;
            } else {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!lyric_content.empty()) {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }
        
        http_client_->Close();
        
        if (read_error) {
            retry_count++;
            continue;
        }
        
        // 如果成功读取数据，跳出重试循环
        if (success) {
            break;
        }
    }
    
    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries) {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }
    
    // 记录前几个字节的数据，帮助调试
    if (!lyric_content.empty()) {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Lyric content preview (%d bytes): %s", lyric_content.length(), preview.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to download lyrics or lyrics are empty");
        return false;
    }
    
    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Parsing lyrics content");
    
    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    lyrics_.clear();
    
    // 按行分割歌词内容
    std::istringstream stream(lyric_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // 跳过空行
        if (line.empty()) {
            continue;
        }
        
        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[') {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos) {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);
                
                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos) {
                    std::string left_part = tag_or_time.substr(0, colon_pos);
                    
                    // 检查冒号左边是否是时间（数字）
                    bool is_time_format = true;
                    for (char c : left_part) {
                        if (!isdigit(c)) {
                            is_time_format = false;
                            break;
                        }
                    }
                    
                    // 如果不是时间格式，跳过这一行（元数据标签）
                    if (!is_time_format) {
                        // 可以在这里处理元数据，例如提取标题、艺术家等信息
                        ESP_LOGD(TAG, "Skipping metadata tag: [%s]", tag_or_time.c_str());
                        continue;
                    }
                    
                    // 是时间格式，解析时间戳
                    try {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);
                        
                        // 安全处理歌词文本，确保UTF-8编码正确
                        std::string safe_lyric_text;
                        if (!content.empty()) {
                            // 创建安全副本并验证字符串
                            safe_lyric_text = content;
                            // 确保字符串以null结尾
                            safe_lyric_text.shrink_to_fit();
                        }
                        
                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));
                        
                        if (!safe_lyric_text.empty()) {
                            // 限制日志输出长度，避免中文字符截断问题
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] %s", timestamp_ms, log_text.c_str());
                        } else {
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] (empty)", timestamp_ms);
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "Failed to parse time: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }
    
    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());
    
    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    return !lyrics_.empty();
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "Lyric display thread started");
    
    if (!DownloadLyrics(current_lyric_url_)) {
        ESP_LOGE(TAG, "Failed to download or parse lyrics");
        is_lyric_running_ = false;
        return;
    }
    
    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    
    // 查找当前应该显示的歌词
    int new_lyric_index = -1;
    
    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
    
    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;  // 时间戳已超过当前时间
        }
    }
    
    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1) {
        new_lyric_index = -1;
    }
    
    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        
        auto& board = Board::GetInstance();
        // auto display = board.GetDisplay();
        // if (display) {
        //     std::string lyric_text;
            
        //     if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
        //         lyric_text = lyrics_[current_lyric_index_].second;
        //     }
            
        //     // 显示歌词
        //     display->SetChatMessage("lyric", lyric_text.c_str());
            
        //     ESP_LOGD(TAG, "Lyric update at %lldms: %s", 
        //             current_time_ms, 
        //             lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        // }
    }
}

/**************************************
 * 为主MCP服务，查询一级分类
 **************************************/
std::vector<categorie> Esp32Music::GetCategories(long catId) {
    std::vector<categorie> categories;

    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/wechat/work/cat/list?deviceId=" + GloableVar::device_id + "&catId=" + std::to_string(catId);
    } else {
        url += "wechat/work/cat/list?deviceId=" + GloableVar::device_id + "&catId=" + std::to_string(catId);
    }

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return categories;
    }

    // 检查响应状态码
    int status_code = http_client_->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http_client_->Close();
        return categories;
    }

    // 读取响应数据
    std::string response = http_client_->ReadAll();
    http_client_->Close();
    
    ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d", status_code, response.length());
    ESP_LOGD(TAG, "Categories response: %s", response.c_str());

    // 解析返回结果
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return categories;
    }

    // 检查retcode
    cJSON* retcode = cJSON_GetObjectItem(root, "retcode");
    if (!cJSON_IsNumber(retcode) || retcode->valueint != 0) {
        ESP_LOGE(TAG, "API error, retcode: %d", cJSON_IsNumber(retcode) ? retcode->valueint : -1);
        cJSON_Delete(root);
        return categories;
    }

    // 获取data对象
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data) {
        ESP_LOGE(TAG, "No data in response");
        cJSON_Delete(root);
        return categories;
    }

    // 获取cats数组
    cJSON* cats = cJSON_GetObjectItem(data, "cats");
    if (!cats || !cJSON_IsArray(cats)) {
        ESP_LOGE(TAG, "No cats array in response");
        cJSON_Delete(root);
        return categories;
    }
    
    // 遍历cats数组，解析每个分类
    int array_size = cJSON_GetArraySize(cats);
    ESP_LOGI(TAG, "Found %d categories", array_size);

    for (int i = 0; i < array_size; i++) {
        cJSON* cat_item = cJSON_GetArrayItem(cats, i);
        if (!cat_item) continue;
        
        categorie cat;
        
        // 解析id
        cJSON* id_item = cJSON_GetObjectItem(cat_item, "id");
        if (cJSON_IsNumber(id_item)) {
            cat.id = id_item->valueint;
        }
        
        // 解析name
        cJSON* name_item = cJSON_GetObjectItem(cat_item, "name");
        if (cJSON_IsString(name_item) && name_item->valuestring) {
            cat.name = name_item->valuestring;
        }
        
        // 将解析的分类添加到向量中
        categories.push_back(cat);
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Successfully parsed %d categories", categories.size());
    return categories;
}

/**************************************
 * 为主MCP服务，查询故事分类
 * 下的作品
 **************************************/
std::vector<works> Esp32Music::GetWorksByCategory(long storyCategoryId, int maxExamples) {
    std::vector<works> works_list;

    std::vector<categorie> categories = GetCategories(storyCategoryId);
    if (categories.empty()) {
        ESP_LOGE(TAG, "No categories found for category ID: %ld", storyCategoryId);
        return works_list;
    }

    int randi = std::rand() % categories.size();
    long storyCategoryId2 = categories.at(randi).id;

    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/wechat/work/list?deviceId=" + GloableVar::device_id + "&catId=" + std::to_string(storyCategoryId);
    } else {
        url += "wechat/work/list?deviceId=" + GloableVar::device_id + "&catId=" + std::to_string(storyCategoryId);
    }

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return works_list;
    }

    // 检查响应状态码
    int status_code = http_client_->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http_client_->Close();
        return works_list;
    }

    // 参照 Ota::Upgrade 函数的分块读取方法
    std::string response;
    char buffer[512]; // 使用与 Upgrade 函数相同的缓冲区大小
    size_t total_read = 0;
    int64_t last_calc_time = esp_timer_get_time();
    size_t recent_read = 0;
    bool read_success = true;

    // 开始分块读取循环
    while (true) {
        int bytes_read = http_client_->Read(buffer, sizeof(buffer));
        
        if (bytes_read < 0) {
            // 读取错误，但可能已经读取到部分数据
            ESP_LOGE(TAG, "HTTP read error: %d, total read so far: %u", bytes_read, total_read);
            read_success = false;
            break;
        }
        
        if (bytes_read > 0) {
            // 成功读取数据
            response.append(buffer, bytes_read);
            total_read += bytes_read;
            recent_read += bytes_read;
            
            // 每秒打印一次进度
            if (esp_timer_get_time() - last_calc_time >= 1000000) { // 1秒
                ESP_LOGD(TAG, "Read %u bytes, total: %u", recent_read, total_read);
                last_calc_time = esp_timer_get_time();
                recent_read = 0;
            }
        }
        
        if (bytes_read == 0) {
            // 正常读取结束
            ESP_LOGI(TAG, "HTTP read completed, total: %u bytes", total_read);
            break;
        }
    }
    
    http_client_->Close();

    // 即使读取有错误，但已经读取到一些数据，也尝试处理
    if (!response.empty()) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d", status_code, response.length());
        ESP_LOGD(TAG, "Works response: %s", response.c_str());

        // 解析返回结果
        cJSON* root = cJSON_Parse(response.c_str());
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse JSON response");
            ESP_LOGD(TAG, "Raw response (first 500 chars): %s", 
                        response.substr(0, std::min(response.size(), size_t(500))).c_str());
            return works_list;
        }
        
        // 检查retcode
        cJSON* retcode = cJSON_GetObjectItem(root, "retcode");
        if (!cJSON_IsNumber(retcode) || retcode->valueint != 0) {
            ESP_LOGE(TAG, "API error, retcode: %d", cJSON_IsNumber(retcode) ? retcode->valueint : -1);
            cJSON_Delete(root);
            return works_list;
        }
        
        // 获取data对象
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (!data) {
            ESP_LOGE(TAG, "No data in response");
            cJSON_Delete(root);
            return works_list;
        }
        
        // 获取works数组
        cJSON* works_array = cJSON_GetObjectItem(data, "works");
        if (!works_array || !cJSON_IsArray(works_array)) {
            ESP_LOGE(TAG, "No works array in response");
            cJSON_Delete(root);
            return works_list;
        }
        
        // 遍历works数组，解析每个作品
        int array_size = cJSON_GetArraySize(works_array);
        ESP_LOGI(TAG, "Found %d works", array_size);
        
        // 将所有作品解析到临时列表
        std::vector<works> all_works;
        
        for (int i = 0; i < array_size; i++) {
            cJSON* work_item = cJSON_GetArrayItem(works_array, i);
            if (!work_item) continue;
            
            works work;
            
            // 解析id
            cJSON* id_item = cJSON_GetObjectItem(work_item, "id");
            if (cJSON_IsString(id_item) && id_item->valuestring) {
                work.id = id_item->valuestring;
            }
            
            // 解析name
            cJSON* name_item = cJSON_GetObjectItem(work_item, "name");
            if (cJSON_IsString(name_item) && name_item->valuestring) {
                work.name = name_item->valuestring;
            }
            
            all_works.push_back(work);
        }
        
        cJSON_Delete(root);
        
        // 如果作品数量超过maxExamples，随机选择maxExamples个
        if (all_works.size() > (size_t)maxExamples) {
            // 随机选择作品
            for (int i = 0; i < maxExamples; i++) {
                // 使用ESP32硬件随机数
                uint32_t random_val = esp_random();
                int random_index = random_val % all_works.size();
                works_list.push_back(all_works[random_index]);
                
                // 从all_works中移除已选择的，避免重复
                all_works.erase(all_works.begin() + random_index);
            }
        } else {
            // 如果作品数量不足，返回所有作品
            works_list = all_works;
        }
        
        ESP_LOGI(TAG, "Successfully selected %d works", works_list.size());
        return works_list;
    }
    return works_list;
}

/**********************************************
 * 支持MCP的讲哪些故事，讲什么故事
 * 既列举分类又列举故事的名字
 **********************************************/
std::string Esp32Music::getStoryCategoriesWithExamples() {
    int max_examples = 5;
    
    // 获取所有一级分类
    std::vector<categorie> categories = GetCategories(-1); // -1表示获取一级分类
    if (categories.empty()) {
        return "{\"success\": false, \"message\": \"获取分类信息失败\"}";
    }
    
    // 获取故事分类下的示例作品
    std::vector<works> story_examples;
    int story_category_id = -1;
    
    // 找到故事分类ID
    for (const auto& cat : categories) {
        if (cat.name == "故事") {
            story_category_id = cat.id;
            break;
        }
    }
    
    // 获取故事示例
    if (story_category_id != -1) {
        story_examples = GetWorksByCategory(story_category_id, max_examples);
    }
    
    // 构建响应JSON
    std::string result = "{\"success\": true, \"data\": {";
    
    // 1. 添加分类列表
    result += "\"categories\": [";
    for (size_t i = 0; i < categories.size(); ++i) {
        result += "{";
        result += "\"id\": " + std::to_string(categories[i].id) + ",";
        result += "\"name\": \"" + categories[i].name + "\"";
        result += "}";
        if (i < categories.size() - 1) {
            result += ",";
        }
    }
    result += "],";
    
    // 2. 添加故事示例
    result += "\"story_examples\": [";
    for (size_t i = 0; i < story_examples.size(); ++i) {
        result += "{";
        result += "\"name\": \"" + story_examples[i].name + "\"";
        result += "}";
        if (i < story_examples.size() - 1) {
            result += ",";
        }
    }
    result += "]";
    
    result += "}}";
    
    return result;
}

/**********************************************
 * 支持MCP的唱哪些儿歌，唱什么儿歌
 * 既列举分类又列举儿歌的名字
 **********************************************/
std::string Esp32Music::getSongCategoriesWithExamples() {
    int max_examples = 5;
    
    // 获取所有一级分类
    std::vector<categorie> categories = GetCategories(-1); // -1表示获取一级分类
    if (categories.empty()) {
        return "{\"success\": false, \"message\": \"获取分类信息失败\"}";
    }

    // 获取儿歌分类下的示例作品
    std::vector<works> song_examples;
    int song_category_id = -1;
    
    // 找到儿歌分类ID
    for (const auto& cat : categories) {
        if (cat.name == "儿歌") {
            song_category_id = cat.id;
            break;
        }
    }
    
    // 获取儿歌示例
    if (song_category_id != -1) {
        song_examples = GetWorksByCategory(song_category_id, max_examples);
    }

    // 构建响应JSON
    std::string result = "{\"success\": true, \"data\": {";
    
    // 1. 添加分类列表
    result += "\"categories\": [";
    for (size_t i = 0; i < categories.size(); ++i) {
        result += "{";
        result += "\"id\": " + std::to_string(categories[i].id) + ",";
        result += "\"name\": \"" + categories[i].name + "\"";
        result += "}";
        if (i < categories.size() - 1) {
            result += ",";
        }
    }
    result += "],";

    // 2. 添加儿歌示例
    result += "\"song_examples\": [";
    for (size_t i = 0; i < song_examples.size(); ++i) {
        result += "{";
        result += "\"name\": \"" + song_examples[i].name + "\"";
        result += "}";
        if (i < song_examples.size() - 1) {
            result += ",";
        }
    }
    result += "]";
    
    result += "}}";
    
    return result;
}

/**************************************
 * 为主MCP服务，查询故事分类
 * 下的随机作品
 **************************************/
std::string Esp32Music::GetWorkByCategory(long storyCategoryId) {
    std::vector<categorie> categories = GetCategories(storyCategoryId);

    int randi = std::rand() % categories.size();
    long storyCategoryId2 = categories.at(randi).id;

    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/wechat/work/list?deviceId=" + GloableVar::device_id + "&catId=" + std::to_string(storyCategoryId);
    } else {
        url += "wechat/work/list?deviceId=" + GloableVar::device_id + "&catId=" + std::to_string(storyCategoryId);
    }

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive

    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        http_client_->Close();
        return "";
    }

    // 检查响应状态码
    int status_code = http_client_->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http_client_->Close();
        return "";
    }

    // 参考 Ota::Upgrade 函数的分块读取方法
    std::string response;
    char buffer[512]; // 使用与 Upgrade 函数相同的缓冲区大小
    size_t total_read = 0;
    int64_t last_calc_time = esp_timer_get_time();
    size_t recent_read = 0;
    bool read_success = true;

    // 开始分块读取循环
    while (true) {
        int bytes_read = http_client_->Read(buffer, sizeof(buffer));
        
        if (bytes_read < 0) {
            // 读取错误，但可能已经读取到部分数据
            ESP_LOGE(TAG, "HTTP read error: %d, total read so far: %u", bytes_read, total_read);
            read_success = false;
            break;
        }
        
        if (bytes_read > 0) {
            // 成功读取数据
            response.append(buffer, bytes_read);
            total_read += bytes_read;
            recent_read += bytes_read;
            
            // 每秒打印一次进度
            if (esp_timer_get_time() - last_calc_time >= 1000000) { // 1秒
                ESP_LOGI(TAG, "Read %u bytes, total: %u", recent_read, total_read);
                last_calc_time = esp_timer_get_time();
                recent_read = 0;
            }
        }
        
        if (bytes_read == 0) {
            // 正常读取结束
            ESP_LOGI(TAG, "HTTP read completed, total: %u bytes", total_read);
            break;
        }
    }
    
    http_client_->Close();

    // 即使读取有错误，但已经读取到一些数据，也尝试处理
    if (!response.empty()) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d", status_code, response.length());
        
        // 检查响应数据是否有效
        if (response.find("\"retcode\"") == std::string::npos) {
            ESP_LOGW(TAG, "Response may not be valid JSON, retrying");
            ESP_LOGD(TAG, "Response preview (first 200 chars): %s", 
                        response.substr(0, std::min(response.size(), size_t(200))).c_str());
            return "";
        }
        
        // 尝试解析JSON验证有效性
        cJSON* test_root = cJSON_Parse(response.c_str());
        if (!test_root) {
            ESP_LOGW(TAG, "Failed to parse JSON, but we have data, will try to parse anyway");
            ESP_LOGD(TAG, "Response data (first 300 chars): %s", 
                        response.substr(0, std::min(response.size(), size_t(300))).c_str());
            return "";
        }
        
        cJSON_Delete(test_root);
        
        // 数据看起来有效，继续处理
        ESP_LOGD(TAG, "Works response: %s", response.c_str());

        // 解析返回结果
        cJSON* root = cJSON_Parse(response.c_str());
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse JSON response");
            ESP_LOGD(TAG, "Raw response (first 500 chars): %s", 
                        response.substr(0, std::min(response.size(), size_t(500))).c_str());
            return "";
        }
        
        // 检查retcode
        cJSON* retcode = cJSON_GetObjectItem(root, "retcode");
        if (!cJSON_IsNumber(retcode) || retcode->valueint != 0) {
            ESP_LOGE(TAG, "API error, retcode: %d", cJSON_IsNumber(retcode) ? retcode->valueint : -1);
            cJSON_Delete(root);
            return "";
        }
        
        // 获取data对象
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (!data) {
            ESP_LOGE(TAG, "No data in response");
            cJSON_Delete(root);
            return "";
        }
        
        // 获取works数组
        cJSON* works_array = cJSON_GetObjectItem(data, "works");
        if (!works_array || !cJSON_IsArray(works_array)) {
            ESP_LOGE(TAG, "No works array in response");
            cJSON_Delete(root);
            return "";
        }
        
        // 遍历works数组，解析每个作品
        int array_size = cJSON_GetArraySize(works_array);
        ESP_LOGI(TAG, "Found %d works", array_size);
        
        // 将所有作品解析到临时列表
        std::vector<works> all_works;
        
        for (int i = 0; i < array_size; i++) {
            cJSON* work_item = cJSON_GetArrayItem(works_array, i);
            if (!work_item) continue;
            
            works work;
            
            // 解析id
            cJSON* id_item = cJSON_GetObjectItem(work_item, "id");
            if (cJSON_IsString(id_item) && id_item->valuestring) {
                work.id = id_item->valuestring;
            }
            
            // 解析name
            cJSON* name_item = cJSON_GetObjectItem(work_item, "name");
            if (cJSON_IsString(name_item) && name_item->valuestring) {
                work.name = name_item->valuestring;
            }
            
            all_works.push_back(work);
        }
        
        cJSON_Delete(root);
        
        if (all_works.empty()) {
            ESP_LOGW(TAG, "No works found in response, retrying");
            return "";
        }

        int rand_work = std::rand() % all_works.size();
        std::string work_name = all_works.at(rand_work).name;

        ESP_LOGI(TAG, "Successfully selected work: %s", work_name.c_str());
        return work_name;
    }

    ESP_LOGE(TAG, "Failed to get work by category after 1 attempts");
    return "";
}

/**********************************************
 * 支持MCP随机讲故事和唱儿歌
 **********************************************/
bool Esp32Music::PlayRandomByCategory(std::string content) {
    // 获取所有一级分类
    std::vector<categorie> categories = GetCategories(-1); // -1表示获取一级分类
    if (categories.empty()) {
        return false;
    }

    int story_category_id = -1;
    
    // 找到故事分类ID
    for (const auto& cat : categories) {
        if (cat.name == content) {
            story_category_id = cat.id;
            break;
        }
    }

    // 获取故事名字
    std::string rand_story;
    if (story_category_id != -1) {
        rand_story = GetWorkByCategory(story_category_id);
    }

    if (rand_story == "") {
        return false;
    }
    else {
        return Download(rand_story, false);
    }
}

void Esp32Music::resetStreamingEvents() {
    if (streaming_event_group_) {
        // 清除所有事件位
        xEventGroupClearBits(streaming_event_group_, 
            EVENT_DOWNLOAD_COMPLETE | EVENT_PLAY_COMPLETE | 
            EVENT_STREAMING_ERROR | EVENT_STREAMING_STOPPED);
        
        ESP_LOGI(TAG, "流式事件已重置");
    }
}

void Esp32Music::resetEvents() {
    if (event_group_) {
        // 清除所有事件位
        xEventGroupClearBits(event_group_,  PLAY_EVENT_SCHEDULE);
        xEventGroupClearBits(event_group_, WIFI_EVENT_SCHEDULE);
        
        ESP_LOGI(TAG, "流式事件已重置");
    }
}

Esp32Music::StreamingResult Esp32Music::waitForStreamingCompletion(uint32_t timeout_ms) {
    if (!streaming_event_group_) {
        ESP_LOGE(TAG, "Event group not initialized");
        return StreamingResult::ERROR;
    }
    
    ESP_LOGI(TAG, "等待流式播放完成 (超时: %u ms)", timeout_ms);
    
    // 🔥 修改：等待下载完成和播放完成两个事件
    EventBits_t bits = xEventGroupWaitBits(
        streaming_event_group_,        // 事件组句柄
        EVENT_DOWNLOAD_COMPLETE | EVENT_PLAY_COMPLETE,  // 等待这两个事件
        pdTRUE,                        // 等待后清除这些位
        pdTRUE,                        // 🔥 修改：等待ALL位（两个都发生）
        timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY
    );
    
    // 检查错误和停止事件
    EventBits_t error_bits = xEventGroupGetBits(streaming_event_group_);
    
    if (error_bits & EVENT_STREAMING_ERROR) {
        ESP_LOGE(TAG, "流式播放错误");
        return StreamingResult::ERROR;
    }
    
    if (error_bits & EVENT_STREAMING_STOPPED) {
        ESP_LOGW(TAG, "流式播放被外部停止");
        return StreamingResult::STOPPED;
    }
    
    if ((bits & (EVENT_DOWNLOAD_COMPLETE | EVENT_PLAY_COMPLETE)) == 
        (EVENT_DOWNLOAD_COMPLETE | EVENT_PLAY_COMPLETE)) {
        // 两个事件都发生了
        ESP_LOGI(TAG, "流式播放正常完成（下载和播放都完成）");
        return StreamingResult::SUCCESS;
    }
    else if (bits & EVENT_DOWNLOAD_COMPLETE) {
        // 只有下载完成，播放未完成
        ESP_LOGW(TAG, "只有下载完成，播放未完成");
        return StreamingResult::STOPPED;
    }
    else if (bits & EVENT_PLAY_COMPLETE) {
        // 只有播放完成，下载未完成（不应该发生）
        ESP_LOGW(TAG, "只有播放完成，下载未完成");
        return StreamingResult::ERROR;
    }
    else if (timeout_ms > 0 && bits == 0) {
        // 超时
        ESP_LOGW(TAG, "流式播放超时，强制停止");
        return StreamingResult::TIMEOUT;
    }
    
    ESP_LOGE(TAG, "未知的流式播放完成状态 (bits: 0x%x)", bits);
    return StreamingResult::ERROR;
}

void Esp32Music::signalDownload() {
    if (event_group_) {
        xEventGroupSetBits(event_group_, PLAY_EVENT_SCHEDULE);
    }
}

void Esp32Music::signalWIFIError() {
    if (event_group_) {
        xEventGroupSetBits(event_group_, WIFI_EVENT_SCHEDULE);
    }
}

void Esp32Music::signalDownloadComplete() {
    if (streaming_event_group_) {
        ESP_LOGI(TAG, "Signaling download complete");
        xEventGroupSetBits(streaming_event_group_, EVENT_DOWNLOAD_COMPLETE);
    }
}

void Esp32Music::signalPlayComplete() {
    if (streaming_event_group_) {
        ESP_LOGI(TAG, "Signaling play complete");
        xEventGroupSetBits(streaming_event_group_, EVENT_PLAY_COMPLETE);
    }
}

void Esp32Music::signalStreamingError() {
    if (streaming_event_group_) {
        ESP_LOGW(TAG, "Signaling streaming error");
        xEventGroupSetBits(streaming_event_group_, EVENT_STREAMING_ERROR);
    }
}

void Esp32Music::signalStreamingStopped() {
    if (streaming_event_group_) {
        ESP_LOGW(TAG, "Signaling streaming stopped");
        xEventGroupSetBits(streaming_event_group_, EVENT_STREAMING_STOPPED);
    }
}
