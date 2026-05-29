#include "voice_call_client.h"
#include "board.h"
#include "gloable_var.h"
#include "constants.h"
#include <sstream>
#include <iomanip>
#include <cctype>
#include <string>
#include "esp_random.h"

#define TAG "VoiceCallClient"
#define MAX_RETRIES 3
#define RETRY_DELAY_MS 1000

// URL编码函数
static std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    
    for (char c : value) {
        // 保持字母数字字符不变
        if (std::isalnum(static_cast<unsigned char>(c)) || 
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        }
        // 编码其他所有字符
        else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) 
                    << static_cast<int>(static_cast<unsigned char>(c));
            escaped << std::nouppercase;
        }
    }
    
    return escaped.str();
}

VoiceCallClient& VoiceCallClient::GetInstance() {
    static VoiceCallClient instance;
    return instance;
}

VoiceCallClient::VoiceCallClient() : initialized_(false) {
}

VoiceCallClient::~VoiceCallClient() {
}

bool VoiceCallClient::Initialize(const std::string& api_url, const std::string& device_id) {
    if (api_url.empty() || device_id.empty()) {
        ESP_LOGE(TAG, "API URL or device ID is empty");
        return false;
    }
    
    api_url_ = api_url;
    device_id_ = device_id;
    initialized_ = true;
    
    ESP_LOGI(TAG, "VoiceCall client initialized with API URL: %s, device ID: %s", 
            api_url_.c_str(), device_id_.c_str());
    
    return true;
}

VoiceCallClient::MediaServerResult VoiceCallClient::getMediaServer() {
    MediaServerResult result = {false};

    if (!initialized_) {
        ESP_LOGE(TAG, "VoiceCall client not initialized");
        result.message = "VoiceCall client not initialized";
        return result;
    }

    // 发送请求
    std::string response = SendRequest("voice/mediaServer", "GET");

    ESP_LOGI(TAG, "Creating call request: response=%s",  response.c_str());
    
    // 解析响应
    cJSON* response_json = cJSON_Parse(response.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "Failed to parse response JSON");
        result.message = "Failed to parse response JSON";
        return result;
    }

    // 检查状态码
    cJSON* status_item = cJSON_GetObjectItem(response_json, "status");
    if (status_item && cJSON_IsString(status_item)) {
        result.status = std::string(status_item->valuestring);
        
        if (result.status == "success") {
            result.success = true;
            
            // 解析成功响应
            cJSON* server_ip_item = cJSON_GetObjectItem(response_json, "serverIp");
            if (server_ip_item && cJSON_IsString(server_ip_item)) {
                result.server_ip = std::string(server_ip_item->valuestring);
            }

            cJSON* server_port_item = cJSON_GetObjectItem(response_json, "serverPort");
            if (server_port_item && cJSON_IsString(server_port_item)) {
                result.server_port = std::stoi(std::string(server_port_item->valuestring));
            }

            ESP_LOGI(TAG, "Media server created successfully: server_ip=%s, server_port=%d", 
                    result.server_ip.c_str(), result.server_port);
            
        }
        else {
            // 其他错误状态
            result.success = false;
            
            cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
            if (error_item && cJSON_IsString(error_item)) {
                result.message = std::string(error_item->valuestring);
            } else {
                result.message = "Unknown error occurred";
            }
            
            ESP_LOGE(TAG, "Call request failed: %s", result.message.c_str());
        }
    } else {
        // 没有status字段，检查error字段
        cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
        if (error_item && cJSON_IsString(error_item)) {
            result.message = std::string(error_item->valuestring);
        } else {
            result.message = "Invalid response format";
        }
        
        ESP_LOGE(TAG, "Media server failed: %s", result.message.c_str());
    }
    
    cJSON_Delete(response_json);
    return result;
}

