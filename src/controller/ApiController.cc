#include "controller/ApiController.h"

#include <nlohmann/json.hpp>

#include "core/AppContext.h"
#include "core/Database.h"
#include "core/Util.h"
#include "service/CrawlService.h"

namespace pm {
namespace {

HttpResponsePtr jsonResponse(const nlohmann::json& payload, int statusCode) {
    auto response = HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<HttpStatusCode>(statusCode));
    response->setContentTypeCode(CT_APPLICATION_JSON);
    response->setBody(payload.dump());
    return response;
}

HttpResponsePtr okResponse(const nlohmann::json& data, int statusCode = 200) {
    return jsonResponse(nlohmann::json{{"ok", true}, {"data", data}}, statusCode);
}

HttpResponsePtr errorResponse(int statusCode, const std::string& message) {
    return jsonResponse(nlohmann::json{{"ok", false}, {"error", message}}, statusCode);
}

bool parseId(const std::string& text, long long& id) {
    try {
        size_t consumed = 0;
        long long value = std::stoll(text, &consumed);
        if (consumed != text.size() || value <= 0) return false;
        id = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

long long parseLong(const std::string& text, long long fallback) {
    if (text.empty()) return fallback;
    try {
        return std::stoll(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string stringField(const nlohmann::json& object, const char* key,
                        const std::string& fallback = "") {
    if (!object.contains(key) || !object.at(key).is_string()) return fallback;
    return object.at(key).get<std::string>();
}

}  // namespace

void ApiController::health(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
    int64_t now = nowSeconds();
    callback(okResponse(nlohmann::json{
        {"status", "ok"},
        {"time", now},
        {"timeText", formatTime(now)},
    }));
}

void ApiController::stats(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
    callback(okResponse(AppContext::instance().crawl().stats()));
}

void ApiController::configInfo(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
    const Config& config = AppContext::instance().config();
    callback(okResponse(nlohmann::json{
        {"listen", config.listenAddr},
        {"port", config.port},
        {"intervalMinutes", config.crawler.intervalMinutes},
        {"platforms", nlohmann::json::array({"jd", "taobao", "tmall"})},
        {"notifierEnabled", config.notifier.enabled},
        {"notifierType", config.notifier.type},
        {"dropPercent", config.notifier.dropPercent},
        {"taobaoCookieConfigured", !config.crawler.taobao.cookie.empty()},
    }));
}

void ApiController::listProducts(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
    auto items = AppContext::instance().crawl().listProducts();
    callback(okResponse(nlohmann::json{
        {"items", items},
        {"total", items.size()},
    }));
}

void ApiController::addProduct(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(std::string(req->body()));
    } catch (const std::exception&) {
        callback(errorResponse(400, "请求体不是合法 JSON"));
        return;
    }
    if (!body.is_object()) {
        callback(errorResponse(400, "请求体必须是 JSON 对象"));
        return;
    }

    const std::string url = stringField(body, "url");
    const std::string note = stringField(body, "note");

    bool hasTarget = false;
    double targetPrice = 0.0;
    if (body.contains("targetPrice") && !body.at("targetPrice").is_null()) {
        if (!body.at("targetPrice").is_number()) {
            callback(errorResponse(400, "targetPrice 必须是数字或 null"));
            return;
        }
        hasTarget = true;
        targetPrice = body.at("targetPrice").get<double>();
    }

    std::string error;
    int code = 200;
    auto product = AppContext::instance().crawl().addProduct(url, hasTarget, targetPrice,
                                                             note, error, code);
    if (!error.empty()) {
        callback(errorResponse(code, error));
        return;
    }
    callback(okResponse(product, code));
}

void ApiController::getProduct(const HttpRequestPtr&,
                               std::function<void(const HttpResponsePtr&)>&& callback,
                               const std::string& id) {
    long long productId = 0;
    if (!parseId(id, productId)) {
        callback(errorResponse(400, "非法的商品 ID"));
        return;
    }
    std::string error;
    int code = 200;
    auto product = AppContext::instance().crawl().getProduct(productId, error, code);
    if (!error.empty()) {
        callback(errorResponse(code, error));
        return;
    }
    callback(okResponse(product));
}

void ApiController::updateProduct(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback,
                                  const std::string& id) {
    long long productId = 0;
    if (!parseId(id, productId)) {
        callback(errorResponse(400, "非法的商品 ID"));
        return;
    }

    nlohmann::json patch;
    try {
        patch = nlohmann::json::parse(std::string(req->body()));
    } catch (const std::exception&) {
        callback(errorResponse(400, "请求体不是合法 JSON"));
        return;
    }
    if (!patch.is_object()) {
        callback(errorResponse(400, "请求体必须是 JSON 对象"));
        return;
    }

    std::string error;
    int code = 200;
    auto product =
        AppContext::instance().crawl().updateProduct(productId, patch, error, code);
    if (!error.empty()) {
        callback(errorResponse(code, error));
        return;
    }
    callback(okResponse(product));
}

void ApiController::deleteProduct(const HttpRequestPtr&,
                                  std::function<void(const HttpResponsePtr&)>&& callback,
                                  const std::string& id) {
    long long productId = 0;
    if (!parseId(id, productId)) {
        callback(errorResponse(400, "非法的商品 ID"));
        return;
    }
    std::string error;
    int code = 200;
    auto result =
        AppContext::instance().crawl().deleteProduct(productId, error, code);
    if (!error.empty()) {
        callback(errorResponse(code, error));
        return;
    }
    callback(okResponse(result));
}

void ApiController::refreshProduct(const HttpRequestPtr&,
                                   std::function<void(const HttpResponsePtr&)>&& callback,
                                   const std::string& id) {
    long long productId = 0;
    if (!parseId(id, productId)) {
        callback(errorResponse(400, "非法的商品 ID"));
        return;
    }
    std::string error;
    int code = 200;
    auto product =
        AppContext::instance().crawl().refreshProduct(productId, error, code);
    if (!error.empty()) {
        callback(errorResponse(code, error));
        return;
    }
    callback(okResponse(product));
}

void ApiController::priceHistory(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 const std::string& id) {
    long long productId = 0;
    if (!parseId(id, productId)) {
        callback(errorResponse(400, "非法的商品 ID"));
        return;
    }

    long long from = parseLong(req->getParameter("from"), 0);
    long long to = parseLong(req->getParameter("to"), 0);
    long long days = parseLong(req->getParameter("days"), 0);
    if (days > 0) from = nowSeconds() - days * 86400;

    int limit = static_cast<int>(parseLong(req->getParameter("limit"), 2000));
    if (limit <= 0) limit = 2000;
    if (limit > 5000) limit = 5000;

    std::string error;
    int code = 200;
    auto history = AppContext::instance().crawl().priceHistory(productId, from, to,
                                                               limit, error, code);
    if (!error.empty()) {
        callback(errorResponse(code, error));
        return;
    }
    callback(okResponse(history));
}

}  // namespace pm
