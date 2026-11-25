#pragma once

#include <string>

#include "core/Config.h"

namespace pm {

// 通过本机 Chrome/Chromium（CDP）加载页面，用于绕过需要 JS 才能通过的风控校验。
class BrowserClient {
public:
    struct Options {
        bool enabled = true;
        bool headless = true;
        std::string chromePath = "google-chrome";
        std::string profileDir = "./data/chrome-profile";
        std::string userAgent;
        int timeoutSeconds = 45;
    };

    explicit BrowserClient(Options options) : options_(std::move(options)) {}

    // 无界面加载页面，等待 waitFor 出现在渲染结果中后返回 HTML。
    bool fetchHtml(const std::string& url, const std::string& waitFor,
                   std::string& html, std::string& err);

    // 带界面启动浏览器供用户手动登录，阻塞直到浏览器窗口关闭。
    static bool openForLogin(const Options& options, const std::string& url,
                             std::string& err);

    static Options fromConfig(const CrawlerConfig& config);

private:
    Options options_;
};

}  // namespace pm
