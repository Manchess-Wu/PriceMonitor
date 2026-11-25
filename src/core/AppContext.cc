#include "core/AppContext.h"

#include <algorithm>
#include <filesystem>

#include "core/Database.h"
#include "core/Logger.h"
#include "core/Scheduler.h"
#include "core/Util.h"
#include "service/CrawlService.h"
#include "service/Notifier.h"

namespace pm {
namespace fs = std::filesystem;

AppContext& AppContext::instance() {
    static AppContext context;
    return context;
}

AppContext::~AppContext() { stopScheduler(); }

std::string AppContext::absolutePath(const std::string& path) const {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) return path;
    return absolute.lexically_normal().string();
}

bool AppContext::init(const std::string& configPath, const std::string& baseDir,
                      std::string& error) {
    if (!Config::load(configPath, config_, error)) return false;

    const fs::path base = baseDir.empty() ? fs::current_path() : fs::path(baseDir);
    auto resolve = [this, &base](const std::string& path) {
        const fs::path p(path);
        return absolutePath(p.is_absolute() ? p.string() : (base / p).string());
    };

    config_.dbPath = resolve(config_.dbPath);
    documentRoot_ = resolve(config_.documentRoot);
    config_.crawler.browser.profileDir = resolve(config_.crawler.browser.profileDir);
    if (!fs::exists(documentRoot_)) {
        logWarn("Web 目录不存在: " + documentRoot_ +
                "（可执行文件需与 web/index.html 一起部署）");
    }

    db_ = std::make_unique<Database>(config_.dbPath);
    if (!db_->open()) {
        error = db_->lastError();
        return false;
    }

    notifier_ = std::make_unique<Notifier>(config_.notifier);
    crawl_ = std::make_unique<CrawlService>(*db_, *notifier_, config_.crawler);

    logInfo("PriceMonitor 已初始化，数据库: " + config_.dbPath);
    logInfo("Web 目录: " + documentRoot_);
    logInfo("采集间隔: " + std::to_string(config_.crawler.intervalMinutes) +
            " 分钟（可按商品单独设置），并发 4（京东 2 / 淘宝天猫 2）");
    if (!config_.crawler.browser.enabled && config_.crawler.taobao.cookie.empty()) {
        logWarn("未配置淘宝 Cookie，淘宝/天猫商品将无法采集");
    }
    if (config_.crawler.browser.enabled) {
        logInfo("淘宝采集方式: 浏览器（配置目录 " + config_.crawler.browser.profileDir +
                "）");
    }
    if (config_.notifier.enabled && config_.notifier.type == "serverchan" &&
        config_.notifier.sendKey.empty()) {
        logWarn("已启用通知但未配置 Server酱 send_key");
    }
    return true;
}

void AppContext::startScheduler() {
    if (scheduler_ || !db_ || !crawl_) return;

    const int tickSeconds =
        std::max(5, std::min(60, config_.crawler.intervalMinutes * 60));

    // 并发 4：京东最多 2 个、淘宝/天猫共用 2 个（具体额度在 CrawlService 内控制），
    // 浏览器复用同一个 Chrome 实例的多标签页，不会再有 profile 抢占问题。
    scheduler_ = std::make_unique<Scheduler>(
        tickSeconds, 4,
        [this]() {
            return db_->listDueProducts(nowSeconds(), config_.crawler.intervalMinutes);
        },
        [this](long long id) {
            std::string error;
            bool ok = crawl_->crawl(id, error, true);
            if (!ok) {
                logWarn("定时采集失败 (id=" + std::to_string(id) + "): " + error);
            }
            return ok;
        });
    scheduler_->start();
}

void AppContext::stopScheduler() {
    if (scheduler_) {
        scheduler_->stop();
        scheduler_.reset();
    }
}

}  // namespace pm
