#include "core/Config.h"

#include <filesystem>
#include <fstream>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "core/Logger.h"
#include "core/Util.h"

namespace pm {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

template <typename T>
T valueOr(const json& obj, const char* key, T fallback) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj.at(key);
    try {
        if constexpr (std::is_same_v<T, std::string>) {
            if (v.is_string()) return v.get<std::string>();
        } else if constexpr (std::is_same_v<T, bool>) {
            if (v.is_boolean()) return v.get<bool>();
        } else if constexpr (std::is_same_v<T, int>) {
            if (v.is_number_integer()) return v.get<int>();
        } else if constexpr (std::is_same_v<T, double>) {
            if (v.is_number()) return v.get<double>();
        }
    } catch (const json::exception&) {
        // 类型不匹配时使用默认值
    }
    return fallback;
}

json objectOr(const json& root, const char* key) {
    if (!root.is_object() || !root.contains(key)) return json::object();
    const auto& value = root.at(key);
    return value.is_object() ? value : json::object();
}

json toJson(const Config& c) {
    return json{
        {"server",
         {{"listen", c.listenAddr},
          {"port", c.port},
          {"threads", c.threads},
          {"document_root", c.documentRoot}}},
        {"database", {{"path", c.dbPath}}},
        {"crawler",
         {{"interval_minutes", c.crawler.intervalMinutes},
          {"timeout_seconds", c.crawler.timeoutSeconds},
          {"concurrency", c.crawler.concurrency},
          {"user_agent", c.crawler.userAgent},
          {"jd", {{"cookie", c.crawler.jd.cookie}}},
          {"taobao",
           {{"cookie", c.crawler.taobao.cookie},
            {"user_agent", c.crawler.taobao.userAgent}}},
          {"browser",
           {{"enabled", c.crawler.browser.enabled},
            {"headless", c.crawler.browser.headless},
            {"chrome_path", c.crawler.browser.chromePath},
            {"profile_dir", c.crawler.browser.profileDir},
            {"user_agent", c.crawler.browser.userAgent},
            {"timeout_seconds", c.crawler.browser.timeoutSeconds}}}}},
        {"notifier",
         {{"enabled", c.notifier.enabled},
          {"type", c.notifier.type},
          {"send_key", c.notifier.sendKey},
          {"wecom_webhook", c.notifier.wecomWebhook},
          {"drop_percent", c.notifier.dropPercent}}}};
}

void writeDefault(const std::string& path, const Config& c, std::string& err) {
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            err = "无法创建配置目录: " + p.parent_path().string() + " (" + ec.message() + ")";
            return;
        }
    }
    std::ofstream out(path);
    if (!out) {
        err = "无法写入默认配置文件: " + path;
        return;
    }
    out << toJson(c).dump(2) << std::endl;
    out.flush();
    if (!out) {
        err = "写入默认配置文件失败: " + path;
        return;
    }
    logInfo("已生成默认配置文件: " + path);
}

}  // namespace

bool Config::load(const std::string& path, Config& out, std::string& err) {
    out = Config{};

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        writeDefault(path, out, err);
        if (!err.empty()) return false;
        return true;
    }

    std::ifstream in(path);
    if (!in) {
        err = "无法打开配置文件: " + path;
        return false;
    }

    json j;
    try {
        in >> j;
    } catch (const json::exception& e) {
        err = "配置文件 JSON 解析失败: " + std::string(e.what());
        return false;
    }
    if (!j.is_object()) {
        err = "配置文件根节点必须是 JSON 对象";
        return false;
    }

    const auto server = objectOr(j, "server");
    const auto database = objectOr(j, "database");
    const auto crawler = objectOr(j, "crawler");
    const auto jd = objectOr(crawler, "jd");
    const auto taobao = objectOr(crawler, "taobao");
    const auto browser = objectOr(crawler, "browser");
    const auto notifier = objectOr(j, "notifier");

    out.listenAddr = valueOr<std::string>(server, "listen", out.listenAddr);
    out.port = valueOr<int>(server, "port", out.port);
    out.threads = valueOr<int>(server, "threads", out.threads);
    out.documentRoot = valueOr<std::string>(server, "document_root", out.documentRoot);

    out.dbPath = valueOr<std::string>(database, "path", out.dbPath);

    out.crawler.intervalMinutes =
        valueOr<int>(crawler, "interval_minutes", out.crawler.intervalMinutes);
    out.crawler.timeoutSeconds =
        valueOr<int>(crawler, "timeout_seconds", out.crawler.timeoutSeconds);
    out.crawler.concurrency =
        valueOr<int>(crawler, "concurrency", out.crawler.concurrency);
    out.crawler.userAgent =
        valueOr<std::string>(crawler, "user_agent", out.crawler.userAgent);
    out.crawler.jd.cookie = valueOr<std::string>(jd, "cookie", "");
    out.crawler.taobao.cookie = valueOr<std::string>(taobao, "cookie", "");
    out.crawler.taobao.userAgent = valueOr<std::string>(taobao, "user_agent", "");
    out.crawler.browser.enabled =
        valueOr<bool>(browser, "enabled", out.crawler.browser.enabled);
    out.crawler.browser.headless =
        valueOr<bool>(browser, "headless", out.crawler.browser.headless);
    out.crawler.browser.chromePath =
        valueOr<std::string>(browser, "chrome_path", out.crawler.browser.chromePath);
    out.crawler.browser.profileDir =
        valueOr<std::string>(browser, "profile_dir", out.crawler.browser.profileDir);
    out.crawler.browser.userAgent =
        valueOr<std::string>(browser, "user_agent", out.crawler.browser.userAgent);
    out.crawler.browser.timeoutSeconds =
        valueOr<int>(browser, "timeout_seconds", out.crawler.browser.timeoutSeconds);

    out.notifier.enabled =
        valueOr<bool>(notifier, "enabled", out.notifier.enabled);
    out.notifier.type =
        valueOr<std::string>(notifier, "type", out.notifier.type);
    out.notifier.sendKey =
        valueOr<std::string>(notifier, "send_key", out.notifier.sendKey);
    out.notifier.wecomWebhook =
        valueOr<std::string>(notifier, "wecom_webhook", out.notifier.wecomWebhook);
    out.notifier.dropPercent =
        valueOr<double>(notifier, "drop_percent", out.notifier.dropPercent);

    // 基础纠偏，避免明显的配置错误导致服务不可用
    if (out.port <= 0 || out.port > 65535) out.port = 8080;
    if (out.threads <= 0) out.threads = 4;
    if (out.crawler.intervalMinutes <= 0) out.crawler.intervalMinutes = 60;
    if (out.crawler.timeoutSeconds <= 0) out.crawler.timeoutSeconds = 20;
    if (out.crawler.concurrency <= 0) out.crawler.concurrency = 1;
    if (out.crawler.concurrency > 8) out.crawler.concurrency = 8;
    if (out.crawler.browser.timeoutSeconds <= 0) out.crawler.browser.timeoutSeconds = 45;
    if (out.crawler.browser.chromePath.empty()) {
        out.crawler.browser.chromePath = "google-chrome";
    }
    if (out.crawler.browser.profileDir.empty()) {
        out.crawler.browser.profileDir = "./data/chrome-profile";
    }
    return true;
}

}  // namespace pm
