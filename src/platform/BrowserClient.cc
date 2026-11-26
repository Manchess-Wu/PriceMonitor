#include "platform/BrowserClient.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HttpClient.h"
#include "core/Logger.h"
#include "core/Util.h"

namespace pm {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::string base64(const unsigned char* data, size_t length) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((length + 2) / 3 * 4);
    for (size_t i = 0; i < length; i += 3) {
        unsigned int chunk = data[i] << 16;
        if (i + 1 < length) chunk |= data[i + 1] << 8;
        if (i + 2 < length) chunk |= data[i + 2];
        out.push_back(table[(chunk >> 18) & 0x3F]);
        out.push_back(table[(chunk >> 12) & 0x3F]);
        out.push_back(i + 1 < length ? table[(chunk >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < length ? table[chunk & 0x3F] : '=');
    }
    return out;
}

// 最小的 WebSocket 客户端，仅用于连接本机 CDP 调试端口。
class WebSocketClient {
public:
    ~WebSocketClient() { close(); }

    bool connect(const std::string& host, int port, const std::string& path,
                 std::string& err) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* info = nullptr;
        const std::string service = std::to_string(port);
        if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &info) != 0 || !info) {
            err = "无法解析浏览器调试地址";
            return false;
        }
        int fd = -1;
        for (addrinfo* it = info; it != nullptr; it = it->ai_next) {
            fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(info);
        if (fd < 0) {
            err = "无法连接浏览器调试端口";
            return false;
        }
        fd_ = fd;

        unsigned char key[16];
        std::random_device rd;
        for (auto& byte : key) byte = static_cast<unsigned char>(rd());
        const std::string request =
            "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + service +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
            base64(key, sizeof(key)) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!writeAll(request.data(), request.size())) {
            err = "调试通道握手发送失败";
            return false;
        }

