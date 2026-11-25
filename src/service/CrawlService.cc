#include "service/CrawlService.h"

#include "core/Database.h"
#include "core/Logger.h"
#include "core/Util.h"
#include "platform/PlatformAdapter.h"
#include "platform/UrlDetector.h"
#include "service/Notifier.h"

namespace pm {

nlohmann::json CrawlService::addProduct(const std::string& url, bool hasTarget,
                                        double targetPrice, const std::string& note,
                                        int intervalMinutes, std::string& error,
                                        int& code) {
    if (hasTarget && !(targetPrice > 0)) {
        error = "目标价必须大于 0";
        code = 400;
        return nlohmann::json();
    }

    std::string detectError;
    auto detected = detectProductUrl(url, config_, detectError);
    if (!detected) {
        error = detectError;
        code = 400;
        return nlohmann::json();
    }

    if (auto existing = db_.findProduct(detected->platform, detected->productId)) {
        error = "该商品已在监控列表中";
        code = 409;
        return nlohmann::json();
    }

    Product product;
    product.platform = detected->platform;
    product.productId = detected->productId;
    product.url = detected->url;
    product.note = note;
    product.hasTarget = hasTarget;
    product.targetPrice = hasTarget ? targetPrice : 0.0;
    product.enabled = true;
    product.createdAt = nowSeconds();
    product.updatedAt = product.createdAt;
    product.intervalMinutes = intervalMinutes > 0 ? intervalMinutes : 0;

    FetchResult fetchResult;
    CrawlLimit& limit = limitFor(product.platform);
    {
        std::unique_lock<std::mutex> lock(limit.mutex);
        limit.cv.wait(lock, [&limit] { return limit.active < limit.maxActive; });
        ++limit.active;
    }
    if (auto adapter = createAdapter(product.platform, config_)) {
        fetchResult = adapter->fetch(product.productId, true);
    } else {
        fetchResult.error = "不支持的平台: " + product.platform;
    }
    {
        std::lock_guard<std::mutex> lock(limit.mutex);
        --limit.active;
    }
    limit.cv.notify_one();

    const int64_t checkedAt = nowSeconds();
    product.hasLastChecked = true;
    product.lastCheckedAt = checkedAt;
    // 首次采集算作一次自动采集，下一次到期时间 = 加入时间 + 间隔
    product.lastAutoCheckedAt = checkedAt;

    if (fetchResult.ok) {
        product.title = fetchResult.title;
        product.shopName = fetchResult.shopName;
        product.imageUrl = fetchResult.imageUrl;
        product.hasLastPrice = true;
        product.lastPrice = fetchResult.price;
        product.hasLastStock = true;
        product.lastInStock = fetchResult.inStock;
        product.hasLastCapture = true;
        product.lastCaptureAt = checkedAt;
        product.lastError.clear();
    } else {
        product.lastError = fetchResult.error;
    }

    if (!db_.insertProduct(product)) {
        error = db_.lastError();
        code = 500;
        return nlohmann::json();
    }

    if (fetchResult.ok) {
        PriceRecord record;
        record.productId = product.id;
        record.price = fetchResult.price;
        record.hasOriginal = fetchResult.hasOriginal;
        record.originalPrice = fetchResult.originalPrice;
        record.inStock = fetchResult.inStock;
        record.capturedAt = checkedAt;
        record.raw = fetchResult.raw;
        if (!db_.addPriceRecord(record)) {
            logWarn("保存首次价格失败: " + db_.lastError());
        }
        db_.addCrawlLog(product.id, true, "");
    } else {
        db_.addCrawlLog(product.id, false, fetchResult.error);
        logWarn("新增商品时首次采集失败: " + product.displayTitle() + " - " +
                fetchResult.error);
    }

    code = 201;
    return productToJson(product);
}

nlohmann::json CrawlService::listProducts() {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& product : db_.listProducts()) {
        list.push_back(productToJson(product));
    }
    return list;
}

nlohmann::json CrawlService::getProduct(long long id, std::string& error, int& code) {
    auto product = db_.getProduct(id);
    if (!product) {
        error = "商品不存在";
        code = 404;
        return nlohmann::json();
    }
    code = 200;
    return productToJson(*product);
}

