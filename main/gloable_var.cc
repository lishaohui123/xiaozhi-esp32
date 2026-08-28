#include "gloable_var.h"
#include "ota.h"
#include "esp_system.h"
#include <sstream>
#include <iomanip>
#include <string>
#include "mbedtls/sha1.h"
#include <sstream>
#include <iomanip>
#include <string>
#include "esp_sntp.h"
#include <ctime>
#include <esp_log.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include "system_info.h"
#include "constants.h"


#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "GloableVar"

std::string GloableVar::work_url;
std::string GloableVar::app_id;
std::string GloableVar::app_secret;
std::string GloableVar::device_id;
std::string GloableVar::work_detail_url;
volatile int GloableVar::mode_realtime;
int GloableVar::volume;
std::string GloableVar::mqtt_user_name;
std::string GloableVar::mqtt_password;

// 定义新增的TTS静态变量
std::string GloableVar::tts_api_url;
std::string GloableVar::tts_api_key;
std::string GloableVar::alarm_app_id;
std::string GloableVar::token;
std::string GloableVar::cluster;
std::string GloableVar::voice_type;

std::shared_ptr<HttpClient> GloableVar::http_client_ = nullptr;

std::string GloableVar::generate_uuid() {
    // 生成32个十六进制字符的UUID
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    
    for (int i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            ss << "-";
        }
        uint32_t random_byte = esp_random() & 0xF; // 获取0-15的随机数
        ss << random_byte;
    }
    
    return ss.str();
}

std::string GloableVar::generate_sha1_signature(const std::string& app_id, 
                                   const std::string& app_secret, 
                                   const std::string& timestamp) {
    // 拼接字符串：app_id + app_secret + timestamp
    std::string sign_content = app_id + app_secret + timestamp;
    
    unsigned char hash[20]; // SHA1 produces 20-byte hash
    mbedtls_sha1_context ctx;
    
    // 初始化SHA1上下文
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    
    // 计算哈希
    mbedtls_sha1_update(&ctx, 
                       reinterpret_cast<const unsigned char*>(sign_content.c_str()), 
                       sign_content.length());
    
    // 完成哈希计算
    mbedtls_sha1_finish(&ctx, hash);
    mbedtls_sha1_free(&ctx);
    
    // 将二进制哈希转换为十六进制字符串
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 20; i++) {
        ss << std::setw(2) << static_cast<unsigned int>(hash[i]);
    }
    
    return ss.str();
}

// 初始化NTP时间同步
void GloableVar::init_ntp_time() {
    ESP_LOGI(TAG, "Initializing NTP time synchronization");
    
    // 设置时区为东八区（北京时间）
    if (getenv("TZ") == nullptr) {
        setenv("TZ", "CST-8", 1);  // 中国标准时间，UTC+8
        tzset();
    }
    
    // 设置NTP服务器 - 使用更稳定的国内NTP服务器
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // 主用：国内稳定的NTP服务器
    sntp_setservername(0, "ntp.aliyun.com");          // 阿里云NTP
    sntp_setservername(1, "cn.pool.ntp.org");         // 中国NTP池
    sntp_setservername(2, "time.windows.com");        // Windows时间服务器
    sntp_setservername(3, "ntp.tuna.tsinghua.edu.cn"); // 清华大学NTP
    
    // 设置同步模式为立即同步
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    
    // 设置同步间隔（单位：毫秒）
    sntp_set_time_sync_notification_cb([](struct timeval *tv) {
        ESP_LOGI(TAG, "Time synchronized via NTP");
        
        // 打印同步后的时间
        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        
        ESP_LOGI(TAG, "Current local time: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    });
    
    sntp_init();
    
    // 增加等待时间和重试次数
    int retry = 0;
    const int retry_count = 15;  // 增加重试次数
    bool sync_success = false;
    
    while (retry < retry_count) {
        auto sync_status = sntp_get_sync_status();
        
        if (sync_status == SNTP_SYNC_STATUS_COMPLETED) {
            sync_success = true;
            break;
        }
        
        ESP_LOGI(TAG, "Waiting for time synchronization... (%d/%d), status: %d", 
                 retry + 1, retry_count, sync_status);
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        retry++;
        
        // 如果多次重试失败，尝试重新初始化
        // if (retry == 5 && sync_status == SNTP_SYNC_STATUS_RESET) {
        //     ESP_LOGW(TAG, "Re-initializing NTP client");
        //     sntp_stop();
        //     vTaskDelay(1000 / portTICK_PERIOD_MS);
        //     sntp_init();
        // }
    }
    
    if (sync_success) {
        // 获取并显示同步后的时间
        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        
        ESP_LOGI(TAG, "Time synchronized successfully");
        ESP_LOGI(TAG, "Local time: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        
        // 验证时间是否合理（年份应该在2020年以后）
        if (timeinfo.tm_year + 1900 < 2020) {
            ESP_LOGW(TAG, "Synchronized time seems incorrect, year: %d", timeinfo.tm_year + 1900);
        }
    } else {
        ESP_LOGE(TAG, "Failed to synchronize time after %d attempts", retry_count);
        
        // 设置一个默认时间作为后备
        struct timeval tv = {
            .tv_sec = 1700000000,  // 2023-11-14 左右的默认时间
            .tv_usec = 0
        };
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "Set default time due to NTP synchronization failure");
    }
}

int GloableVar::update_gloable_var(int volume) {
    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/var/set";
    } else {
        url += "var/set";
    }
    url += "?deviceId=" + SystemInfo::GetMacAddress() + "&volume=" + std::to_string(volume);

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true);  // 启用 Keep-Alive

    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "update gloable var successful");
    return ESP_OK;
}

