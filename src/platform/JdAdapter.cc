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

double parsePriceText(const std::string& text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char ch : text) {
        if (ch != ',') cleaned.push_back(ch);  // 去掉千分位
    }
    try {
        return std::stod(cleaned);
    } catch (const std::exception&) {
        return 0;
    }
}

// 从详情页 HTML 中提取价格（浏览器渲染后的页面兜底）。
std::optional<double> priceFromPage(const std::string& html) {
    static const char* patterns[] = {
        // 新版页面：<div class="product-price--value">¥2,498.00</div>
        R"re(class="product-price--value"[^>]*>\s*[￥¥]?\s*(?:<[^>]*>\s*)*[￥¥]?\s*([0-9][0-9,]*(?:\.[0-9]+)?))re",
        // 价格块：<p class="price" id="priceSale2">¥<em>2,498.00</em></p>
        // 也覆盖老版 <span class="price J-p-100...">￥2,498.00</span>
        R"re(class="[^"]*\bprice\b[^"]*"[^>]*>\s*[￥¥]?\s*(?:<[^>]*>\s*)*[￥¥]?\s*([0-9][0-9,]*(?:\.[0-9]+)?))re",
        R"re("jdPrice"\s*:\s*"([0-9][0-9,]*(?:\.[0-9]+)?)")re",
        R"re("p"\s*:\s*"([0-9]+(?:\.[0-9]+)?)")re",
        R"re("price"\s*:\s*"([0-9]+(?:\.[0-9]+)?)")re",
    };
    for (const char* pattern : patterns) {
        if (auto match = regexGroup(html, pattern)) {
            const double price = parsePriceText(*match);
            if (price > 0) return price;
        }
    }
    return std::nullopt;
}

// 京东对未登录请求会把价格中间的数字替换成 ?（如 1??00），说明需要登录后查看。
bool hasMaskedPrice(const std::string& html, std::string* raw = nullptr) {
    if (auto match = regexGroup(html, R"re("jdPrice"\s*:\s*"([^"]*)")re")) {
        if (raw) *raw = *match;
        return match->find('?') != std::string::npos;
    }
    return false;
}

}  // namespace

FetchResult JdAdapter::fetch(const std::string& productId, bool withMeta) {
    FetchResult result;

    // item.jd.com 现在会返回一个跳转壳，把 PC 详情页转到 M 站；
    // 直接访问 M 站商品页，避免被重定向到首页。
    const std::string itemUrl =
        "https://item.m.jd.com/product/" + productId + ".html";
    std::string pageHtml;
    std::string pageError;

    // 1) 直接用浏览器加载详情页。p.3.cn 价格接口在部分网络下长期超时
    //    （每次白等 timeout），已移除，改为浏览器优先。
    if (config_.browser.enabled) {
        BrowserClient browser(BrowserClient::fromConfig(config_));
        std::string html;
        std::string browserError;
        if (browser.fetchHtml(itemUrl, "priceSale2", html, browserError)) {
            pageHtml = std::move(html);
            logInfo("京东页面采集完成: " + std::to_string(pageHtml.size()) +
                    " 字节，登录提示=" +
                    (contains(pageHtml, "欢迎登录") ? "是" : "否"));
            std::string pageTitle;
            if (auto match = regexGroup(pageHtml, R"re(<title>([^<]*)</title>)re")) {
                pageTitle = trim(*match);
            }
            std::string maskedRaw;
            const bool maskedPrice = hasMaskedPrice(pageHtml, &maskedRaw);
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
            } else if (pageTitle.find("正品低价") != std::string::npos) {
                pageError =
                    "京东把商品页重定向到了首页（商品可能已下架、需要登录或仅移动端可见）";
            } else if (maskedPrice) {
                pageError = "京东返回了隐藏价格（" + maskedRaw +
                            "），需要登录后查看：请执行 ./PriceMonitor --login jd 后重试";
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
            "Referer: https://item.m.jd.com/",
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

    if (!result.ok && !pageHtml.empty()) {
        std::string title;
        if (auto match = regexGroup(pageHtml, R"re(<title>([^<]*)</title>)re")) {
            title = trim(*match);
        }
        size_t pos = pageHtml.find("p-price");
        if (pos == std::string::npos) pos = pageHtml.find("price");
        std::string excerpt;
        if (pos != std::string::npos) {
            const size_t start = pos > 120 ? pos - 120 : 0;
            excerpt = pageHtml.substr(start, 360);
        }
        logWarn("京东页面未匹配价格，标题=" + title + "，摘录=" + excerpt);
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
            "Referer: https://item.m.jd.com/",
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
