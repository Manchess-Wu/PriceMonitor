#pragma once

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
    // 不加锁的实际采集逻辑；调用方必须已持有 crawlMutex_
    bool crawlInternal(long long id, std::string& error, bool autoCrawl);

    Database& db_;
    Notifier& notifier_;
    CrawlerConfig config_;
    // 同一时刻只允许一个采集任务，保证按顺序检索，也保证手动与定时不会并发
    std::mutex crawlMutex_;
};

}  // namespace pm
