#ifndef MCP_SERVICE_H
#define MCP_SERVICE_H

#include <sstream>
#include <iomanip>
#include <cctype>
#include <string>
#include <memory>

class HttpClient;
class McpService {
public:
  std::shared_ptr<HttpClient> http_client_ = nullptr;
  static McpService &GetInstance();
  
  void healthReportIssue(const std::string& issue);

  void shareSecret(const std::string& secret, const std::string& emotion, const std::string& topic);
      
private:
    std::string SendRequest(const std::string& path, 
                          const std::string& method, 
                          const std::string& body = "");

};

#endif // MCP_SERVICE_H
