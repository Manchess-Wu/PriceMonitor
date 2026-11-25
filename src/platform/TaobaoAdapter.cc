#include "platform/TaobaoAdapter.h"

#include <initializer_list>
#include <nlohmann/json.hpp>

#include "core/HtmlParser.h"
#include "core/HttpClient.h"
#include "core/Logger.h"
#include "core/Util.h"
#include "platform/BrowserClient.h"

namespace pm {
namespace {

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string stripSuffixes(std::string value,
                          std::initializer_list<const char*> suffixes) {
    value = trim(value);
    bool changed = true;
    while (changed && !value.empty()) {
        changed = false;
        for (const char* suffix : suffixes) {
            std::string s(suffix);
            if (value.size() > s.size() && endsWith(value, s)) {
                value = trim(value.substr(0, value.size() - s.size()));
                changed = true;
            }
        }
    }
    return value;
}

// 从 "¥19.90"、"19.90-29.90" 之类的文本中取第一个有效价格
std::optional<double> extractPrice(const std::string& text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char c : text) {
        if (c == ',' || c == ' ') continue;
        cleaned.push_back(c);
    }
    auto match = regexGroup(cleaned, R"(([0-9]+(?:\.[0-9]+)?))", 1);
    if (!match) return std::nullopt;
    try {
        double value = std::stod(*match);
        if (value > 0) return value;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::string unescapeSlashes(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '/') {
            out.push_back('/');
            ++i;
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string normalizeImage(const std::string& raw) {
    std::string url = unescapeSlashes(trim(htmlUnescape(raw)));
    if (url.empty()) return url;
    if (startsWith(url, "//")) return "https:" + url;
    if (startsWith(url, "http://")) return "https://" + url.substr(7);
    return url;
}

}  // namespace

FetchResult TaobaoAdapter::fetch(const std::string& productId, bool withMeta) {
    FetchResult result;

    const std::string ua = config_.taobao.userAgent.empty()
                               ? config_.userAgent
                               : config_.taobao.userAgent;
    const std::string url =
        tmall_ ? "https://detail.tmall.com/item.htm?id=" + productId
               : "https://item.taobao.com/item.htm?id=" + productId;

    // 淘宝商品页需要执行页面 JS 才能通过风控，优先用本机浏览器加载。
    std::string body;
    bool viaBrowser = false;
    if (config_.browser.enabled) {
        BrowserClient browser(BrowserClient::fromConfig(config_));
        std::string html;
        std::string browserError;
        if (browser.fetchHtml(url, "priceText", html, browserError)) {
            body = std::move(html);
            viaBrowser = true;
        } else {
            logWarn("浏览器采集淘宝失败，回退到普通 HTTP: " + browserError +
                    "（可执行 ./PriceMonitor --login 重新登录）");
        }
    }

    if (body.empty()) {
        if (trim(config_.taobao.cookie).empty()) {
            result.error =
                "未配置淘宝 Cookie，且浏览器采集不可用。请配置 crawler.taobao.cookie，"
                "或启用 crawler.browser 并执行 ./PriceMonitor --login";
            result.loginRequired = true;
            return result;
        }

        std::vector<std::string> headers = {
            "User-Agent: " + ua,
            "Referer: https://www.taobao.com/",
            "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        };

        HttpResult page =
            HttpClient::get(url, headers, config_.taobao.cookie, config_.timeoutSeconds);
        if (!page.error.empty()) {
            result.error = "请求淘宝页面失败: " + page.error;
            return result;
        }
        if (page.status == 403 || page.status == 429) {
            result.error = "淘宝返回 HTTP " + std::to_string(page.status) +
                           "，触发限流或风控，请更新 Cookie 后稍后重试";
            return result;
        }
        if (page.status != 200) {
            result.error = "淘宝页面返回 HTTP " + std::to_string(page.status);
            return result;
        }
        body = std::move(page.body);
    }

    HtmlDocument doc(body);

    if (doc.title() == "登录" || contains(body, "havanaone")) {
        result.error = viaBrowser
                           ? "淘宝登录已失效，请执行 ./PriceMonitor --login 重新登录"
                           : "淘宝登录态已失效，请重新获取 Cookie";
        result.loginRequired = true;
        return result;
    }
    const bool hasPriceText = contains(body, "\"priceText\"");
    if (!hasPriceText &&
        (contains(body, "x5secdata") || contains(body, "_____tmd_____") ||
         contains(body, "baxia") || contains(body, "滑动验证"))) {
        result.error = "被淘宝风控拦截（出现验证页），请稍后重试或重新登录";
        result.loginRequired = true;
        return result;
    }
    if (contains(body, "宝贝不存在") || contains(body, "您查看的宝贝不存在") ||
        contains(body, "此商品已下架")) {
        result.error = "商品不存在或已下架";
        return result;
    }

    // 1. 价格：优先使用 DOM 节点，其次使用页面内嵌 JSON
    std::optional<double> price;
    std::string priceSource;
    if (doc.valid()) {
        const char* priceIds[] = {"J_PromoPriceNum", "J_StrPriceModBox", "J_StrPrice"};
        for (const char* id : priceIds) {
            std::string text = doc.textById(id);
            if (auto value = extractPrice(text)) {
                price = value;
                priceSource = text;
                break;
            }
        }
        if (!price) {
            std::string text = doc.textByClass("tb-rmb-num");
            if (auto value = extractPrice(text)) {
                price = value;
                priceSource = text;
            }
        }
    }

    if (!price) {
        const char* patterns[] = {
            R"re("priceText"\s*:\s*"([^"]*)")re",
            R"re("promotionPrice"\s*:\s*"([^"]*)")re",
            R"re("price"\s*:\s*"([^"]*)")re",
            R"re("defaultItemPrice"\s*:\s*"([^"]*)")re",
        };
        for (const char* pattern : patterns) {
            if (auto raw = regexGroup(body, pattern, 1)) {
                if (auto value = extractPrice(*raw)) {
                    price = value;
                    priceSource = *raw;
                    break;
                }
            }
        }
    }

    if (!price) {
        result.error = "未能解析到价格（可能页面改版、登录态失效或触发风控）";
        return result;
    }

    result.price = *price;
    result.inStock = true;

    // 2. 标题
    std::string title = stripSuffixes(doc.title(),
                                      {"-淘宝网", "-tmall.com天猫", "-tmall.com 天猫",
                                       "-天猫Tmall.com", "-天猫tmall.com",
                                       "-天猫", "-淘宝", "淘宝网", "天猫"});
    if (title.empty()) title = doc.metaContent("og:title");
    result.title = title;

    // 3. 店铺
    std::string shop = doc.textByClass("shop-name");
    if (shop.empty()) shop = doc.textByClass("shopName");
    if (shop.empty()) {
        if (auto m = regexGroup(body, R"re("shopName"\s*:\s*"([^"]+)")re")) shop = *m;
    }
    result.shopName = htmlUnescape(shop);

    // 4. 主图
    std::string image = doc.attrById("J_ImgBooth", "src");
    if (image.empty()) image = doc.attrById("J_ImgBooth", "data-src");
    if (image.empty()) image = doc.metaContent("og:image");
    if (image.empty()) {
        if (auto m = regexGroup(body, R"re("picUrl"\s*:\s*"([^"]+)")re")) image = *m;
    }
    if (image.empty()) {
        if (auto m = regexGroup(body, R"re("images"\s*:\s*\[\s*"([^"]+)")re")) image = *m;
    }
    result.imageUrl = normalizeImage(image);

    // 5. 划线价 / 原价
    const char* originalPatterns[] = {
        R"re("originalPrice"\s*:\s*"([^"]*)")re",
        R"re("reservePrice"\s*:\s*"([^"]*)")re",
        R"re("oriPrice"\s*:\s*"([^"]*)")re",
    };
    for (const char* pattern : originalPatterns) {
        if (auto raw = regexGroup(body, pattern, 1)) {
            if (auto value = extractPrice(*raw)) {
                result.hasOriginal = true;
                result.originalPrice = *value;
                break;
            }
        }
    }

    nlohmann::json raw{
        {"source", tmall_ ? "tmall" : "taobao"},
        {"price", fmtPrice(result.price)},
        {"priceText", truncate(priceSource, 60)},
        {"hasOriginal", result.hasOriginal},
        {"originalPrice", result.hasOriginal ? fmtPrice(result.originalPrice) : ""},
    };
    result.raw = truncate(raw.dump(), 500);
    result.ok = true;
    return result;
}

}  // namespace pm