VoiceCallClient::RelationResult VoiceCallClient::getRelation(const std::string& device_id, const std::string& parent_openid) {
    RelationResult result = {false};

    if (!initialized_) {
        ESP_LOGE(TAG, "VoiceCall client not initialized");
        result.message = "VoiceCall client not initialized";
        return result;
    }

    // 发送请求
    std::string response = SendRequest(std::format("voice/relation?deviceId={}&parentOpenid={}", device_id, parent_openid), "GET");

    ESP_LOGI(TAG, "relation request: response=%s",  response.c_str());

    // 解析响应
    cJSON* response_json = cJSON_Parse(response.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "Failed to parse response JSON");
        result.message = "Failed to parse response JSON";
        return result;
    }

    // 检查状态码
    cJSON* status_item = cJSON_GetObjectItem(response_json, "status");
    if (status_item && cJSON_IsString(status_item)) {
        result.status = std::string(status_item->valuestring);
        
        if (result.status == "success") {
            result.success = true;
            
            // 解析成功响应
            cJSON* relation_item = cJSON_GetObjectItem(response_json, "relation");
            if (relation_item && cJSON_IsString(relation_item)) {
                result.relation = std::string(relation_item->valuestring);
            }

            ESP_LOGI(TAG, "relation successfully: relation=%s", result.relation.c_str());
        }
        else {
            // 其他错误状态
            result.success = false;
            
            cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
            if (error_item && cJSON_IsString(error_item)) {
                result.message = std::string(error_item->valuestring);
            } else {
                result.message = "Unknown error occurred";
            }
            
            ESP_LOGE(TAG, "relation request failed: %s", result.message.c_str());
        }
    } else {
        // 没有status字段，检查error字段
        cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
        if (error_item && cJSON_IsString(error_item)) {
            result.message = std::string(error_item->valuestring);
        } else {
            result.message = "Invalid response format";
        }
        
        ESP_LOGE(TAG, "relation failed: %s", result.message.c_str());
    }
    
    cJSON_Delete(response_json);
    return result;
}

VoiceCallClient::ChatResult VoiceCallClient::getChatId(const std::string& device_id, const std::string& member) {
    ChatResult result = {false};

    if (!initialized_) {
        ESP_LOGE(TAG, "VoiceCall client not initialized");
        result.message = "VoiceCall client not initialized";
        return result;
    }

    // 发送请求
    std::string response = SendRequest(std::format("voice/chatId?deviceId={}&member={}", device_id, member), "GET");

    ESP_LOGI(TAG, "chat id request: response=%s",  response.c_str());

    // 解析响应
    cJSON* response_json = cJSON_Parse(response.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "Failed to parse response JSON");
        result.message = "Failed to parse response JSON";
        return result;
    }

    // 检查状态码
    cJSON* status_item = cJSON_GetObjectItem(response_json, "status");
    if (status_item && cJSON_IsString(status_item)) {
        result.status = std::string(status_item->valuestring);
        
        if (result.status == "success") {
            result.success = true;
            
            // 解析成功响应
            cJSON* chat_id_item = cJSON_GetObjectItem(response_json, "charId");
            if (chat_id_item && cJSON_IsString(chat_id_item)) {
                result.chat_id = std::string(chat_id_item->valuestring);
            }

            ESP_LOGI(TAG, "chat id successfully: chat_id=%s", result.chat_id.c_str());
        }
        else {
            // 其他错误状态
            result.success = false;
            
            cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
            if (error_item && cJSON_IsString(error_item)) {
                result.message = std::string(error_item->valuestring);
            } else {
                result.message = "Unknown error occurred";
            }
            
            ESP_LOGE(TAG, "chat id request failed: %s", result.message.c_str());
        }
    } else {
        // 没有status字段，检查error字段
        cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
        if (error_item && cJSON_IsString(error_item)) {
            result.message = std::string(error_item->valuestring);
        } else {
            result.message = "Invalid response format";
        }
        
        ESP_LOGE(TAG, "chat id failed: %s", result.message.c_str());
    }
    
    cJSON_Delete(response_json);
    return result;
}