nlohmann::json CrawlService::updateProduct(long long id, const nlohmann::json& patch,
                                           std::string& error, int& code) {
    auto product = db_.getProduct(id);
    if (!product) {
        error = "商品不存在";
        code = 404;
        return nlohmann::json();
    }

    bool hasTarget = product->hasTarget;
    double targetPrice = product->targetPrice;
    std::string note = product->note;
    bool enabled = product->enabled;
    int intervalMinutes = product->intervalMinutes;

    if (patch.contains("targetPrice")) {
        const auto& value = patch.at("targetPrice");
        if (value.is_null()) {
            hasTarget = false;
            targetPrice = 0.0;
        } else if (value.is_number()) {
            double parsed = value.get<double>();
            if (!(parsed > 0)) {
                error = "目标价必须大于 0";
                code = 400;
                return nlohmann::json();
            }
            hasTarget = true;
            targetPrice = parsed;
        } else {
            error = "targetPrice 必须是数字或 null";
            code = 400;
            return nlohmann::json();
        }
    }
    if (patch.contains("note")) {
        if (!patch.at("note").is_string()) {
            error = "note 必须是字符串";
            code = 400;
            return nlohmann::json();
        }
        note = patch.at("note").get<std::string>();
    }
    if (patch.contains("enabled")) {
        if (!patch.at("enabled").is_boolean()) {
            error = "enabled 必须是布尔值";
            code = 400;
            return nlohmann::json();
        }
        enabled = patch.at("enabled").get<bool>();
    }
    if (patch.contains("intervalMinutes")) {
        const auto& value = patch.at("intervalMinutes");
        if (value.is_null()) {
            intervalMinutes = 0;  // 0 = 使用全局默认间隔
        } else if (value.is_number_integer()) {
            const long long parsed = value.get<long long>();
            if (parsed < 0 || parsed > 10080) {
                error = "检索间隔必须是 0-10080 之间的整数（分钟）";
                code = 400;
                return nlohmann::json();
            }
            intervalMinutes = static_cast<int>(parsed);
        } else {
            error = "intervalMinutes 必须是整数或 null";
            code = 400;
            return nlohmann::json();
        }
    }

    if (!db_.updateProductSettings(id, hasTarget, targetPrice, note, enabled,
                                   intervalMinutes)) {
        error = db_.lastError();
        code = 500;
        return nlohmann::json();
    }

    auto updated = db_.getProduct(id);
    code = 200;
    return updated ? productToJson(*updated) : nlohmann::json();
}

nlohmann::json CrawlService::deleteProduct(long long id, std::string& error,
                                           int& code) {
    if (!db_.getProduct(id)) {
        error = "商品不存在";
        code = 404;
        return nlohmann::json();
    }
    if (!db_.deleteProduct(id)) {
        error = db_.lastError();
        code = 500;
        return nlohmann::json();
    }
    code = 200;
    return nlohmann::json{{"id", id}};
}

CrawlService::CrawlLimit& CrawlService::limitFor(const std::string& platform) {
    // 淘宝与天猫属于同一家，共用 2 个并发额度
    return platform == "jd" ? jdLimit_ : taobaoLimit_;
}

nlohmann::json CrawlService::refreshProduct(long long id, std::string& error,
                                            int& code) {
    auto product = db_.getProduct(id);
    if (!product) {
        error = "商品不存在";
        code = 404;
        return nlohmann::json();
    }

    // 手动检索：该平台额度已满时直接拒绝弹窗，不排队、不打断已开始的采集
    CrawlLimit& limit = limitFor(product->platform);
    {
        std::lock_guard<std::mutex> lock(limit.mutex);
        if (limit.active >= limit.maxActive) {
            error = "该平台已有 " + std::to_string(limit.maxActive) +
                    " 个采集在进行，请稍后再试";
            code = 409;
            return nlohmann::json();
        }
        ++limit.active;
    }

    std::string crawlError;
    const bool ok = crawlInternal(id, crawlError, false);
    {
        std::lock_guard<std::mutex> lock(limit.mutex);
        --limit.active;
    }
    limit.cv.notify_one();

    if (!ok) {
        error = crawlError;
        code = 502;
        return nlohmann::json();
    }
    return getProduct(id, error, code);
}

