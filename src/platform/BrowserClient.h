#pragma once

#include <optional>
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
    // extractExpr 非空时，在同一个已渲染页面上再执行一段 JS，
    // 结果写入 extractResult（用于读取券后价等文本）。
    bool fetchHtml(const std::string& url, const std::string& waitFor,
                   std::string& html, std::string& err,
                   const std::string& extractExpr = "",
                   std::string* extractResult = nullptr);

    // 页面内提取“券后价/到手价”的通用脚本，返回用 | 分隔的候选价格
    static const char* couponPriceScript();
    // 从 couponPriceScript 的返回串里取第一个有效价格
    static std::optional<double> firstCouponPrice(const std::string& candidates);
    // 取出脚本返回的诊断片段（未匹配到价格时用于排查页面文案）
    static std::string couponDiagnostics(const std::string& candidates);

    // 带界面启动浏览器供用户手动登录，阻塞直到浏览器窗口关闭。
    static bool openForLogin(const Options& options, const std::string& url,
                             std::string& err);

    static Options fromConfig(const CrawlerConfig& config);

private:
    Options options_;
};

}  // namespace pm
