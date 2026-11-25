#pragma once

#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "core/Config.h"
#include "model/Models.h"

namespace pm {

class Database;
class Notifier;

// 采集业务流程：新增商品、单次采集、查询与统计。
// 返回 JSON 以便控制器直接输出；code 为建议的 HTTP 状态码。
class CrawlService {
public:
    CrawlService(Database& db, Notifier& notifier, CrawlerConfig config)
        : db_(db), notifier_(notifier), config_(std::move(config)) {}

    nlohmann::json addProduct(const std::string& url, bool hasTarget,
                              double targetPrice, const std::string& note,
                              int intervalMinutes, std::string& error, int& code);

    nlohmann::json listProducts();
    nlohmann::json getProduct(long long id, std::string& error, int& code);
    nlohmann::json updateProduct(long long id, const nlohmann::json& patch,
                                 std::string& error, int& code);
    nlohmann::json deleteProduct(long long id, std::string& error, int& code);
    nlohmann::json refreshProduct(long long id, std::string& error, int& code);
    nlohmann::json priceHistory(long long id, long long from, long long to,
                                int limit, std::string& error, int& code);
    nlohmann::json stats();

    // 供调度器调用：执行一次采集并落库。autoCrawl=true 表示定时采集，
    // 会推进自动采集计划时间；手动检索不推进。
    bool crawl(long long id, std::string& error, bool autoCrawl = false);

private:
    // 平台并发额度：同一 IP 下京东最多 2 个、淘宝/天猫（同一家）最多 2 个，
    // 合计最多 4 个采集同时进行。
    struct CrawlLimit {
        explicit CrawlLimit(int limit) : maxActive(limit) {}
        std::mutex mutex;
        std::condition_variable cv;
        int active = 0;
        int maxActive;
    };

    CrawlLimit& limitFor(const std::string& platform);

    // 实际的采集逻辑；并发额度由调用方负责
    bool crawlInternal(long long id, std::string& error, bool autoCrawl);

    Database& db_;
    Notifier& notifier_;
    CrawlerConfig config_;
    CrawlLimit jdLimit_{2};
    CrawlLimit taobaoLimit_{2};
};

}  // namespace pm
