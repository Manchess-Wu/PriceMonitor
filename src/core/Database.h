#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "model/Models.h"

struct sqlite3;

namespace pm {

// SQLite 持久化层。所有方法内部加锁，可在多个采集线程中并发调用。
class Database {
public:
    explicit Database(std::string path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open();
    std::string lastError() const;

    // products
    bool insertProduct(Product& product);
    std::optional<Product> getProduct(long long id);
    std::optional<Product> findProduct(const std::string& platform,
                                       const std::string& productId);
    std::vector<Product> listProducts();
    std::vector<long long> listDueProducts(long long cutoff);

    bool updateProductSettings(long long id, bool hasTarget, double targetPrice,
                               const std::string& note, bool enabled);
    bool updateProductMeta(long long id, const std::string& title,
                           const std::string& shopName,
                           const std::string& imageUrl);
    bool updateCheckStatus(long long id, long long checkedAt,
                           const std::string& error);
    bool deleteProduct(long long id);

    // price_records
    bool addPriceRecord(const PriceRecord& record, long long* outId = nullptr);
    std::optional<PriceRecord> latestPrice(long long productId);
    std::vector<PriceRecord> priceHistory(long long productId, long long from,
                                          long long to, int limit);

    // 通知去重
    std::optional<double> lastNotifiedPrice(long long productId);
    bool setLastNotifiedPrice(long long productId, double price);

    bool addCrawlLog(long long productId, bool ok, const std::string& message);
    Stats getStats();

private:
    bool initSchema();
    void setError(const std::string& error);

    std::string path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
    std::string lastError_;
};

}  // namespace pm
