#pragma once

#include "platform/PlatformAdapter.h"

namespace pm {

class JdAdapter : public PlatformAdapter {
public:
    explicit JdAdapter(const CrawlerConfig& config) : config_(config) {}

    std::string platform() const override { return "jd"; }
    FetchResult fetch(const std::string& productId, bool withMeta) override;

private:
    CrawlerConfig config_;
};

}  // namespace pm
