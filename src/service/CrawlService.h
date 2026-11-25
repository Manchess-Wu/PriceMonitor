#pragma once

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
                              std::string& error, int& code);

    nlohmann::json listProducts();
    nlohmann::json getProduct(long long id, std::string& error, int& code);
    nlohmann::json updateProduct(long long id, const nlohmann::json& patch,
                                 std::string& error, int& code);
    nlohmann::json deleteProduct(long long id, std::string& error, int& code);
    nlohmann::json refreshProduct(long long id, std::string& error, int& code);
    nlohmann::json priceHistory(long long id, long long from, long long to,
                                int limit, std::string& error, int& code);
    nlohmann::json stats();

    // 供调度器调用：执行一次采集并落库
    bool crawl(long long id, std::string& error);

private:
    Database& db_;
    Notifier& notifier_;
    CrawlerConfig config_;
};

}  // namespace pm
