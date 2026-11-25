#include "platform/UrlDetector.h"

#include <regex>

#include "core/HttpClient.h"
#include "core/Util.h"

namespace pm {
namespace {

std::string hostOf(const std::string& url) {
    size_t scheme = url.find("://");
    size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos
                                                                 : end - start);
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return toLower(host);
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isShortHost(const std::string& host) {
    return host == "u.jd.com" || host == "3.cn" || endsWith(host, ".3.cn") ||
           host == "m.tb.cn" || host == "e.tb.cn" ||
           host == "s.click.taobao.com";
}

bool isJdHost(const std::string& host) {
    return host.find("jd.com") != std::string::npos ||
           host.find("jd.hk") != std::string::npos;
}

bool isTmallHost(const std::string& host) {
    return host.find("tmall.com") != std::string::npos ||
           host.find("tmall.hk") != std::string::npos;
}

bool isTaobaoHost(const std::string& host) {
    return host.find("taobao.com") != std::string::npos ||
           host.find("tb.cn") != std::string::npos;
}

std::optional<std::string> matchFirst(const std::string& text,
                                      const std::string& pattern) {
    return regexGroup(text, pattern, 1);
}

}  // namespace

std::optional<DetectedProduct> detectProductUrl(const std::string& input,
                                                const CrawlerConfig& config,
                                                std::string& error) {
    std::string value = trim(input);
    if (value.empty()) {
        error = "请输入商品链接";
        return std::nullopt;
    }
    if (!startsWith(value, "http://") && !startsWith(value, "https://")) {
        value = "https://" + value;
    }

    std::string host = hostOf(value);

    // 短链接先跟随 302，拿到真实商品地址
    if (isShortHost(host)) {
        std::string cookie = isJdHost(host) || host == "3.cn" || endsWith(host, ".3.cn")
                                 ? config.jd.cookie
                                 : config.taobao.cookie;
        std::vector<std::string> headers = {"User-Agent: " + config.userAgent};
        HttpResult resp =
            HttpClient::get(value, headers, cookie, config.timeoutSeconds);
        if (!resp.error.empty()) {
            error = "短链接解析失败: " + resp.error;
            return std::nullopt;
        }
        if (!resp.effectiveUrl.empty()) {
            value = resp.effectiveUrl;
            host = hostOf(value);
        }
        if (isShortHost(host)) {
            error = "短链接未跳转到商品详情页，请复制商品页地址后重试";
            return std::nullopt;
        }
    }

    const std::string lowered = toLower(value);

    if (isJdHost(host)) {
        std::optional<std::string> id =
            matchFirst(lowered, R"(item\.jd\.com/(?:product/)?(\d+)\.html)");
        if (!id) id = matchFirst(lowered, R"(item\.m\.jd\.com/product/(\d+))");
        if (!id) id = matchFirst(lowered, R"(npcitem\.jd\.hk/(\d+)\.html)");
        if (!id) id = matchFirst(lowered, R"([?&]sku=(\d+))");
        if (!id) id = matchFirst(lowered, R"(item\.jd\.com/(\d+))");
        if (!id) {
            error = "未能从京东链接中识别商品 ID，请使用 item.jd.com/<SKU>.html 形式的链接";
            return std::nullopt;
        }
        DetectedProduct result;
        result.platform = "jd";
        result.productId = *id;
        result.url = "https://item.jd.com/" + *id + ".html";
        return result;
    }

    if (isTaobaoHost(host) || isTmallHost(host)) {
        std::optional<std::string> id = matchFirst(lowered, R"([?&]id=(\d+))");
        if (!id) {
            error = "未能从链接中识别商品 ID，请使用 item.taobao.com/item.htm?id=<ID> 形式的链接";
            return std::nullopt;
        }
        DetectedProduct result;
        result.platform = isTmallHost(host) ? "tmall" : "taobao";
        result.productId = *id;
        result.url = result.platform == "tmall"
                         ? "https://detail.tmall.com/item.htm?id=" + *id
                         : "https://item.taobao.com/item.htm?id=" + *id;
        return result;
    }

    error = "暂不支持该平台，目前仅支持京东、淘宝、天猫商品链接";
    return std::nullopt;
}

}  // namespace pm
