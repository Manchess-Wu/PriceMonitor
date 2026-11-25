#include "core/Logger.h"

#include <ctime>
#include <iostream>
#include <mutex>

namespace pm {
namespace {

std::mutex g_logMutex;

void write(const char* level, const std::string& msg) {
    std::time_t now = std::time(nullptr);
    std::tm tmBuf{};
    localtime_r(&now, &tmBuf);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmBuf);

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ostream& os = std::string(level) == "ERROR" ? std::cerr : std::cout;
    os << "[" << stamp << "] [" << level << "] " << msg << std::endl;
}

}  // namespace

void logInfo(const std::string& msg) { write("INFO", msg); }
void logWarn(const std::string& msg) { write("WARN", msg); }
void logError(const std::string& msg) { write("ERROR", msg); }

}  // namespace pm
