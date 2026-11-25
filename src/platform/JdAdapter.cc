#include "platform/JdAdapter.h"

#include <cstring>
#include <initializer_list>
#include <nlohmann/json.hpp>

#include "core/HtmlParser.h"
#include "core/HttpClient.h"
#include "core/Logger.h"
#include "core/Util.h"
#include "platform/BrowserClient.h"

namespace pm {
namespace {

std::string jsonText(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number()) {
        double number = value.get<double>();
        return fmtPrice(number);
    }
    return "";
}

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
            size_t len = std::strlen(suffix);
            if (value.size() > len && endsWith(value, suffix)) {
                value = trim(value.substr(0, value.size() - len));
                changed = true;
            }
        }
    }
    return value;
}

std::string normalizeImage(std::string url) {
    url = trim(url);
    if (url.empty()) return url;
    if (startsWith(url, "//")) return "https:" + url;
    if (startsWith(url, "http://")) return "https://" + url.substr(7);
    return url;
}

std::optional<double> toPrice(const std::string& text) {
    if (text.empty()) return std::nullopt;
    try {
        double value = std::stod(text);
        if (value > 0) return value;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

// 从详情页 HTML 中提取价格（浏览器渲染后的页面兜底）。
std::optional<double> priceFromPage(const std::string& html) {
    static const char* patterns[] = {
        R"re(class="product-price--value">\s*([0-9]+(?:\.[0-9]+)?))re",
        R"re(class="price J-p-\d+"[^>]*>\s*([0-9]+(?:\.[0-9]+)?))re",
        R"re("p"\s*:\s*"([0-9]+(?:\.[0-9]+)?)")re",
        R"re("price"\s*:\s*"([0-9]+(?:\.[0-9]+)?)")re",
    };
    for (const char* pattern : patterns) {
        if (auto match = regexGroup(html, pattern)) {
            if (auto price = toPrice(*match)) return price;
        }
    }
    return std::nullopt;
}

}  // namespace

FetchResult JdAdapter::fetch(const std::string& productId, bool withMeta) {
    FetchResult result;

    const std::string itemUrl = "https://item.jd.com/" + productId + ".html";

    std::vector<std::string> headers = {
        "User-Agent: " + config_.userAgent,
        "Referer: " + itemUrl,
        "Accept: application/json, text/plain, */*",
    };

    // 1) 优先使用官方价格接口
    std::string priceError;
    const std::string priceUrl =
        "https://p.3.cn/prices/mgets?skuIds=J_" + productId + "&type=1";
    HttpResult priceResp =
        HttpClient::get(priceUrl, headers, config_.jd.cookie, config_.timeoutSeconds);

    if (!priceResp.error.empty()) {
        priceError = "请求京东价格接口失败: " + priceResp.error;
    } else if (priceResp.status < 200 || priceResp.status >= 300) {
        priceError = "京东价格接口返回 HTTP " + std::to_string(priceResp.status);
    } else {
        try {
            nlohmann::json json = nlohmann::json::parse(priceResp.body);
            if (!json.is_array() || json.empty()) {
                priceError = "京东价格接口返回格式异常: " + truncate(priceResp.body, 120);
            } else {
                const auto& item = json.at(0);
                std::string priceText = jsonText(item.value("p", nlohmann::json()));
                if (priceText.empty()) {
                    priceText = jsonText(item.value("price", nlohmann::json()));
                }
                if (auto price = toPrice(priceText)) {
                    result.price = *price;
                    result.inStock = true;
                    result.ok = true;
                    if (item.contains("op")) {
                        if (auto original = toPrice(jsonText(item.at("op")))) {
                            result.hasOriginal = true;
                            result.originalPrice = *original;
                        }
                    }
                    if (!result.hasOriginal && item.contains("m")) {
                        if (auto market = toPrice(jsonText(item.at("m")))) {
                            result.hasOriginal = true;
                            result.originalPrice = *market;
                        }
                    }
                    nlohmann::json raw{
                        {"source", "p.3.cn"},
                        {"p", priceText},
                        {"op", jsonText(item.value("op", nlohmann::json()))},
                        {"m", jsonText(item.value("m", nlohmann::json()))},
                    };
                    result.raw = truncate(raw.dump(), 500);
                } else {
                    priceError = "京东未返回有效价格（商品可能已下架）";
                }
            }
        } catch (const std::exception& e) {
            priceError = std::string("解析京东价格失败: ") + e.what();
        }
    }

    // 2) 价格接口不可用时，用浏览器加载详情页兜底（不需要登录）
    std::string pageHtml;
    if (!result.ok && config_.browser.enabled) {
        BrowserClient browser(BrowserClient::fromConfig(config_));
        std::string html;
        std::string browserError;
        if (browser.fetchHtml(itemUrl, "product-price--value", html, browserError)) {
            pageHtml = std::move(html);
            logInfo("京东页面采集完成: " + std::to_string(pageHtml.size()) +
                    " 字节，登录提示=" +
                    (contains(pageHtml, "欢迎登录") ? "是" : "否"));
            if (auto price = priceFromPage(pageHtml)) {
                result.price = *price;
                result.ok = true;
                result.inStock = !contains(pageHtml, "无货");
                result.raw = "{\"source\":\"browser\"}";
            } else if (contains(pageHtml, "频控")) {
                browserError =
                    "京东频控拦截（PC频控页）。可降低采集频率，或把 "
                    "crawler.browser.headless 设为 false 使用有头模式";
            } else if (contains(pageHtml, "passport.jd.com") ||
                       contains(pageHtml, "欢迎登录")) {
                browserError = "京东要求登录，请执行 ./PriceMonitor --login jd";
            } else {
                browserError = "页面中未找到价格";
            }
        }
        if (!result.ok) {
            result.error = priceError.empty()
                               ? "浏览器采集京东失败: " + browserError
                               : priceError + "；浏览器兜底失败: " + browserError;
            return result;
        }
        logInfo("京东价格接口不可用，已改用浏览器采集: " + productId);
    }

    if (!result.ok) {
        result.error = priceError.empty() ? "京东价格采集失败" : priceError;
        return result;
    }

    if (!withMeta) return result;

    // 3) 详情页元信息（标题、主图、店铺）
    if (pageHtml.empty()) {
        std::vector<std::string> pageHeaders = {
            "User-Agent: " + config_.userAgent,
            "Referer: https://www.jd.com/",
            "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        };
        HttpResult page = HttpClient::get(itemUrl, pageHeaders, config_.jd.cookie,
                                          config_.timeoutSeconds);
        if (page.error.empty() && page.status == 200) {
            pageHtml = std::move(page.body);
        }
    }

    if (!pageHtml.empty()) {
        HtmlDocument doc(pageHtml);
        if (doc.valid()) {
            std::string title = stripSuffixes(doc.title(), {" - 京东", "-京东"});
            if (title.empty()) title = doc.metaContent("og:title");
            result.title = title;

            std::string image = doc.attrById("spec-img", "data-origin");
            if (image.empty()) image = doc.attrById("spec-img", "src");
            if (image.empty()) image = doc.metaContent("og:image");
            result.imageUrl = normalizeImage(image);

            std::string shop = doc.textByClass("shopName");
            if (shop.empty()) {
                if (auto m = regexGroup(pageHtml, R"re("shopName"\s*:\s*"([^"]+)")re")) {
                    shop = *m;
                }
            }
            result.shopName = htmlUnescape(shop);
        }
    }

    return result;
}

}  // namespace pm
