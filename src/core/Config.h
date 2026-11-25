#pragma once

#include <string>

namespace pm {

struct JdConfig {
    std::string cookie;
};

struct TaobaoConfig {
    std::string cookie;
    // 留空则使用 crawler.user_agent
    std::string userAgent;
};

// 淘宝的页面需要执行 JS 才能通过风控校验，因此通过本机浏览器加载。
struct BrowserConfig {
    bool enabled = true;
    bool headless = true;
    std::string chromePath = "google-chrome";
    std::string profileDir = "./data/chrome-profile";
    std::string userAgent =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36";
    int timeoutSeconds = 45;
};

struct CrawlerConfig {
    int intervalMinutes = 60;
    int timeoutSeconds = 20;
    int concurrency = 2;
    std::string userAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
    JdConfig jd;
    TaobaoConfig taobao;
    BrowserConfig browser;
};

struct NotifierConfig {
    bool enabled = false;
    // serverchan | wecom
    std::string type = "serverchan";
    std::string sendKey;
    std::string wecomWebhook;
    double dropPercent = 5.0;
};

struct Config {
    std::string listenAddr = "0.0.0.0";
    int port = 8080;
    int threads = 4;
    std::string documentRoot = "./web";
    std::string dbPath = "./data/pricemonitor.db";

    CrawlerConfig crawler;
    NotifierConfig notifier;

    // 配置文件不存在时会自动生成默认配置；JSON 非法则返回 false 并填充 err。
    static bool load(const std::string& path, Config& out, std::string& err);
};

}  // namespace pm