bool CrawlService::crawl(long long id, std::string& error, bool autoCrawl) {
    auto product = db_.getProduct(id);
    if (!product) {
        error = "商品不存在";
        return false;
    }

    // 自动采集：额度满时等待，保证不会超过每个平台的并发上限
    CrawlLimit& limit = limitFor(product->platform);
    {
        std::unique_lock<std::mutex> lock(limit.mutex);
        limit.cv.wait(lock, [&limit] { return limit.active < limit.maxActive; });
        ++limit.active;
    }

    const bool ok = crawlInternal(id, error, autoCrawl);
    {
        std::lock_guard<std::mutex> lock(limit.mutex);
        --limit.active;
    }
    limit.cv.notify_one();
    return ok;
}

bool CrawlService::crawlInternal(long long id, std::string& error, bool autoCrawl) {
    auto product = db_.getProduct(id);
    if (!product) {
        error = "商品不存在";
        return false;
    }

    auto previous = db_.latestPrice(id);
    const bool withMeta = product->title.empty() || product->imageUrl.empty();

    FetchResult fetchResult;
    if (auto adapter = createAdapter(product->platform, config_)) {
        fetchResult = adapter->fetch(product->productId, withMeta);
    } else {
        fetchResult.error = "不支持的平台: " + product->platform;
    }

    const int64_t checkedAt = nowSeconds();

    if (!fetchResult.ok) {
        db_.updateCheckStatus(id, checkedAt, fetchResult.error, autoCrawl);
        db_.addCrawlLog(id, false, fetchResult.error);
        error = fetchResult.error;
        return false;
    }

    if (!fetchResult.title.empty() || !fetchResult.imageUrl.empty() ||
        !fetchResult.shopName.empty()) {
        db_.updateProductMeta(id, fetchResult.title, fetchResult.shopName,
                              fetchResult.imageUrl);
    }

    PriceRecord record;
    record.productId = id;
    record.price = fetchResult.price;
    record.hasOriginal = fetchResult.hasOriginal;
    record.originalPrice = fetchResult.originalPrice;
    record.inStock = fetchResult.inStock;
    record.capturedAt = checkedAt;
    record.raw = fetchResult.raw;

    if (!db_.addPriceRecord(record)) {
        error = db_.lastError();
        db_.updateCheckStatus(id, checkedAt, error, autoCrawl);
        return false;
    }

    db_.updateCheckStatus(id, checkedAt, "", autoCrawl);
    db_.addCrawlLog(id, true, "");
    notifier_.evaluate(*product, record, previous, db_);
    return true;
}

nlohmann::json CrawlService::priceHistory(long long id, long long from, long long to,
                                          int limit, std::string& error, int& code) {
    auto product = db_.getProduct(id);
    if (!product) {
        error = "商品不存在";
        code = 404;
        return nlohmann::json();
    }

    auto records = db_.priceHistory(id, from, to, limit);

    nlohmann::json list = nlohmann::json::array();
    double minPrice = 0.0;
    double maxPrice = 0.0;
    double sum = 0.0;
    int validCount = 0;
    for (const auto& record : records) {
        list.push_back(priceToJson(record));
        if (record.price > 0) {
            if (validCount == 0 || record.price < minPrice) minPrice = record.price;
            if (validCount == 0 || record.price > maxPrice) maxPrice = record.price;
            sum += record.price;
            ++validCount;
        }
    }

    nlohmann::json summary{
        {"count", records.size()},
        {"min", validCount > 0 ? nlohmann::json(round2(minPrice)) : nlohmann::json(nullptr)},
        {"max", validCount > 0 ? nlohmann::json(round2(maxPrice)) : nlohmann::json(nullptr)},
        {"avg", validCount > 0 ? nlohmann::json(round2(sum / validCount))
                               : nlohmann::json(nullptr)},
    };

    code = 200;
    return nlohmann::json{
        {"product", productToJson(*product)},
        {"records", list},
        {"summary", summary},
    };
}

nlohmann::json CrawlService::stats() {
    Stats stats = db_.getStats();
    return nlohmann::json{
        {"products", stats.products},
        {"enabled", stats.enabled},
        {"records", stats.records},
        {"records24h", stats.records24h},
        {"intervalMinutes", config_.intervalMinutes},
        {"notifierEnabled", notifier_.enabled()},
        {"taobaoCookieConfigured", !config_.taobao.cookie.empty()},
    };
}

}  // namespace pm