VoiceCallClient::CallRequestResult VoiceCallClient::CreateCallRequest(
    const std::string& call_id, 
    const std::string& parent_openid,
    const std::string& device_ip) {
    
    CallRequestResult result = {false};
    
    if (!initialized_) {
        ESP_LOGE(TAG, "VoiceCall client not initialized");
        result.message = "VoiceCall client not initialized";
        return result;
    }
    
    if (call_id.empty() || parent_openid.empty()) {
        ESP_LOGE(TAG, "Call ID or parent openid is empty");
        result.message = "Call ID or parent openid is empty";
        return result;
    }
    
    // 构建请求体
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "call_id", call_id.c_str());
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "parent_openid", parent_openid.c_str());
    cJSON_AddStringToObject(root, "device_ip", device_ip.c_str());
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string body(json_str);
    cJSON_Delete(root);
    free(json_str);
    
    ESP_LOGI(TAG, "Creating call request: call_id=%s, parent_openid=%s", 
            call_id.c_str(), parent_openid.c_str());
    
    // 发送请求
    std::string response = SendRequest("/api/call/request", "POST", body);
    ESP_LOGI(TAG, "Creating call request: response=%s", 
            response.c_str());
    // 解析响应
    cJSON* response_json = cJSON_Parse(response.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "Failed to parse response JSON");
        result.message = "Failed to parse response JSON";
        return result;
    }
    
    // 检查状态码
    cJSON* status_item = cJSON_GetObjectItem(response_json, "status");
    if (status_item && cJSON_IsString(status_item)) {
        result.status = std::string(status_item->valuestring);
        
        if (result.status == "success") {
            result.success = true;
            
            // 解析成功响应
            cJSON* call_id_item = cJSON_GetObjectItem(response_json, "call_id");
            if (call_id_item && cJSON_IsString(call_id_item)) {
                result.call_id = std::string(call_id_item->valuestring);
            }
            
            cJSON* device_websocket_port_item = cJSON_GetObjectItem(response_json, "device_websocket_port");
            if (device_websocket_port_item && cJSON_IsNumber(device_websocket_port_item)) {
                result.device_websocket_port = device_websocket_port_item->valueint;
            }

            cJSON* websocket_port_item = cJSON_GetObjectItem(response_json, "websocket_port");
            if (websocket_port_item && cJSON_IsNumber(websocket_port_item)) {
                result.websocket_port = websocket_port_item->valueint;
            }

            cJSON* websocket_url_item = cJSON_GetObjectItem(response_json, "websocket_url");
            if (websocket_url_item && cJSON_IsString(websocket_url_item)) {
                result.websocket_url = std::string(websocket_url_item->valuestring);
            }

            cJSON* timestamp_item = cJSON_GetObjectItem(response_json, "timestamp");
            if (timestamp_item && cJSON_IsString(timestamp_item)) {
                result.timestamp = std::string(timestamp_item->valuestring);
            }
            
            ESP_LOGI(TAG, "Call request created successfully: call_id=%s, websocket_port=%d, device_websocket_port=%d", 
                    result.call_id.c_str(), result.websocket_port, result.device_websocket_port);
            
        }
        else {
            // 其他错误状态
            result.success = false;
            
            cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
            if (error_item && cJSON_IsString(error_item)) {
                result.message = std::string(error_item->valuestring);
            } else {
                result.message = "Unknown error occurred";
            }
            
            ESP_LOGE(TAG, "Call request failed: %s", result.message.c_str());
        }
    } else {
        // 没有status字段，检查error字段
        cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
        if (error_item && cJSON_IsString(error_item)) {
            result.message = std::string(error_item->valuestring);
        } else {
            result.message = "Invalid response format";
        }
        
        ESP_LOGE(TAG, "Call request failed: %s", result.message.c_str());
    }
    
    cJSON_Delete(response_json);
    return result;
}

