#include "core/HttpClient.h"

#include <curl/curl.h>

#include <mutex>

#include "core/Util.h"

namespace pm {
namespace {

constexpr size_t kMaxBodySize = 8 * 1024 * 1024;

std::once_flag g_curlOnce;

void ensureCurlInitialized() {
    std::call_once(g_curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    if (body->size() + total > kMaxBodySize) {
        return 0;  // 中止传输，避免异常页面撑爆内存
    }
    body->append(ptr, total);
    return total;
}

bool hasHeader(const std::vector<std::string>& headers, const std::string& name) {
    std::string loweredName = toLower(name);
    for (const auto& h : headers) {
        size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        if (toLower(trim(h.substr(0, colon))) == loweredName) return true;
    }
    return false;
}

HttpResult perform(const std::string& url, bool isPost, const std::string& body,
                   const std::vector<std::string>& headers,
                   const std::string& cookie, long timeoutSeconds) {
    HttpResult result;
    if (url.empty()) {
        result.error = "请求地址为空";
        return result;
    }
    ensureCurlInitialized();

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "初始化 libcurl 失败";
        return result;
    }

    struct curl_slist* headerList = nullptr;
    std::vector<std::string> effectiveHeaders = headers;
    if (isPost && !hasHeader(effectiveHeaders, "Content-Type")) {
        effectiveHeaders.push_back("Content-Type: application/json; charset=utf-8");
    }
    if (!hasHeader(effectiveHeaders, "Accept-Language")) {
        effectiveHeaders.push_back("Accept-Language: zh-CN,zh;q=0.9");
    }
    for (const auto& h : effectiveHeaders) {
        headerList = curl_slist_append(headerList, h.c_str());
    }

    if (timeoutSeconds <= 0) timeoutSeconds = 20;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 6L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // 自动协商并解压 gzip
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    if (!hasHeader(effectiveHeaders, "User-Agent")) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "PriceMonitor/1.0");
    }
    if (!cookie.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookie.c_str());
    }
    if (isPost) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
        char* effective = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
        if (effective) result.effectiveUrl = effective;
    } else if (rc == CURLE_WRITE_ERROR && result.body.size() >= kMaxBodySize) {
        result.error = "响应内容超过 8MB，已中止";
    } else {
        result.error = std::string("网络请求失败: ") + curl_easy_strerror(rc);
    }

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return result;
}

}  // namespace

HttpResult HttpClient::get(const std::string& url,
                           const std::vector<std::string>& headers,
                           const std::string& cookie, long timeoutSeconds) {
    return perform(url, false, "", headers, cookie, timeoutSeconds);
}

HttpResult HttpClient::postJson(const std::string& url, const std::string& jsonBody,
                                const std::vector<std::string>& headers,
                                long timeoutSeconds) {
    return perform(url, true, jsonBody, headers, "", timeoutSeconds);
}

HttpResult HttpClient::postForm(const std::string& url, const std::string& formBody,
                                const std::vector<std::string>& headers,
                                long timeoutSeconds) {
    std::vector<std::string> merged = headers;
    merged.push_back("Content-Type: application/x-www-form-urlencoded; charset=utf-8");
    return perform(url, true, formBody, merged, "", timeoutSeconds);
}

}  // namespace pm