int GloableVar::get_gloable_var() {
    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/var";
    } else {
        url += "var";
    }
    url += "?deviceId=" + SystemInfo::GetMacAddress();

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true);  // 启用 Keep-Alive

    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }

    auto status_code = http_client_->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        return ESP_FAIL;
    }

    std::string data = http_client_->ReadAll();
    http_client_->Close();
    ESP_LOGD(TAG, "response=%s", data.c_str());

    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }
    
    // 解析workUrl
    cJSON* workUrl = cJSON_GetObjectItem(root, "workUrl");
    if (cJSON_IsString(workUrl) && workUrl->valuestring) {
        work_url = workUrl->valuestring;
    }
    
    // 解析appId
    cJSON* appId = cJSON_GetObjectItem(root, "appId");
    if (cJSON_IsString(appId) && appId->valuestring) {
        app_id = appId->valuestring;
    }
    
    // 解析appSecret
    cJSON* appSecret = cJSON_GetObjectItem(root, "appSecret");
    if (cJSON_IsString(appSecret) && appSecret->valuestring) {
        app_secret = appSecret->valuestring;
    }
    
    // 解析deviceId
    device_id = SystemInfo::GetMacAddress();
    
    // 解析workDetailUrl
    cJSON* workDetailUrl = cJSON_GetObjectItem(root, "workDetailUrl");
    if (cJSON_IsString(workDetailUrl) && workDetailUrl->valuestring) {
        work_detail_url = workDetailUrl->valuestring;
    }

    // 解析实时打断使能
    cJSON* modeRealtime = cJSON_GetObjectItem(root, "modeRealtime");
    if (cJSON_IsNumber(modeRealtime)) {
        mode_realtime = modeRealtime->valueint;   // 获取整数值
    }

    // 解析用户在页面上设置的音量
    cJSON* volume_item = cJSON_GetObjectItem(root, "volume");
    if (cJSON_IsNumber(volume_item)) {
        volume = volume_item->valueint;   // 获取整数值
    }

    cJSON* mqttUserName = cJSON_GetObjectItem(root, "mqttUserName");
    if (cJSON_IsString(mqttUserName) && mqttUserName->valuestring) {
        mqtt_user_name = mqttUserName->valuestring;
    }

    cJSON* mqttPassword = cJSON_GetObjectItem(root, "mqttPassword");
    if (cJSON_IsString(mqttPassword) && mqttPassword->valuestring) {
        mqtt_password = mqttPassword->valuestring;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Get gloable var successful");
    return ESP_OK;
}

int GloableVar::get_alarm_var() {
    Ota ota;
    // std::string url = ota.GetCheckVersionUrl();
    std::string url = OTA_URI;
    if (url.back() != '/') {
        url += "/alarm/var";
    } else {
        url += "alarm/var";
    }

    auto http = std::unique_ptr<Http>(ota.SetupHttp());

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive
    
    if (!http_client_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }

    auto status_code = http_client_->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        return ESP_FAIL;
    }

    std::string data = http_client_->ReadAll();
    http_client_->Close();
    ESP_LOGD(TAG, "response=%s", data.c_str());

    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }
    
    // 解析ttsApiUrl
    cJSON* ttsApiUrl = cJSON_GetObjectItem(root, "ttsApiUrl");
    if (cJSON_IsString(ttsApiUrl) && ttsApiUrl->valuestring) {
        tts_api_url = ttsApiUrl->valuestring;
    }
    
    // 解析ttsApiKey
    cJSON* ttsApiKey = cJSON_GetObjectItem(root, "ttsApiKey");
    if (cJSON_IsString(ttsApiKey) && ttsApiKey->valuestring) {
        tts_api_key = ttsApiKey->valuestring;
    }
    
    // 解析appId
    cJSON* appId = cJSON_GetObjectItem(root, "appId");
    if (cJSON_IsString(appId) && appId->valuestring) {
        alarm_app_id = appId->valuestring;
    }
    
    // 解析token
    cJSON* token_c = cJSON_GetObjectItem(root, "token");
    if (cJSON_IsString(token_c) && token_c->valuestring) {
        token = token_c->valuestring;
    }

    // 解析cluster
    cJSON* cluster_c = cJSON_GetObjectItem(root, "cluster");
    if (cJSON_IsString(cluster_c) && cluster_c->valuestring) {
        cluster = cluster_c->valuestring;
    }

    // 解析voiceType
    cJSON* voiceType = cJSON_GetObjectItem(root, "voiceType");
    if (cJSON_IsString(voiceType) && voiceType->valuestring) {
        voice_type = voiceType->valuestring;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Get alarm var successful");
    return ESP_OK;
}