void VoiceCallClient::endCallRequest(const std::string& call_id) {
    if (!initialized_) {
        ESP_LOGE(TAG, "VoiceCall client not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Ending call request: call_id=%s", call_id.c_str());
    
    // 发送请求
    std::string path = "/api/call/end/" + call_id;
    std::string response = SendRequest(path, "DELETE");
    ESP_LOGI(TAG, "Ending call request: response=%s", response.c_str());
}

VoiceCallClient::CallRequest VoiceCallClient::getCallRequest(const std::string &device_id, const std::string &parent_openid) {
    CallRequest result = {true};
  
    std::string path = "call/request?deviceId=" + device_id + "&parentOpenId=" + parent_openid;
    std::string response = SendRequest(path, "GET");

    ESP_LOGI(TAG, "call request: response=%s", response.c_str());

    cJSON *response_json = cJSON_Parse(response.c_str());

    cJSON* roomSn_item = cJSON_GetObjectItem(response_json, "roomSn");
    if (roomSn_item && cJSON_IsString(roomSn_item)) {
        result.roomSn = std::string(roomSn_item->valuestring);
    }

    cJSON* type_item = cJSON_GetObjectItem(response_json, "type");
    if (type_item && cJSON_IsString(type_item)) {
        result.type = std::string(type_item->valuestring);
    }

    cJSON* roomIp_item = cJSON_GetObjectItem(response_json, "roomIp");
    if (roomIp_item && cJSON_IsString(roomIp_item)) {
        result.roomIp = std::string(roomIp_item->valuestring);
    }

    cJSON* roomPort_item = cJSON_GetObjectItem(response_json, "roomPort");
    if (roomPort_item && cJSON_IsString(roomPort_item)) {
        result.roomPort = std::stoi(std::string(roomPort_item->valuestring));
    }

    cJSON* roomToken_item = cJSON_GetObjectItem(response_json, "roomToken");
    if (roomToken_item && cJSON_IsString(roomToken_item)) {
        result.roomToken = std::string(roomToken_item->valuestring);
    }
    return result;
}


bool VoiceCallClient::getCallStatus(const std::string &device_id, const std::string &room_sn) {

    std::string path = "call/status?deviceId=" + device_id + "&roomSn=" + room_sn;
  
    std::string response = SendRequest(path, "GET");

    cJSON *response_json = cJSON_Parse(response.c_str());

    std::string status = "0";
    cJSON* status_item = cJSON_GetObjectItem(response_json, "status");
    if (status_item && cJSON_IsString(status_item)) {
        status = std::string(status_item->valuestring);
    }

    return (status == "1") ? true : false;
}

bool VoiceCallClient::setResponse(const std::string &device_id, const std::string &room_sn, const std::string &response) {
    std::string path = "call/response?deviceId=" + device_id + "&roomSn=" + room_sn + "&response=" + response;

    SendRequest(path, "GET");

    return true;
}

VoiceCallClient::CallStatusResult VoiceCallClient::GetCallStatus(const std::string& call_id) {
    CallStatusResult result = {false};
    
    if (!initialized_) {
        ESP_LOGE(TAG, "VoiceCall client not initialized");
        result.message = "VoiceCall client not initialized";
        return result;
    }
    
    if (call_id.empty()) {
        ESP_LOGE(TAG, "Call ID is empty");
        result.message = "Call ID is empty";
        return result;
    }
    
    std::string path = "/api/calls/" + call_id + "/status";
    std::string response = SendRequest(path, "GET");
    
    // 解析响应
    cJSON* response_json = cJSON_Parse(response.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "Failed to parse response JSON");
        result.message = "Failed to parse response JSON";
        return result;
    }
    
    // 检查错误
    cJSON* error_item = cJSON_GetObjectItem(response_json, "error");
    if (error_item && cJSON_IsString(error_item)) {
        result.message = std::string(error_item->valuestring);
        ESP_LOGE(TAG, "Get call status failed: %s", result.message.c_str());
        cJSON_Delete(response_json);
        return result;
    }
    
    // 解析成功响应
    result.success = true;
    
    cJSON* call_id_item = cJSON_GetObjectItem(response_json, "call_id");
    if (call_id_item && cJSON_IsString(call_id_item)) {
        result.call_id = std::string(call_id_item->valuestring);
    }
    
    cJSON* device_id_item = cJSON_GetObjectItem(response_json, "device_id");
    if (device_id_item && cJSON_IsString(device_id_item)) {
        result.device_id = std::string(device_id_item->valuestring);
    }
    
    cJSON* parent_openid_item = cJSON_GetObjectItem(response_json, "parent_openid");
    if (parent_openid_item && cJSON_IsString(parent_openid_item)) {
        result.parent_openid = std::string(parent_openid_item->valuestring);
    }
    
    cJSON* status_item = cJSON_GetObjectItem(response_json, "status");
    if (status_item && cJSON_IsString(status_item)) {
        result.status = std::string(status_item->valuestring);
    }
    
    cJSON* device_port_item = cJSON_GetObjectItem(response_json, "device_port");
    if (device_port_item && cJSON_IsNumber(device_port_item)) {
        result.device_port = device_port_item->valueint;
    }
    
    cJSON* start_time_item = cJSON_GetObjectItem(response_json, "start_time");
    if (start_time_item && cJSON_IsString(start_time_item)) {
        result.start_time = std::string(start_time_item->valuestring);
    }
    
    cJSON* duration_seconds_item = cJSON_GetObjectItem(response_json, "duration_seconds");
    if (duration_seconds_item && cJSON_IsNumber(duration_seconds_item)) {
        result.duration_seconds = duration_seconds_item->valueint;
    }
    
    cJSON* websocket_connected_item = cJSON_GetObjectItem(response_json, "websocket_connected");
    if (websocket_connected_item && cJSON_IsBool(websocket_connected_item)) {
        result.websocket_connected = cJSON_IsTrue(websocket_connected_item);
    }
    
    cJSON* server_info_item = cJSON_GetObjectItem(response_json, "server_info");
    if (server_info_item && cJSON_IsObject(server_info_item)) {
        cJSON* ip_item = cJSON_GetObjectItem(server_info_item, "ip");
        if (ip_item && cJSON_IsString(ip_item)) {
            result.server_ip = std::string(ip_item->valuestring);
        }
    }
    
    cJSON* timestamp_item = cJSON_GetObjectItem(response_json, "timestamp");
    if (timestamp_item && cJSON_IsString(timestamp_item)) {
        result.timestamp = std::string(timestamp_item->valuestring);
    }
    
    ESP_LOGI(TAG, "Call status retrieved: call_id=%s, status=%s", 
            result.call_id.c_str(), result.status.c_str());
    
    cJSON_Delete(response_json);
    return result;
}

