#include "platform/PlatformAdapter.h"

#include "platform/JdAdapter.h"
#include "platform/TaobaoAdapter.h"

namespace pm {

std::unique_ptr<PlatformAdapter> createAdapter(const std::string& platform,
                                               const CrawlerConfig& config) {
    if (platform == "jd") {
        return std::make_unique<JdAdapter>(config);
    }
    if (platform == "taobao") {
        return std::make_unique<TaobaoAdapter>(config, false);
    }
    if (platform == "tmall") {
        return std::make_unique<TaobaoAdapter>(config, true);
    }
    return nullptr;
}

}  // namespace pm
