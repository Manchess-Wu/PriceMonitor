#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

namespace pm {

class ApiController : public HttpController<ApiController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ApiController::health, "/api/health", Get);
    ADD_METHOD_TO(ApiController::stats, "/api/stats", Get);
    ADD_METHOD_TO(ApiController::configInfo, "/api/config", Get);
    ADD_METHOD_TO(ApiController::listProducts, "/api/products", Get);
    ADD_METHOD_TO(ApiController::addProduct, "/api/products", Post);
    ADD_METHOD_TO(ApiController::getProduct, "/api/products/{1}", Get);
    ADD_METHOD_TO(ApiController::updateProduct, "/api/products/{1}", Put);
    ADD_METHOD_TO(ApiController::deleteProduct, "/api/products/{1}", Delete);
    ADD_METHOD_TO(ApiController::refreshProduct, "/api/products/{1}/refresh", Post);
    ADD_METHOD_TO(ApiController::priceHistory, "/api/products/{1}/prices", Get);
    METHOD_LIST_END

    void health(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback);
    void stats(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback);
    void configInfo(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

    void listProducts(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);
    void addProduct(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
    void getProduct(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id);
    void updateProduct(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id);
    void deleteProduct(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id);
    void refreshProduct(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& id);
    void priceHistory(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id);
};

}  // namespace pm
