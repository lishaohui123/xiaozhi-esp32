#ifndef VOICE_CALL_H
#define VOICE_CALL_H

#include <atomic>
#include "esp_err.h"
#include "mqtt_client.h"
#include "lwip/sockets.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "esp_websocket_client.h"
#include "esp_tls.h"
extern "C" {
#include "opus.h"
}
#include "esp_wifi.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string>
#include "audio_codec.h"
#include "protocol.h"

// MQTT主题定义
#define MQTT_TOPIC_DEVICE_COMMAND "device/command"
#define MQTT_TOPIC_CALL_REQUEST "call/request"
#define MQTT_TOPIC_CALL_ACCEPT "call/accept"
#define MQTT_TOPIC_CALL_END "call/end"
#define MQTT_TOPIC_CALL_END_ACK "call/end/ack"
#define MQTT_TOPIC_CALL_RESPONSE "call/response"
#define MQTT_TOPIC_DEVICE_CALL_REQUEST "device/%s/call/request"
#define MQTT_TOPIC_DEVICE_CALL_ACCEPT "device/%s/call/accept"
#define MQTT_TOPIC_DEVICE_CALL_END "device/%s/call/end"
#define MQTT_TOPIC_DEVICE_CALL_RESPONSE "device/%s/call/response"
#define TOPIC_DEVICE_ALARM_TEMPLATE  "device/%s/alarm"
#define TOPIC_DEVICE_WORKS_TEMPLATE  "device/%s/works"
#define TOPIC_DEVICE_AUDIO_TEMPLATE  "device/%s/audio"
#define TOPIC_SOCIAL_HD_TEMPLATE  "social/%s/hd"
#define TOPIC_DEVICE_SET_TEMPLATE  "device/%s/set"

#define CALL_CONNECT_TIMEOUT_MS 60000  // 60秒连接超时
#define CALL_RESPONSE_TIMEOUT_MS 60000 // 60秒响应超时


#define WEBSOCKET_SEND_AUDIO (1 << 1)

#define AUDIO_SAMPLES_PER_PACKAGE  320

// 通话状态定义
typedef enum {
    CALL_STATE_IDLE,
    CALL_STATE_WAITING,
    CALL_STATE_CONNECTING,
    CALL_STATE_IN_CALL,
    CALL_STATE_ENDING
} call_state_t;

// 设备信息结构体
typedef struct {
    std::string device_id;
    std::string device_name;
    std::string ip_address;
    std::string verification_code;
    time_t verification_code_expiry; // 验证码过期时间
} device_info_t;

// 通话信息结构体
typedef struct {
    std::atomic<call_state_t> state{CALL_STATE_IDLE};
    std::string roomSn;
    std::string parent_openid;
    std::string server_ip;
    int server_port;
    std::string roomToken;
} call_info_t;

// WebSocket数据包结构
typedef struct {
    uint8_t* data;
    int length;
    bool is_binary;
} websocket_packet_t;


#define CUSTOM_HEADER_SIZE 52
#define CUSTOM_TOKEN_LEN   12

#pragma pack(push, 1) // 禁止内存对齐
typedef struct {
    uint8_t mac[6];                           // 设备mac地址
    uint8_t token[CUSTOM_TOKEN_LEN];          // token
    uint32_t magic;         	              // 固定0xABCD1234（网络字节序）
    uint16_t version;       	              // 协议版本（当前为0x0102）
    uint8_t  payload_type;  	              // 0=视频，1=音频，2=设备心跳，3=网关心跳
    uint8_t  codec_id;      	              // 0=H264，1=PCM
    uint16_t width;         	              // 视频宽度（仅视频有效）
    uint16_t height;        	              // 视频高度（仅视频有效）
    uint32_t sample_rate;   	              // 音频采样率（仅音频有效）
    uint8_t  channels;      	              // 音频声道数（仅音频有效）
    uint8_t  reserved;      	              // 通话状态（仅心跳有效）
    uint64_t timestamp;     	              // 时间戳（微秒，网络字节序）
    uint32_t payload_size;  	              // 负载数据长度（网络字节序）
    uint8_t total_frags;                      // 总分片数 (v1.2协议)
    uint8_t current_frag;                     // 当前分片序号 (v1.2协议)
    uint16_t header_size;                     // 头部长度（固定为CUSTOM_HEADER_SIZE）
} CustomHeader;
#pragma pack(pop)

