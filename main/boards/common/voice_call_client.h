#ifndef VOICE_CALL_CLIENT_H
#define VOICE_CALL_CLIENT_H

#include <string>
#include <memory>
#include "esp_log.h"
#include "cJSON.h"
#include "http_client.h"

class VoiceCallClient {
public:
    VoiceCallClient();
    ~VoiceCallClient();

    // 单例模式
    static VoiceCallClient& GetInstance();
    
    // 初始化
    bool Initialize(const std::string& api_url, const std::string& device_id);
    
    // 创建通话请求
    struct CallRequestResult {
        bool success;
        std::string status;
        std::string call_id;
        int websocket_port;
        std::string websocket_url;
        int device_websocket_port;
        std::string timestamp;
        std::string message; // 错误信息或重定向信息
        // 重定向信息
        struct {
            std::string node_id;
            std::string ip;
            int websocket_port;
            int api_port;
        } target_server;
    };
    
    struct MediaServerResult {
        bool success;
        std::string status;
        std::string message; // 错误信息或重定向信息
        std::string server_ip;
        int server_port;
    };

    struct ChatResult {
        bool success;
        std::string status;
        std::string message; // 错误信息或重定向信息
        std::string chat_id;
    };

    struct RelationResult {
        bool success;
        std::string status;
        std::string message; // 错误信息或重定向信息
        std::string relation;
    };

    VoiceCallClient::CallRequestResult CreateCallRequest(
    const std::string& call_id, 
    const std::string& parent_openid,
    const std::string& device_ip);
    
    VoiceCallClient::MediaServerResult getMediaServer() ;

    ChatResult getChatId(const std::string& device_id, const std::string& memberr);
    RelationResult getRelation(const std::string& device_id, const std::string& parent_openid);

    // 获取通话状态
    struct CallStatusResult {
        bool success;
        std::string call_id;
        std::string device_id;
        std::string parent_openid;
        std::string status;
        int device_port;
        std::string start_time;
        int duration_seconds;
        bool websocket_connected;
        std::string server_ip;
        std::string timestamp;
        std::string message;
    };
    
    CallStatusResult GetCallStatus(const std::string& call_id);
    
    // 结束通话
    struct EndCallResult {
        bool success;
        std::string call_id;
        std::string message;
        std::string end_time;
        int duration_seconds;
        std::string timestamp;
    };
    
    void endCallRequest(const std::string& call_id);


    struct CallRequest {
        bool success;
        std::string roomSn;
        std::string type;
        std::string roomIp;
        int roomPort;
        std::string roomToken;
    };
    CallRequest getCallRequest(const std::string& device_id, const std::string& parent_openid);

    bool getCallStatus(const std::string &device_id, const std::string &room_sn);

    bool setResponse(const std::string &device_id, const std::string &room_sn, const std::string &response);
    
private:

    
    // 禁用拷贝
    VoiceCallClient(const VoiceCallClient&) = delete;
    VoiceCallClient& operator=(const VoiceCallClient&) = delete;
    
    std::string SendRequest(const std::string& path, 
                          const std::string& method, 
                          const std::string& body = "");
    
    bool initialized_;
    std::string api_url_;
    std::string device_id_;
    static const char *TAG;

    std::shared_ptr<HttpClient> http_client_;   // 复用 HTTP 客户端
};

#endif // VOICE_CALL_CLIENT_H