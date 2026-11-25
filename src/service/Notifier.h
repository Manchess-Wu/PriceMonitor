#pragma once

#include <optional>
#include <string>

#include "core/Config.h"
#include "model/Models.h"

namespace pm {

class Database;

// 降价通知：目标价命中或跌幅达标时推送。当前支持 Server酱 与 企业微信机器人。
class Notifier {
public:
    explicit Notifier(const NotifierConfig& config) : config_(config) {}

    bool enabled() const { return config_.enabled; }
    const NotifierConfig& config() const { return config_; }

    void evaluate(const Product& product, const PriceRecord& record,
                  const std::optional<PriceRecord>& previous, Database& db);

private:
    void send(const std::string& title, const std::string& markdown);

    NotifierConfig config_;
};

}  // namespace pm