#define MAX_PACKET_SIZE  1400  // 推荐 UDP 包总大小（含协议头）
#define MAX_DATA_PER_PACKET (MAX_PACKET_SIZE - CUSTOM_HEADER_SIZE)


typedef enum {
    CODEC_ID_H264 = 0,    // H.264 视频编解码器
    CODEC_ID_H265,        // H.265 视频编解码器
    CODEC_ID_H263,        // H.263 视频编解码器
    CODEC_ID_MPEG4,       // MPEG-4 视频编解码器
    CODEC_ID_JPEG,        // JPEG 视频编解码器
    CODEC_ID_VIDEO_MAX
} CodecId_Video;

typedef enum {
  CODEC_ID_OPUS = 0, // Opus 音频编解码器
  CODEC_ID_PCM,      // PCM 音频编解码器
  CODEC_ID_VORBIS,   // Vorbis 音频编解码器
  CODEC_ID_FLAC,     // FLAC 音频编解码器
  CODEC_ID_AMR_NB,   // AMR-NB 音频编解码器
  CODEC_ID_AMR_WB,   // AMR-WB 音频编解码器
  CODEC_ID_G711A,    // G.711 A-law 音频编解码器
  CODEC_ID_G711U,    // G.711 μ-law 音频编解码器
  CODEC_ID_G729,     // G.729 音频编解码器
  CODEC_ID_SPEEX,    // Speex 音频编解码器
  CODEC_ID_AUDIO_MAX
} CodecId_Audio;

typedef enum session_status {
  SESSION_IDLE = 0,             // 初始状态
  SESSION_CALLING = 1,          // 拨打电话中
  SESSION_TALKING = 2,          // 通话中 (被拨打的微信用户接听了电话)
  SESSION_REJECTED = 3,         // 被拨打的微信用户拒绝接听电话
  SESSION_CANCELED = 4,         // 拨打过程中, 设备取消了电话拨打
  SESSION_HANGUP_BY_CALLER = 5, // 通话时设备挂断了电话
  SESSION_HANGUP_BY_CALLEE = 6, // 通话时被拨打的微信用户挂断了电话
  SESSION_ABORTED = 7,          // 发生异常
  SESSION_BUSY = 8,             // 被拨打的微信用户处于占线状态
  SESSION_TIMEOUT = 9,          // 超时未接听
} session_status_t;

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 5000

#define CALL_STATUS_INTERVAL_MS 10000

// VoiceCall类
class VoiceCall {
public:
    // 单例模式
    static VoiceCall* get_instance();

    // 初始化MQTT
    esp_err_t initMqtt(const std::string& device_id, const std::string& device_name, const std::string& mqtt_broker_uri);

    // 初始化UDP
    void initUdp();

    // 开始音频处理
    void start_audio_processing();
    
    // 停止音频处理
    void stop_audio_processing();

    // 发起通话
    void make_call(std::string member);
    // 发起通话（异步，不阻塞调用线程）
    void make_call_async(const std::string& member);

    // MCP调用生成设备验证码
    std::string mcp_generate_verification_code();

    // 设置事件
    void SetWebsocketSendAudio();

private:
    // 私有构造函数
    VoiceCall();
    
    // 析构函数
    ~VoiceCall();

    // 处理MQTT消息
    void handle_mqtt_message(const char* topic, const char* payload, int payload_len);

    // 发送MQTT消息
    esp_err_t send_mqtt_message(const char *topic, const char *payload);

    // 处理来电
    void handle_incoming_call(const char* payload);
    
    // 处理通话结束
    void handle_call_end();

