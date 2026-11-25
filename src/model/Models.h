#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/Util.h"

namespace pm {

struct Product {
    long long id = 0;
    std::string platform;   // jd | taobao | tmall
    std::string productId;  // 平台商品 ID / SKU
    std::string url;
    std::string title;
    std::string shopName;
    std::string imageUrl;
    std::string note;

    bool hasTarget = false;
    double targetPrice = 0.0;
    bool enabled = true;

    long long createdAt = 0;
    long long updatedAt = 0;

    bool hasLastChecked = false;
    long long lastCheckedAt = 0;
    std::string lastError;

    bool hasLastPrice = false;
    double lastPrice = 0.0;
    bool hasPrevPrice = false;
    double prevPrice = 0.0;
    bool hasLastCapture = false;
    long long lastCaptureAt = 0;
    bool hasLastStock = false;
    bool lastInStock = true;

    std::string displayTitle() const {
        if (!title.empty()) return title;
        return platform + " " + productId;
    }
};

struct PriceRecord {
    long long id = 0;
    long long productId = 0;
    double price = 0.0;
    bool hasOriginal = false;
    double originalPrice = 0.0;
    bool inStock = true;
    long long capturedAt = 0;
    std::string raw;
};

struct Stats {
    long long products = 0;
    long long enabled = 0;
    long long records = 0;
    long long records24h = 0;
};

inline nlohmann::json productToJson(const Product& p) {
    nlohmann::json j;
    j["id"] = p.id;
    j["platform"] = p.platform;
    j["productId"] = p.productId;
    j["url"] = p.url;
    j["title"] = p.displayTitle();
    j["shopName"] = p.shopName;
    j["imageUrl"] = p.imageUrl;
    j["note"] = p.note;
    j["enabled"] = p.enabled;

    j["targetPrice"] = p.hasTarget ? nlohmann::json(p.targetPrice) : nlohmann::json(nullptr);
    j["createdAt"] = p.createdAt;
    j["createdAtText"] = formatTime(p.createdAt);
    j["updatedAt"] = p.updatedAt;

    j["lastCheckedAt"] =
        p.hasLastChecked ? nlohmann::json(p.lastCheckedAt) : nlohmann::json(nullptr);
    j["lastCheckedText"] = p.hasLastChecked ? formatTime(p.lastCheckedAt) : "";
    j["lastError"] = p.lastError;

    j["lastPrice"] = p.hasLastPrice ? nlohmann::json(p.lastPrice) : nlohmann::json(nullptr);
    j["previousPrice"] =
        p.hasPrevPrice ? nlohmann::json(p.prevPrice) : nlohmann::json(nullptr);
    j["lastCaptureAt"] =
        p.hasLastCapture ? nlohmann::json(p.lastCaptureAt) : nlohmann::json(nullptr);
    j["inStock"] = p.hasLastStock ? p.lastInStock : true;

    double change = 0.0;
    bool hasChange = false;
    if (p.hasLastPrice && p.hasPrevPrice && p.prevPrice > 0) {
        change = round2((p.lastPrice - p.prevPrice) / p.prevPrice * 100.0);
        hasChange = true;
    }
    j["changePercent"] = hasChange ? nlohmann::json(change) : nlohmann::json(nullptr);
    return j;
}

inline nlohmann::json priceToJson(const PriceRecord& r) {
    nlohmann::json j;
    j["id"] = r.id;
    j["productId"] = r.productId;
    j["price"] = r.price;
    j["originalPrice"] = r.hasOriginal ? nlohmann::json(r.originalPrice) : nlohmann::json(nullptr);
    j["inStock"] = r.inStock;
    j["capturedAt"] = r.capturedAt;
    j["capturedAtText"] = formatTime(r.capturedAt);
    return j;
}

}  // namespace pm
