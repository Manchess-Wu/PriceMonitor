#include "platform/JdAdapter.h"

#include <cstring>
#include <initializer_list>

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
    std::string pageHtml;
    std::string pageError;

    // 1) 直接用浏览器加载详情页。p.3.cn 价格接口在部分网络下长期超时
    //    （每次白等 timeout），已移除，改为浏览器优先。
    if (config_.browser.enabled) {
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
                pageError =
                    "京东频控拦截（PC频控页）。可降低采集频率，或把 "
                    "crawler.browser.headless 设为 false 使用有头模式";
            } else if (contains(pageHtml, "京东验证") ||
                       contains(pageHtml, "JDR_shields")) {
                pageError = "京东风控验证（京东验证页），请降低采集频率后重试";
            } else if (contains(pageHtml, "passport.jd.com") ||
                       contains(pageHtml, "欢迎登录")) {
                pageError = "京东要求登录，请执行 ./PriceMonitor --login jd";
            } else {
                pageError = "页面中未找到价格";
            }
        } else {
            pageError = "浏览器采集京东失败: " + browserError;
        }
    } else {
        pageError = "浏览器采集未启用（crawler.browser.enabled=false）";
    }

    // 2) 浏览器不可用时，用普通 HTTP 拉详情页兜底（可带登录 Cookie），
    //    静态 HTML 里通常也内嵌了价格字段。
    if (!result.ok) {
        std::vector<std::string> pageHeaders = {
            "User-Agent: " + config_.userAgent,
            "Referer: https://www.jd.com/",
            "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        };
        HttpResult page = HttpClient::get(itemUrl, pageHeaders, config_.jd.cookie,
                                          config_.timeoutSeconds);
        if (page.error.empty() && page.status == 200) {
            if (pageHtml.empty()) pageHtml = page.body;
            if (auto price = priceFromPage(pageHtml)) {
                result.price = *price;
                result.ok = true;
                result.inStock = !contains(pageHtml, "无货");
                result.raw = "{\"source\":\"http\"}";
            }
        } else if (pageError.empty()) {
            pageError = page.error.empty()
                            ? "京东详情页返回 HTTP " + std::to_string(page.status)
                            : "请求京东详情页失败: " + page.error;
        }
    }

    if (!result.ok) {
        result.error = pageError;
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
