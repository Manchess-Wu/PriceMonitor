#include "service/Notifier.h"

#include <cmath>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Database.h"
#include "core/HttpClient.h"
#include "core/Logger.h"
#include "core/Util.h"

namespace pm {

void Notifier::evaluate(const Product& product, const PriceRecord& record,
                        const std::optional<PriceRecord>& previous, Database& db) {
    if (!config_.enabled || !record.inStock) return;

    std::vector<std::string> reasons;

    if (product.hasTarget && record.price <= product.targetPrice) {
        reasons.push_back("已达到目标价 ¥" + fmtPrice(product.targetPrice));
    }

    if (config_.dropPercent > 0 && previous && previous->price > 0) {
        double drop = (previous->price - record.price) / previous->price * 100.0;
        if (drop >= config_.dropPercent) {
            reasons.push_back("较上次下降 " + fmtPrice(drop) + "%（¥" +
                              fmtPrice(previous->price) + " → ¥" +
                              fmtPrice(record.price) + "）");
        }
    }

    if (reasons.empty()) return;

    // 同一价格只提醒一次，避免每次采集重复推送
    auto lastNotified = db.lastNotifiedPrice(product.id);
    if (lastNotified && std::fabs(*lastNotified - record.price) < 0.005) return;

    const std::string title =
        "【降价提醒】" + truncate(product.displayTitle(), 40) + " 现价 ¥" +
        fmtPrice(record.price);

    std::string content;
    content += "**商品**：" + product.displayTitle() + "\n\n";
    content += "**当前价**：¥" + fmtPrice(record.price) + "\n\n";
    if (product.hasTarget) {
        content += "**目标价**：¥" + fmtPrice(product.targetPrice) + "\n\n";
    }
    content += "**触发原因**：\n";
    for (const auto& reason : reasons) content += "- " + reason + "\n";
    content += "\n[查看商品](" + product.url + ")\n\n";
    content += "采集时间：" + formatTime(record.capturedAt);

    send(title, content);

    if (db.setLastNotifiedPrice(product.id, record.price)) {
        logInfo("已发送降价通知: " + product.displayTitle() + " ¥" +
                fmtPrice(record.price));
    }
}

void Notifier::send(const std::string& title, const std::string& markdown) {
    if (config_.type == "serverchan") {
        if (config_.sendKey.empty()) {
            logWarn("通知类型为 serverchan，但未配置 send_key，跳过推送");
            return;
        }
        const std::string url =
            "https://sctapi.ftqq.com/" + config_.sendKey + ".send";
        const std::string body =
            "title=" + urlEncode(truncate(title, 100)) + "&desp=" + urlEncode(markdown);
        HttpResult resp = HttpClient::postForm(url, body);
        if (!resp.ok()) {
            logWarn("Server酱推送失败: " +
                    (resp.error.empty() ? ("HTTP " + std::to_string(resp.status))
                                        : resp.error));
        }
        return;
    }

    if (config_.type == "wecom") {
        if (config_.wecomWebhook.empty()) {
            logWarn("通知类型为 wecom，但未配置 wecom_webhook，跳过推送");
            return;
        }
        nlohmann::json payload{
            {"msgtype", "markdown"},
            {"markdown", {{"content", "### " + title + "\n\n" + markdown}}},
        };
        HttpResult resp =
            HttpClient::postJson(config_.wecomWebhook, payload.dump());
        if (!resp.ok()) {
            logWarn("企业微信推送失败: " +
                    (resp.error.empty() ? ("HTTP " + std::to_string(resp.status))
                                        : resp.error));
        }
        return;
    }

    logWarn("未知的通知类型: " + config_.type + "（支持 serverchan / wecom）");
}

}  // namespace pm
