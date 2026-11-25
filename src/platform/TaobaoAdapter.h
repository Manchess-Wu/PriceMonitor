#pragma once

#include "platform/PlatformAdapter.h"

namespace pm {

// 淘宝与天猫页面结构一致，通过 tmall 标记区分平台
class TaobaoAdapter : public PlatformAdapter {
public:
    TaobaoAdapter(const CrawlerConfig& config, bool tmall)
        : config_(config), tmall_(tmall) {}

    std::string platform() const override { return tmall_ ? "tmall" : "taobao"; }
    FetchResult fetch(const std::string& productId, bool withMeta) override;

private:
    CrawlerConfig config_;
    bool tmall_ = false;
};

}  // namespace pm
