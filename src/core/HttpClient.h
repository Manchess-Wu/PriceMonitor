#pragma once

#include <string>
#include <vector>

namespace pm {

struct HttpResult {
    long status = 0;
    std::string body;
    std::string effectiveUrl;
    std::string error;

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// libcurl 封装。每个请求独立 easy handle，可在多线程中安全并发调用。
class HttpClient {
public:
    static HttpResult get(const std::string& url,
                          const std::vector<std::string>& headers = {},
                          const std::string& cookie = "",
                          long timeoutSeconds = 20);

    static HttpResult postJson(const std::string& url, const std::string& jsonBody,
                               const std::vector<std::string>& headers = {},
                               long timeoutSeconds = 20);

    static HttpResult postForm(const std::string& url, const std::string& formBody,
                               const std::vector<std::string>& headers = {},
                               long timeoutSeconds = 20);
};

}  // namespace pm
