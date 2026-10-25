#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <errno.h>

static const char *TAG = "EspTcp";

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    // 确保先断开已有连接（包括可能仍在运行的接收任务）
    if (connected_ || receive_task_created_) {
        Disconnect();
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    // 设置接收超时，保证接收任务不会无限期阻塞在 recv()，
    // 从而在断开连接时能被可靠唤醒并退出。
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(tcp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    receive_task_created_ = true;
    if (xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_) != pdPASS) {
        receive_task_created_ = false;
        connected_ = false;
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }
    return true;
}

void EspTcp::Disconnect() {
    // 无论当前是否显示为已连接，只要创建过接收任务就必须等待其退出。
    DoDisconnect(true);
}

void EspTcp::DoDisconnect(bool wait_for_task) {
    bool was_connected = connected_;
    connected_ = false;

    if (tcp_fd_ != -1) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }

    // 等待接收任务真正退出。退出位由任务在访问完 this 之后、自删除之前设置，
    // 因此这里必须等到位，不能因超时继续释放对象。
    if (wait_for_task && receive_task_created_) {
        xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        receive_task_created_ = false;
    }

    // 只在从已连接状态真正断开时触发一次回调，避免被动断开后析构重复触发。
    if (was_connected && disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret == 0) {
            // 对端正常关闭连接
            DoDisconnect(false);
            break;
        }
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 接收超时，回到循环顶部检查 connected_ 是否已被要求断开
                continue;
            }
            ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspTcp::GetLastError() {
    return last_error_;
}