    void handle_device_alarm(const char* payload);

    void handle_device_works(const char* payload);

    void handle_device_audio(const char *payload);

    void handle_social_hd(const char* payload);

    void handle_device_set(const char* payload);
    
    // UDP任务
    void udp_receive_task();
    void udp_send_task();

    void call_end();

    void reset_call_state();

    // 重新生成验证码
    void regenerate_verification_code();

    // 妙月的天枢平台提供的UDP发送音频祯
    void send_audio_frame(int sockfd, uint32_t sample_rate, uint8_t  channels, uint64_t timestamp, const uint8_t* audio_data, size_t audio_size);

    // 妙月的天枢平台提供的UDP发送包
    void send_packet(int sockfd, const CustomHeader* header, const uint8_t* rtp_data, int data_len);

    bool parse_packet(const uint8_t* data, size_t len, CustomHeader* header, uint8_t** payload);

    // 字节序转换辅助函数
    static inline uint64_t htonll(uint64_t x) {
        return ((uint64_t)htonl((x & 0xFFFFFFFF)) << 32) | htonl((x >> 32));
    }
    static inline uint64_t ntohll(uint64_t x) {
        return ((uint64_t)ntohl((x & 0xFFFFFFFF)) << 32) | ntohl((x >> 32));
    }

public:
    std::atomic<bool> udp_send_task_exit{true};
    std::atomic<bool> udp_receive_task_exit{true};

    // 任务状态控制
    std::atomic<bool> m_udp_receive_task_running{false};
    std::atomic<bool> m_udp_send_task_running{false};

private:
    // 单例实例
    static VoiceCall* s_instance;

    // 系统提供的mqtt
    std::unique_ptr<Mqtt> mqtt_;

    // MQTT的定时器
    esp_timer_handle_t reconnect_timer_;

    // 通话状态的定时器
    esp_timer_handle_t call_status_timer_;
    
    // UDP句柄
    int client_sock_;
    struct sockaddr_in server_addr_;         // 目标服务器地址（用于发送）
    struct sockaddr_in from_addr_;           // 接收数据时的源地址
    socklen_t from_len_;                     // 地址结构体长度
    uint8_t recv_buf_[MAX_PACKET_SIZE];      // 接收缓冲区（大小由 MAX_PACKET_SIZE 决定）

    // ========== 线程同步 ==========
    std::mutex send_mutex_; // 保护 sendto 操作，避免多线程并发发送
    // std::mutex queue_mutex_;

    // 设备信息
    device_info_t m_device_info;
    
    // 通话信息
    call_info_t m_call_info;

    // 任务句柄
    TaskHandle_t m_udp_receive_task_handle;
    TaskHandle_t m_udp_send_task_handle;
    
    // 🔥 添加静态任务分配相关成员 - 接收任务
    static constexpr uint32_t UDP_RECEIVE_TASK_STACK_SIZE = 8192;
    StackType_t* m_udp_receive_task_stack_;
    StaticTask_t* m_udp_receive_task_tcb_;
    bool m_udp_receive_task_static_allocated_;
    
    // 🔥 添加静态任务分配相关成员 - 发送任务
    static constexpr uint32_t UDP_SEND_TASK_STACK_SIZE = 8192;
    StackType_t* m_udp_send_task_stack_;
    StaticTask_t* m_udp_send_task_tcb_;
    bool m_udp_send_task_static_allocated_;
    // 🔥 添加电话异步任务的静态分配相关成员
    static constexpr uint32_t PHONE_CALL_STACK_SIZE = 8192;
    StackType_t* m_phone_call_stack_;
    StaticTask_t* m_phone_call_tcb_;
    TaskHandle_t m_phone_call_task_handle_;

    // socket事件组
    EventGroupHandle_t event_group_ = nullptr;

    // 删除复杂的事件组标志位，改用简单状态
    bool websocket_reconnect_failed_;

    std::string phone_member_;
};

#endif // VOICE_CALL_H
