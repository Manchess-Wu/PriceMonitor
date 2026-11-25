#pragma once

#include <string>

namespace pm {

// 极简线程安全日志，避免依赖 Drogon 的日志宏，便于在各模块中使用。
void logInfo(const std::string& msg);
void logWarn(const std::string& msg);
void logError(const std::string& msg);

}  // namespace pm
