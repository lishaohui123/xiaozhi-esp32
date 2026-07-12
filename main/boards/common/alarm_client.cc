#include "alarm_client.h"
#include "gloable_var.h"
#include "system_info.h"
#include "constants.h"

#include <esp_log.h>
#include <cJSON.h>
#include <ctime>
#include <sstream>
#include <chrono>
#include <thread>

#define TAG "AlarmClient"
#define MAX_RETRIES 1
#define RETRY_DELAY_MS 1000

AlarmClient& AlarmClient::GetInstance() {
    static AlarmClient instance;
    return instance;
}

AlarmClient::AlarmClient() : initialized_(false) {
}

AlarmClient::~AlarmClient() {
}

bool AlarmClient::Initialize(const std::string& api_url, const std::string& device_id) {
    if (api_url.empty() || device_id.empty()) {
        ESP_LOGE(TAG, "API URL or device ID is empty");
        return false;
    }
    
    api_url_ = api_url;
    device_id_ = device_id;
    initialized_ = true;
    
    ESP_LOGI(TAG, "DB client initialized with API URL: %s, device ID: %s", 
            api_url_.c_str(), device_id_.c_str());
    
    return true;
}

std::string AlarmClient::SendRequest(const std::string& path, const std::string& method, const std::string& body) {
    if (!initialized_) {
        ESP_LOGE(TAG, "DB client not initialized");
        return "{\"success\": false, \"message\": \"DB client not initialized\"}";
    }
    
    auto unique_http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!unique_http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return "{\"success\": false, \"message\": \"Failed to create HTTP client\"}";
    }

    std::shared_ptr<Http> shared_http = std::move(unique_http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true);  // 启用 Keep-Alive

    // 设置请求头
    http_client_->SetHeader("Content-Type", "application/json");
    http_client_->SetHeader("Device-ID", device_id_);

    http_client_->SetHeader("token", ACCESS_TOKEN);
        
    // 构建完整URL
    std::string url = api_url_ + path;
    ESP_LOGI(TAG, "Sending %s request to: %s", method.c_str(), url.c_str());
        
    // 设置请求体
    std::string body_str = body;
    if (!body.empty()) {
        ESP_LOGD(TAG, "Request body: %s", body.c_str());
        http_client_->SetContent(std::move(body_str));
    }
        
    // 发送请求
    if (!http_client_->Open(method.c_str(), url.c_str())) {
        ESP_LOGE(TAG, "Failed to open connection to %s", url.c_str());
        return "{\"success\": false, \"message\": \"Failed to open connection\"}";
    }
        
    // 分块读取响应体
    std::string response;
    char buffer[256];
    while (true) {
        int ret = http_client_->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read response body: %s", esp_err_to_name(ret));
            break;
        }
        if (ret == 0) {
            break;
        }
        response.append(buffer, ret);
    }

    int status_code = http_client_->GetStatusCode();
        
    http_client_->Close();
    http_client_.reset();

    ESP_LOGI(TAG, "Response status code: %d", status_code);
        
    if (status_code >= 200 && status_code < 300) {
        return response;
    }
    return "{\"success\": false, \"message\": \"Max retries reached\"}";
}

std::vector<HolidayInfo> AlarmClient::GetHolidayData(int current_year) {
    std::string path = std::format("holiday/list?year={}", current_year);

    std::string json = SendRequest(path, "GET");

    std::vector<HolidayInfo> holidays;
    if (json.empty()) {
        ESP_LOGE(TAG, "Failed to get holiday data, empty response");
        return holidays;
    }

    // 解析JSON响应
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse holiday JSON");
        return holidays;
    }

    // 检查是否是数组
    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Holiday response is not an array");
        cJSON_Delete(root);
        return holidays;
    }

    // 遍历数组中的每个节假日
    int array_size = cJSON_GetArraySize(root);
    for (int i = 0; i < array_size; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (!item || !cJSON_IsObject(item)) {
            continue;
        }
        
        HolidayInfo holiday;
        
        // 解析各个字段
        cJSON* date = cJSON_GetObjectItem(item, "date");
        cJSON* type = cJSON_GetObjectItem(item, "type");
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* offDay = cJSON_GetObjectItem(item, "isOffDay");
        cJSON* description = cJSON_GetObjectItem(item, "description");
        
        // 确保必要字段存在
        if (!date || !type || !name || !offDay) {
            ESP_LOGW(TAG, "Incomplete holiday data, skipping");
            continue;
        }
        
        // 赋值
        if (cJSON_IsString(date)) {
            holiday.date = date->valuestring;
        }
        
        if (cJSON_IsString(type)) {
            holiday.type = type->valuestring;
        }
        
        if (cJSON_IsString(name)) {
            holiday.name = name->valuestring;
        }
        
        if (cJSON_IsNumber(offDay)) {
            holiday.is_off_day = (offDay->valueint != 0);
        }
        
        // 可选字段
        if (description && cJSON_IsString(description)) {
            // holiday.description = description->valuestring; // 如果HolidayInfo结构体有description字段
        }
        
        // 验证数据
        if (holiday.date.empty() || holiday.type.empty() || holiday.name.empty()) {
            ESP_LOGW(TAG, "Invalid holiday entry, skipping");
            continue;
        }
        
        // 添加到向量
        holidays.push_back(holiday);
        
        ESP_LOGD(TAG, "Loaded holiday: %s - %s (type: %s, offDay: %d)", 
                holiday.date.c_str(), holiday.name.c_str(), 
                holiday.type.c_str(), holiday.is_off_day);
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Successfully loaded %d holidays for year %d", holidays.size(), current_year);
    
    return holidays;
}