        std::string head;
        char ch = 0;
        while (head.find("\r\n\r\n") == std::string::npos) {
            if (::recv(fd_, &ch, 1, 0) <= 0) {
                err = "浏览器调试通道握手失败";
                return false;
            }
            head.push_back(ch);
            if (head.size() > 8192) {
                err = "浏览器调试通道响应异常";
                return false;
            }
        }
        if (head.find(" 101 ") == std::string::npos) {
            err = "浏览器调试通道未升级为 WebSocket";
            return false;
        }
        return true;
    }

    bool sendText(const std::string& text, std::string& err) {
        std::string frame;
        frame.push_back(static_cast<char>(0x81));
        const size_t length = text.size();
        if (length < 126) {
            frame.push_back(static_cast<char>(0x80 | length));
        } else if (length <= 0xFFFF) {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((length >> 8) & 0xFF));
            frame.push_back(static_cast<char>(length & 0xFF));
        } else {
            frame.push_back(static_cast<char>(0x80 | 127));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>((static_cast<uint64_t>(length) >> shift) & 0xFF));
            }
        }
        unsigned char mask[4];
        std::random_device rd;
        for (auto& byte : mask) byte = static_cast<unsigned char>(rd());
        frame.append(reinterpret_cast<char*>(mask), 4);
        for (size_t i = 0; i < length; ++i) {
            frame.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
        }
        if (!writeAll(frame.data(), frame.size())) {
            err = "发送调试指令失败";
            return false;
        }
        return true;
    }

    bool recvText(std::string& out, std::string& err) {
        std::string payload;
        while (true) {
            unsigned char header[2];
            if (!readAll(header, 2)) {
                err = "读取浏览器调试响应失败";
                return false;
            }
            const bool fin = (header[0] & 0x80) != 0;
            const int opcode = header[0] & 0x0F;
            const bool masked = (header[1] & 0x80) != 0;
            uint64_t length = header[1] & 0x7F;
            if (length == 126) {
                unsigned char buffer[2];
                if (!readAll(buffer, 2)) {
                    err = "读取浏览器调试响应失败";
                    return false;
                }
                length = (static_cast<uint64_t>(buffer[0]) << 8) | buffer[1];
            } else if (length == 127) {
                unsigned char buffer[8];
                if (!readAll(buffer, 8)) {
                    err = "读取浏览器调试响应失败";
                    return false;
                }
                length = 0;
                for (unsigned char byte : buffer) length = (length << 8) | byte;
            }
            unsigned char mask[4] = {0, 0, 0, 0};
            if (masked && !readAll(mask, 4)) {
                err = "读取浏览器调试响应失败";
                return false;
            }
            std::string data;
            data.resize(static_cast<size_t>(length));
            if (length > 0 && !readAll(&data[0], static_cast<size_t>(length))) {
                err = "读取浏览器调试响应失败";
                return false;
            }
            if (masked) {
                for (size_t i = 0; i < data.size(); ++i) data[i] ^= mask[i % 4];
            }
            if (opcode == 0x9) {
                std::string ignored;
                sendPong(data, ignored);
                continue;
            }
            if (opcode == 0x8) {
                err = "浏览器调试通道已关闭";
                return false;
            }
            payload += data;
            if (fin) {
                out = std::move(payload);
                return true;
            }
        }
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    bool writeAll(const void* data, size_t length) {
        const char* cursor = static_cast<const char*>(data);
        while (length > 0) {
            const ssize_t written = ::send(fd_, cursor, length, MSG_NOSIGNAL);
            if (written <= 0) return false;
            cursor += written;
            length -= static_cast<size_t>(written);
        }
        return true;
    }

    bool readAll(void* data, size_t length) {
        char* cursor = static_cast<char*>(data);
        while (length > 0) {
            const ssize_t got = ::recv(fd_, cursor, length, 0);
            if (got <= 0) return false;
            cursor += got;
            length -= static_cast<size_t>(got);
        }
        return true;
    }

    bool sendPong(const std::string& payload, std::string& err) {
        std::string frame;
        frame.push_back(static_cast<char>(0x8A));
        frame.push_back(static_cast<char>(0x80 | (payload.size() & 0x7F)));
        unsigned char mask[4] = {1, 2, 3, 4};
        frame.append(reinterpret_cast<char*>(mask), 4);
        for (size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        }
        if (!writeAll(frame.data(), frame.size())) {
            err = "回复心跳失败";
            return false;
        }
        return true;
    }

    int fd_ = -1;
};

bool cdpCall(WebSocketClient& ws, int id, const std::string& method,
             const json& params, const std::string& sessionId, json& result,
             std::string& err) {
    json message{{"id", id}, {"method", method}, {"params", params}};
    if (!sessionId.empty()) message["sessionId"] = sessionId;
    if (!ws.sendText(message.dump(), err)) return false;
    while (true) {
        std::string text;
        if (!ws.recvText(text, err)) return false;
        json object = json::parse(text, nullptr, false);
        if (object.is_discarded() || !object.contains("id") || object["id"] != id) {
            continue;
        }
        if (object.contains("error")) {
            err = object["error"].value("message", std::string("浏览器返回错误"));
            return false;
        }
        result = object.contains("result") ? object["result"] : json::object();
        return true;
    }
}

int reservePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    const int port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

// 在 PATH 中查找可执行文件，找不到返回空字符串
std::string findExecutable(const std::string& name) {
    if (name.empty()) return "";
    if (name.find('/') != std::string::npos) {
        return ::access(name.c_str(), X_OK) == 0 ? name : "";
    }
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) return "";
    std::string paths(pathEnv);
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t end = paths.find(':', start);
        const std::string dir =
            paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::string full = dir + "/" + name;
            if (::access(full.c_str(), X_OK) == 0) return full;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

// 配置里的浏览器找不到时，自动尝试常见名称
std::string resolveChromePath(const std::string& configured) {
    if (!findExecutable(configured).empty()) return configured;
    for (const char* candidate :
         {"chromium", "chromium-browser", "google-chrome", "google-chrome-stable"}) {
        if (!findExecutable(candidate).empty()) return candidate;
    }
    return configured;
}

