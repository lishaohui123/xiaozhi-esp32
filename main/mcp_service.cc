#include "mcp_service.h"
#include "board.h"
#include "gloable_var.h"
#include "constants.h"
#include <sstream>
#include <iomanip>
#include <cctype>
#include <string>
#include "esp_random.h"
#include "esp_log.h"

#define TAG "McpService"

static std::string urlEncode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
            continue;
        }
        
        escaped << std::uppercase;
        escaped << '%' << std::setw(2) << int((unsigned char)c);
        escaped << std::nouppercase;
    }

    return escaped.str();
}

McpService& McpService::GetInstance() {
    static McpService instance;
    return instance;
}

void McpService::healthReportIssue(const std::string& issue) {
    std::string response = SendRequest(std::format("health/reportIssue?deviceId={}&issue={}", GloableVar::device_id, urlEncode(issue)), "GET");
}

void McpService::shareSecret(const std::string &secret,
                             const std::string &emotion,
                             const std::string &topic) {
  std::string response = SendRequest(std::format("health/shareSecret?deviceId={}&secret={}&emotion={}&topic={}", GloableVar::device_id, urlEncode(secret), urlEncode(emotion), urlEncode(topic)), "GET");
}

std::string McpService::SendRequest(const std::string& path, 
                                       const std::string& method, 
                                       const std::string& body) {    
    int connection_id = esp_random() % 10000;
    auto http_unique = Board::GetInstance().GetNetwork()->CreateHttp(connection_id);
    std::shared_ptr<Http> shared_http = std::move(http_unique);
    http_client_ = std::static_pointer_cast<HttpClient>(shared_http);
    if (!http_client_) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return "{\"error\": \"Failed to create HTTP client\"}";
    }
    
    // 设置请求头
    http_client_->SetHeader("Content-Type", "application/json");
    http_client_->SetHeader("Device-ID", GloableVar::device_id);
    http_client_->SetHeader("token", ACCESS_TOKEN);
        
    // 构建完整URL
    std::string url = OTA_URI + path;
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