std::string AlarmClient::GetTips(long tipTemplateId) {
    std::string path = std::format("alarm/tipTemplateId?tipTemplateId={}", tipTemplateId);
    std::string json = SendRequest(path, "GET");

    cJSON* root = cJSON_Parse(json.c_str());
    std::string tips = cJSON_GetObjectItem(root, "tipDesc")->valuestring;
    cJSON_Delete(root);

    return tips;
}

std::string AlarmClient::GetAllAlarms() {
    std::string path = "alarms?deviceId=" + device_id_;
    return SendRequest(path, "GET");
}

std::string AlarmClient::GetAlarm(const std::string& id) {
    std::string path = "alarm/" + id;
    return SendRequest(path, "GET");
}

std::string AlarmClient::CreateAlarm(const std::string& time, const std::string& message,
                                 const std::string& weekdays, bool enabled, bool is_one_time) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceId", device_id_.c_str());
    cJSON_AddStringToObject(root, "time", time.c_str());
    cJSON_AddStringToObject(root, "message", message.c_str());
    cJSON_AddStringToObject(root, "weekdays", weekdays.c_str());
    cJSON_AddNumberToObject(root, "enabled", enabled ? 1 : 0);
    cJSON_AddNumberToObject(root, "isOneTime", is_one_time ? 1 : 0);
    
    char* json_str = cJSON_Print(root);
    std::string body(json_str);
    cJSON_Delete(root);
    free(json_str);

    return SendRequest("alarms/insert", "POST", body);
}

std::string AlarmClient::UpdateAlarm(const std::string& id, const std::string& time, 
                                 const std::string& message, const std::string& weekdays, bool enabled) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "time", time.c_str());
    cJSON_AddStringToObject(root, "message", message.c_str());
    cJSON_AddStringToObject(root, "weekdays", weekdays.c_str());
    cJSON_AddBoolToObject(root, "enabled", enabled);
    
    char* json_str = cJSON_Print(root);
    std::string body(json_str);
    cJSON_Delete(root);
    free(json_str);
    
    std::string path = "/api/alarms/" + id;
    return SendRequest(path, "PUT", body);
}

std::string AlarmClient::DeleteAlarm(const std::string& time_str) {
    std::string path = "alarms/" + GloableVar::device_id + "/" + time_str;
    return SendRequest(path, "DELETE");
}

std::string AlarmClient::DeleteAlarms() {
    std::string path = "alarms/" + GloableVar::device_id;
    return SendRequest(path, "DELETE");
}

std::string AlarmClient::UpdateAlarmStatus(const std::string& id, bool enabled) {
    std::string path = "alarms/" + id + "/status";
    return SendRequest(path, "POST");
}

bool AlarmClient::UpdateDeviceStatus(bool online, const std::string& firmware_version) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "online", online);
    cJSON_AddStringToObject(root, "firmware_version", firmware_version.c_str());
    
    char* json_str = cJSON_Print(root);
    std::string body(json_str);
    cJSON_Delete(root);
    free(json_str);
    
    std::string path = "/api/devices/" + device_id_ + "/status";
    std::string response = SendRequest(path, "PUT", body);
    
    cJSON* response_json = cJSON_Parse(response.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "Failed to parse response");
        return false;
    }
    
    cJSON* success = cJSON_GetObjectItem(response_json, "success");
    bool result = success && cJSON_IsBool(success) && success->valueint;
    
    cJSON_Delete(response_json);
    return result;
}