// 清理上次异常退出遗留的 profile 锁：锁里记录的进程已不存在时直接删除，
// 否则 Chrome 会误以为配置目录被占用而立即退出，表现为“浏览器启动超时”。
void cleanupStaleProfileLock(const std::string& profileDir) {
    const std::string lock = profileDir + "/SingletonLock";
    char buf[256] = {0};
    const ssize_t n = ::readlink(lock.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return;
    const std::string target(buf, static_cast<size_t>(n));
    const size_t dash = target.rfind('-');
    if (dash == std::string::npos) return;
    const std::string pidText = target.substr(dash + 1);
    if (pidText.empty()) return;
    char* end = nullptr;
    const long pid = std::strtol(pidText.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || pid <= 0) return;
    if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) return;
    ::unlink((profileDir + "/SingletonLock").c_str());
    ::unlink((profileDir + "/SingletonCookie").c_str());
    ::unlink((profileDir + "/SingletonSocket").c_str());
    logWarn("已清理失效的浏览器配置目录锁");
}

pid_t spawnBrowser(const BrowserClient::Options& options, const std::string& url,
                   int port, bool headless, std::string& err,
                   bool loadImages = false) {
    std::vector<std::string> args{options.chromePath};
    cleanupStaleProfileLock(options.profileDir);
    if (headless) {
        args.push_back("--headless=new");
        args.push_back("--mute-audio");
        args.push_back("--hide-scrollbars");
    }
    args.push_back("--no-sandbox");
    args.push_back("--disable-gpu");
    args.push_back("--disable-dev-shm-usage");
    args.push_back("--no-first-run");
    args.push_back("--no-default-browser-check");
    // 采集只需要 DOM 文本：不加载图片/字体，显著缩短详情页加载时间。
    // 登录窗口必须加载图片（二维码/验证码），由调用方显式开启。
    if (!loadImages) args.push_back("--blink-settings=imagesEnabled=false");
    args.push_back("--disable-background-networking");
    // 多标签并发时，后台标签不被降频，否则第二个 tab 会明显慢于第一个
    args.push_back("--disable-background-timer-throttling");
    args.push_back("--disable-backgrounding-occluded-windows");
    args.push_back("--disable-renderer-backgrounding");
    args.push_back("--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling");
    // 同站点（如两个 detail.tmall.com）的标签默认会共用一个渲染进程，
    // 解析/渲染会互相排队；尽量让每个标签独立进程（Chrome 不支持时自动忽略）
    args.push_back("--process-per-tab");
    args.push_back("--disable-blink-features=AutomationControlled");
    args.push_back("--remote-debugging-port=" + std::to_string(port));
    args.push_back("--remote-allow-origins=*");
    args.push_back("--user-data-dir=" + options.profileDir);
    if (!options.userAgent.empty()) args.push_back("--user-agent=" + options.userAgent);
    args.push_back(url);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        err = "无法创建浏览器进程";
        return -1;
    }
    if (pid == 0) {
        ::setsid();
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
        }
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    return pid;
}

