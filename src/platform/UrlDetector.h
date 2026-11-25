#pragma once

#include <optional>
#include <string>

#include "core/Config.h"

namespace pm {

struct DetectedProduct {
    std::string platform;   // jd | taobao | tmall
    std::string productId;  // SKU / 商品 ID
    std::string url;        // 规范化链接
};

// 识别商品链接所属平台与商品 ID，并自动跟随短链接。
std::optional<DetectedProduct> detectProductUrl(const std::string& input,
                                                const CrawlerConfig& config,
                                                std::string& error);

}  // namespace pm
