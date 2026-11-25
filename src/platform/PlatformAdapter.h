#pragma once

#include <memory>
#include <string>

#include "core/Config.h"

namespace pm {

struct FetchResult {
    bool ok = false;
    double price = 0.0;
    bool hasOriginal = false;
    double originalPrice = 0.0;
    std::string title;
    std::string shopName;
    std::string imageUrl;
    bool inStock = true;
    std::string raw;
    std::string error;
    bool loginRequired = false;
};

class PlatformAdapter {
public:
    virtual ~PlatformAdapter() = default;
    virtual std::string platform() const = 0;
    // withMeta 为 true 时额外抓取标题、店铺、主图等信息
    virtual FetchResult fetch(const std::string& productId, bool withMeta) = 0;
};

std::unique_ptr<PlatformAdapter> createAdapter(const std::string& platform,
                                               const CrawlerConfig& config);

}  // namespace pm