void shutdownBrowser(pid_t pid) {
    if (pid <= 0) return;
    ::kill(-pid, SIGTERM);
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid || done < 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

bool waitForDevtools(int port, int timeoutSeconds, pid_t pid, json& version,
                     std::string& err) {
    const auto deadline = Clock::now() + std::chrono::seconds(std::max(5, timeoutSeconds));
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/json/version";
    while (Clock::now() < deadline) {
        // 浏览器进程已退出（profile 被其他实例占用、可执行文件不可用等）时
        // 立即报错，避免白等一个超时周期后给出误导性的“启动超时”。
        int status = 0;
        if (pid > 0 && ::waitpid(pid, &status, WNOHANG) == pid) {
            err = "浏览器启动后立即退出，配置目录可能被其他 Chrome 实例占用";
            return false;
        }
        HttpResult result = HttpClient::get(url, {}, "", 2);
        if (result.ok()) {
            json parsed = json::parse(result.body, nullptr, false);
            if (!parsed.is_discarded() && parsed.contains("webSocketDebuggerUrl")) {
                version = parsed;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    err = "浏览器启动超时，请确认 Chrome 路径正确且配置目录未被其他进程占用";
    return false;
}

// 进程内共享一个 Chrome 实例：同一 profile 只允许一个主实例，
// 并发采集通过多个标签页（Target）复用该实例，而不是各自拉起 Chrome。
struct SharedBrowser {
    std::mutex mutex;
    pid_t pid = -1;
    int port = 0;
    int refs = 0;
    uint64_t generation = 0;
};

SharedBrowser& sharedBrowser() {
    static SharedBrowser browser;
    return browser;
}

// 获取共享浏览器（必要时启动），返回调试端口与实例代号。
bool acquireBrowser(const BrowserClient::Options& options, int& port,
                    uint64_t& generation, json& version, std::string& err) {
    SharedBrowser& shared = sharedBrowser();
    std::lock_guard<std::mutex> lock(shared.mutex);

    if (shared.pid > 0 && ::kill(shared.pid, 0) == 0 &&
        waitForDevtools(shared.port, 5, 0, version, err)) {
        ++shared.refs;
        port = shared.port;
        generation = shared.generation;
        return true;
    }

    // 已有实例不可用（退出或调试端口无响应），先清理再重建
    if (shared.pid > 0) {
        shutdownBrowser(shared.pid);
        shared.pid = -1;
        shared.port = 0;
        shared.refs = 0;
    }

    const int newPort = reservePort();
    if (newPort <= 0) {
        err = "无法分配本地调试端口";
        return false;
    }
    const pid_t pid = spawnBrowser(options, "about:blank", newPort,
                                   options.headless, err);
    if (pid < 0) return false;
    if (!waitForDevtools(newPort, options.timeoutSeconds, pid, version, err)) {
        shutdownBrowser(pid);
        return false;
    }
    shared.pid = pid;
    shared.port = newPort;
    shared.refs = 1;
    ++shared.generation;
    port = newPort;
    generation = shared.generation;
    return true;
}

// 释放共享浏览器；最后一个使用者离开时关闭 Chrome。
void releaseBrowser(uint64_t generation) {
    SharedBrowser& shared = sharedBrowser();
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (generation != shared.generation) return;  // 已被新一轮实例取代
    if (shared.refs > 0) --shared.refs;
    if (shared.refs == 0 && shared.pid > 0) {
        shutdownBrowser(shared.pid);
        shared.pid = -1;
        shared.port = 0;
    }
}

}  // namespace

BrowserClient::Options BrowserClient::fromConfig(const CrawlerConfig& config) {
    Options options;
    options.enabled = config.browser.enabled;
    options.headless = config.browser.headless;
    options.chromePath = config.browser.chromePath;
    // 允许用环境变量覆盖，便于安装脚本在不改配置的情况下指定浏览器
    if (const char* env = std::getenv("PM_CHROME_PATH"); env != nullptr && *env != '\0') {
        options.chromePath = env;
    }
    options.chromePath = resolveChromePath(options.chromePath);
    options.profileDir = config.browser.profileDir;
    options.userAgent = config.browser.userAgent.empty() ? config.userAgent
                                                         : config.browser.userAgent;
    options.timeoutSeconds = config.browser.timeoutSeconds;
    return options;
}

bool BrowserClient::fetchHtml(const std::string& url, const std::string& waitFor,
                              std::string& html, std::string& err,
                              const std::string& extractExpr,
                              std::string* extractResult) {
    html.clear();
    int port = 0;
    uint64_t generation = 0;
    json version;
    if (!acquireBrowser(options_, port, generation, version, err)) return false;

    bool ok = false;
    WebSocketClient ws;
    do {
        const std::string wsUrl = version.value("webSocketDebuggerUrl", "");
        if (!startsWith(wsUrl, "ws://")) {
            err = "浏览器未提供可用的调试地址";
            break;
        }
        const std::string rest = wsUrl.substr(5);
        const size_t slash = rest.find('/');
        const std::string hostPort = rest.substr(0, slash);
        const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
        const size_t colon = hostPort.rfind(':');
        if (colon == std::string::npos) {
            err = "浏览器调试地址格式异常";
            break;
        }
        const std::string host = hostPort.substr(0, colon);
        int wsPort = 0;
        try {
            wsPort = std::stoi(hostPort.substr(colon + 1));
        } catch (const std::exception&) {
            err = "浏览器调试端口解析失败";
            break;
        }
        if (!ws.connect(host, wsPort, path, err)) break;

        json result;
        if (!cdpCall(ws, 1, "Target.createTarget", {{"url", "about:blank"}}, "",
                     result, err)) {
            break;
        }
        const std::string targetId = result.value("targetId", "");
        if (targetId.empty()) {
            err = "无法创建浏览器标签页";
            break;
        }
        if (!cdpCall(ws, 2, "Target.attachToTarget",
                     {{"targetId", targetId}, {"flatten", true}}, "", result, err)) {
            break;
        }
        const std::string sessionId = result.value("sessionId", "");
        if (!cdpCall(ws, 3, "Page.enable", json::object(), sessionId, result, err)) break;
        if (!cdpCall(ws, 4, "Page.navigate", {{"url", url}}, sessionId, result, err)) break;

        int nextId = 5;
        const auto deadline = Clock::now() + std::chrono::seconds(options_.timeoutSeconds);
        std::string escaped;
        for (char c : waitFor) {
            if (c == '\'' || c == '\\') escaped.push_back('\\');
            escaped.push_back(c);
        }
        const std::string stateExpr =
            "(() => { const h = document.documentElement ? "
            "document.documentElement.outerHTML : ''; return JSON.stringify({"
            "u: location.href, t: document.title, "
            "p: h.indexOf('" + escaped + "') >= 0, "
            "r: document.readyState, n: h.length, "
            "l: /login\\.taobao\\.com|havanaone/.test(location.href)}); })()";
        long long lastLength = -1;
        int stableRounds = 0;
        bool markerSeen = false;
        bool completeSeen = false;
        auto completeAt = Clock::now();
        const auto markerDeadline = deadline;
        while (Clock::now() < markerDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            json state;
            if (!cdpCall(ws, nextId++, "Runtime.evaluate",
                         {{"expression", stateExpr}, {"returnByValue", true}},
                         sessionId, state, err)) {
                break;
            }
            const std::string value = state["result"].value("value", "");
            json parsed = json::parse(value, nullptr, false);
            if (parsed.is_discarded()) continue;
            if (parsed.value("p", false)) markerSeen = true;
            if (parsed.value("l", false) || parsed.value("t", "") == "登录") break;
            // 页面已加载完成且内容长度不再变化时提前结束，避免等满整个超时；
            // 标记出现后还要给页面脚本留出渲染券后价的时间。
            const long long length = parsed.value("n", 0LL);
            const bool complete = parsed.value("r", "") == "complete";
            if (!completeSeen && complete) {
                completeSeen = true;
                completeAt = Clock::now();
            }
            const bool settled =
                markerSeen
                    ? (completeSeen &&
                       Clock::now() - completeAt >= std::chrono::milliseconds(1000))
                    : completeSeen;
            if (settled && length > 0 && length == lastLength) {
                if (++stableRounds >= 2) break;
            } else {
                stableRounds = 0;
            }
            // 页面一直在变动（广告/倒计时）时不要等满整个超时
            if (completeSeen && Clock::now() - completeAt >= std::chrono::seconds(5)) break;
            lastLength = length;
        }
        if (!err.empty()) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        json dump;
        if (!cdpCall(ws, nextId, "Runtime.evaluate",
                     {{"expression", "document.documentElement.outerHTML"},
                      {"returnByValue", true}},
                     sessionId, dump, err)) {
            break;
        }
        html = dump["result"].value("value", "");
        ok = !html.empty();
        if (!ok) err = "浏览器未返回页面内容";

        // 在已渲染的页面上再执行一段取价脚本（券后价这类信息只在可见文本里）。
        // 券后价有时由页面脚本稍后写入，取不到时短暂重试几次。
        if (!extractExpr.empty() && extractResult != nullptr) {
            extractResult->clear();
            for (int attempt = 0; attempt < 3 && extractResult->empty(); ++attempt) {
                if (attempt > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                std::string evalErr;
                json valueResult;
                if (cdpCall(ws, nextId++, "Runtime.evaluate",
                            {{"expression", extractExpr}, {"returnByValue", true}},
                            sessionId, valueResult, evalErr)) {
                    if (valueResult.contains("exceptionDetails")) {
                        logWarn("页面取价脚本异常: " +
                                valueResult["exceptionDetails"].value("text", ""));
                    }
                    *extractResult = valueResult["result"].value("value", "");
                } else if (attempt == 2) {
                    logWarn("页面取价脚本执行失败: " + evalErr);
                }
            }
        }

        // 共享浏览器不随单次采集关闭，必须主动关掉本任务的标签页
        if (!targetId.empty()) {
            std::string closeErr;
            json ignored;
            cdpCall(ws, nextId++, "Target.closeTarget", {{"targetId", targetId}},
                    "", ignored, closeErr);
        }
    } while (false);

    ws.close();
    // 只释放引用：实例是否健康由下一次 acquire 的端口探测决定，
    // 不能因为单个页面失败就关掉其他并发任务正在用的浏览器
    releaseBrowser(generation);
    return ok;
}

bool BrowserClient::openForLogin(const Options& options, const std::string& url,
                                 std::string& err) {
    const int port = reservePort();
    const pid_t pid =
        spawnBrowser(options, url, port > 0 ? port : 0, false, err, true);
    if (pid < 0) return false;
    logInfo("已启动浏览器，配置目录: " + options.profileDir);
    logInfo("登录态只会写入上面这个目录；如服务已安装，请确认与服务读取的目录一致"
            "（推荐用 sudo bash deploy/login.sh 登录）");
    logInfo("请在浏览器窗口中完成登录，登录成功后直接关闭窗口即可");
    const auto started = std::chrono::steady_clock::now();
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        break;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (elapsed < 3) {
        err = "浏览器启动后立即退出，请确认有图形界面（DISPLAY）且 Chrome 可正常运行";
        return false;
    }
    return true;
}

const char* BrowserClient::couponPriceScript() {
    // 只读取页面可见文本里的券后价，不点击任何“领券”按钮，避免账号副作用和风控。
    // 天猫/淘宝新版详情页大量内容在 shadow DOM 里，需要递归进入 shadow root 取文本；
    // 取不到价格时返回 # 开头的上下文片段，便于定位页面的真实文案。
    return R"JS((() => {
  const roots = [];
  (function walk(root) {
    let list;
    try { list = root.querySelectorAll('*'); } catch (e) { return; }
    for (let i = 0; i < list.length; i++) {
      const el = list[i];
      if (el.shadowRoot) {
        roots.push(el.shadowRoot);
        walk(el.shadowRoot);
      }
    }
  })(document);

  // 天猫详情内容有时在页面内的 iframe 里，跨域会抛异常，忽略即可
  const frames = [];
  try {
    const fs = document.querySelectorAll('iframe');
    for (let i = 0; i < fs.length; i++) {
      try {
        const d = fs[i].contentDocument;
        if (d && d.body) frames.push(d);
      } catch (e) {}
    }
  } catch (e) {}

  let text = document.body ? (document.body.innerText || "") : "";
  for (let i = 0; i < roots.length; i++) {
    const t = roots[i].textContent || "";
    if (t) text += " " + t.slice(0, 30000);
  }
  for (let i = 0; i < frames.length; i++) {
    const t = frames[i].body.innerText || "";
    if (t) text += " " + t.slice(0, 30000);
  }
  text = text.replace(/\s+/g, " ");

  const kw = "(?:预估到手价|预计到手价|预估券后价|券后价|券后|领券后价|领券后|用券后|到手价|满减后价|满减后|折后价|优惠后)";
  const re = new RegExp(kw + "\\s*[:：]?\\s*[¥￥]?\\s*([0-9][0-9,]*(?:\\.[0-9]{1,2})?)", "g");
  const reRev = new RegExp("[¥￥]\\s*([0-9][0-9,]*(?:\\.[0-9]{1,2})?)\\s*" + kw, "g");
  const out = [];
  let m;
  while ((m = re.exec(text)) && out.length < 8) {
    if (/省|立减|优惠|满[0-9]/.test(m[0])) continue;
    out.push(m[1]);
  }
  if (out.length === 0) {
    while ((m = reRev.exec(text)) && out.length < 8) out.push(m[1]);
  }
  if (out.length === 0) {
    const ctx = /券|到手|折后|优惠|满减/g;
    let d;
    let n = 0;
    while ((d = ctx.exec(text)) && n < 3) {
      out.push('#' + text.slice(Math.max(0, d.index - 30), d.index + 50).replace(/\|/g, '/'));
      n++;
    }
    // 价格/优惠相关的 DOM 节点（含 shadow DOM），直接看页面结构
    const seen = {};
    let k = 0;
    const scan = (root) => {
      let nodes = [];
      try {
        nodes = root.querySelectorAll(
          '[class*="price"],[class*="Price"],[id*="price"],[id*="Price"],' +
          '[class*="coupon"],[class*="Coupon"],[class*="promo"],[class*="Promo"],' +
          '[class*="activity"],[class*="Activity"]');
      } catch (e) {}
      for (let i = 0; i < nodes.length && k < 10; i++) {
        const el = nodes[i];
        const t = (el.innerText || el.textContent || "").toString()
          .replace(/\s+/g, " ").trim();
        if (!t || t.length > 120 || seen[t]) continue;
        seen[t] = 1;
        out.push('#' + el.tagName + '.' + String(el.className || "").slice(0, 30) +
                 ' => ' + t.replace(/\|/g, '/'));
        k++;
      }
    };
    scan(document);
    for (let i = 0; i < roots.length && k < 10; i++) scan(roots[i]);
    for (let i = 0; i < frames.length && k < 10; i++) scan(frames[i]);
  }
  if (out.length === 0) {
    out.push('#meta => url=' + location.href + ' title=' + document.title +
             ' textLen=' + text.length + ' shadow=' + roots.length +
             ' frames=' + frames.length +
             ' bodyLen=' + (document.body ? document.body.innerHTML.length : 0) +
             ' text0=' + text.slice(0, 300).replace(/\|/g, '/'));
  }
  return out.join("|");
})())JS";
}

std::string BrowserClient::couponDiagnostics(const std::string& candidates) {
    std::string result;
    size_t start = 0;
    while (start < candidates.size()) {
        const size_t end = candidates.find('|', start);
        const std::string item = candidates.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty() && item[0] == '#') {
            if (!result.empty()) result += " || ";
            result += item.substr(1);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

std::optional<double> BrowserClient::firstCouponPrice(const std::string& candidates) {
    size_t start = 0;
    while (start < candidates.size()) {
        const size_t end = candidates.find('|', start);
        std::string item = candidates.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::string cleaned;
        cleaned.reserve(item.size());
        for (char ch : item) {
            if (ch == '#') return std::nullopt;  // 诊断片段不是价格
            if (ch != ',') cleaned.push_back(ch);
        }
        try {
            const double value = std::stod(cleaned);
            if (value > 0) return value;
        } catch (const std::exception&) {
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

}  // namespace pm
