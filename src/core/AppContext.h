#pragma once

#include <memory>
#include <string>

#include "core/Config.h"

namespace pm {

class Database;
class Notifier;
class CrawlService;
class Scheduler;

// 进程级单例，持有配置、数据库、采集服务与调度器。
class AppContext {
public:
    static AppContext& instance();

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    // baseDir 用于解析配置中相对路径（web、data 等），为空时使用当前工作目录。
    bool init(const std::string& configPath, const std::string& baseDir, std::string& error);

    Config& config() { return config_; }
    Database& db() { return *db_; }
    Notifier& notifier() { return *notifier_; }
    CrawlService& crawl() { return *crawl_; }
    const std::string& documentRoot() const { return documentRoot_; }

    void startScheduler();
    void stopScheduler();

private:
    AppContext() = default;
    ~AppContext();
    std::string absolutePath(const std::string& path) const;

    Config config_;
    std::string documentRoot_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<Notifier> notifier_;
    std::unique_ptr<CrawlService> crawl_;
    std::unique_ptr<Scheduler> scheduler_;
};

}  // namespace pm
