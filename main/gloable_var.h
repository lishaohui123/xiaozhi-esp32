#ifndef GLOABLE_VAR_H
#define GLOABLE_VAR_H

#include <string>
#include "http_client.h"

class GloableVar {
public:
    static std::string work_url;
    static std::string app_id;
    static std::string app_secret;
    static std::string device_id;
    static std::string work_detail_url;
    static volatile int mode_realtime;
    static int volume;
    static std::string mqtt_user_name;
    static std::string mqtt_password;

    static std::string tts_api_url;
    static std::string tts_api_key;
    static std::string alarm_app_id;
    static std::string token;
    static std::string cluster;
    static std::string voice_type;

public:
    static std::string generate_uuid();
    static std::string generate_sha1_signature(const std::string& app_id, 
                                   const std::string& app_secret, 
                                   const std::string& timestamp) ;
    static void init_ntp_time();
    static int get_gloable_var();
    static int get_alarm_var();
    static int update_gloable_var(int volume);

private:
    static std::shared_ptr<HttpClient> http_client_;   // 复用 HTTP 客户端
};

#endif // GLOABLE_VAR_H
