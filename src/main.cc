#include <drogon/drogon.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif

#include "core/AppContext.h"
#include "core/Logger.h"
#include "core/Util.h"
#include "platform/BrowserClient.h"

namespace {

namespace fs = std::filesystem;

std::string executableDir() {
#ifdef __linux__
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0) {
        buffer[length] = '\0';
        return fs::path(buffer).parent_path().string();
    }
#endif
    return fs::current_path().string();
}

// 依次在可执行文件目录、CWD、可执行文件上级目录中寻找部署根目录。
// 必须先看可执行文件所在目录：如果先看 CWD，在源码目录里执行安装后的
// 二进制（例如在源码目录跑 sudo bash deploy/login.sh），会把 ./data 解析
// 到源码目录，登录态写进源码树的 profile，服务读的却是部署目录的 profile，
// 表现为“刚登录就提示登录已失效”。
std::string pickBaseDir(const std::string& exeDir) {
    std::vector<fs::path> candidates{fs::path(exeDir), fs::current_path()};
    const fs::path parent = fs::path(exeDir).parent_path();
    if (!parent.empty()) candidates.push_back(parent);

    for (const auto& dir : candidates) {
        if (fs::exists(dir / "web" / "index.html")) return dir.string();
    }
    for (const auto& dir : candidates) {
        if (fs::exists(dir / "config")) return dir.string();
    }
    return fs::current_path().string();
}

using drogon::HttpRequestPtr;
using drogon::HttpResponsePtr;

// 返回 std::function 临时对象：Drogon 的 HttpBinder 会把左值 lambda 推导成引用类型，
// 函数返回后捕获内容悬空，因此这里必须传右值。
std::function<void(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&&)>
makeStaticHandler(const std::string& indexFile) {
    return [indexFile](const HttpRequestPtr&,
                       std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!std::filesystem::exists(indexFile)) {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k500InternalServerError);
            response->setBody("未找到 web/index.html，请确认部署目录完整");
            callback(response);
            return;
        }
        callback(drogon::HttpResponse::newFileResponse(indexFile));
    };
}

void registerStaticPage(const std::string& indexFile) {
    drogon::app().registerHandler("/", makeStaticHandler(indexFile), {drogon::Get});
    drogon::app().registerHandler("/index.html", makeStaticHandler(indexFile), {drogon::Get});
}

}  // namespace

int main(int argc, char** argv) {
    const std::string baseDir = pickBaseDir(executableDir());
    const std::string configPath =
        argc > 1 && std::string(argv[1]) != "--login"
            ? std::string(argv[1])
            : (fs::path(baseDir) / "config" / "config.json").string();

    // 淘宝/京东需要真实浏览器执行页面脚本，登录一次后把登录态保存在配置目录中。
    if (argc > 1 && std::string(argv[1]) == "--login") {
        std::string site = "taobao";
        std::string path;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "jd" || arg == "taobao" || arg == "tmall") {
                site = arg;
            } else {
                path = arg;
            }
        }
        if (path.empty()) path = (fs::path(baseDir) / "config" / "config.json").string();
        pm::Config config;
        std::string error;
        if (!pm::Config::load(path, config, error)) {
            std::cerr << "[ERROR] 读取配置失败: " << error << std::endl;
            return 1;
        }
        pm::BrowserClient::Options options =
            pm::BrowserClient::fromConfig(config.crawler);
        const fs::path profile(options.profileDir);
        if (!profile.is_absolute()) {
            options.profileDir = (fs::path(baseDir) / profile).string();
        }
        // 天猫和淘宝的登录 Cookie 分属 .tmall.com / .taobao.com 两个域，
        // 在淘宝登录不等于天猫也登录，天猫商品需要单独登录一次。
        const std::string url =
            site == "jd" ? "https://passport.jd.com/new/login.aspx"
                         : (site == "tmall" ? "https://www.tmall.com"
                                            : "https://www.taobao.com");
        std::cout << "登录站点: " << site << std::endl;
        if (!pm::BrowserClient::openForLogin(options, url, error)) {
            std::cerr << "[ERROR] 启动浏览器失败: " << error << std::endl;
            return 1;
        }
        return 0;
    }

    pm::AppContext& context = pm::AppContext::instance();
    std::string error;
    if (!context.init(configPath, baseDir, error)) {
        std::cerr << "[ERROR] 启动失败: " << error << std::endl;
        return 1;
    }

    const pm::Config& config = context.config();

    drogon::app().setThreadNum(config.threads);
    drogon::app().addListener(config.listenAddr, config.port);

    registerStaticPage(context.documentRoot() + "/index.html");
    drogon::app().registerHandler(
        "/favicon.ico",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k204NoContent);
            callback(response);
        },
        {drogon::Get});

    context.startScheduler();

    pm::logInfo("Web 管理界面: http://" +
                (config.listenAddr == "0.0.0.0" ? std::string("127.0.0.1")
                                                : config.listenAddr) +
                ":" + std::to_string(config.port));

    drogon::app().run();

    context.stopScheduler();
    pm::logInfo("PriceMonitor 已退出");
    return 0;
}