std::string VoiceCallClient::SendRequest(const std::string& path, 
                                       const std::string& method, 
                                       const std::string& body) {
    if (!initialized_) {
        ESP_LOGE(TAG, "VoiceCall client not initialized");
        return "{\"error\": \"VoiceCall client not initialized\"}";
    }
    
    int connection_id = esp_random() % 10000;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(connection_id);

    std::shared_ptr<Http> shared_http = std::move(http);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    http_client_->SetKeepAlive(true); // 启用 Keep-Alive
    http_client_->SetTimeout(60 * 1000);
    
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
        return "{\"error\": \"Failed to open connection\"}";
    }
        
    // 读取响应
    std::string response = http_client_->ReadAll();
    int status_code = http_client_->GetStatusCode();
        
    http_client_->Close();
        
    ESP_LOGI(TAG, "Response status code: %d", status_code);
    ESP_LOGD(TAG, "Response body: %s", response.c_str());
        
    if (status_code >= 200 && status_code < 300) {
        return response;
    }
    
    // 根据状态码返回相应的错误信息
    if (status_code == 400) {
        return "{\"error\": \"Bad request\"}";
    } else if (status_code == 404) {
        return "{\"error\": \"Call not found\"}";
    } else if (status_code == 503) {
        return "{\"error\": \"Service unavailable\"}";
    } else {
        return "{\"error\": \"HTTP error: " + std::to_string(status_code) + "\"}";
    }
}