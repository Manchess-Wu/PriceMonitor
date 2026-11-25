#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pm {

int64_t nowSeconds();

// epoch 秒 -> 本地时间字符串
std::string formatTime(int64_t epoch, const char* fmt = "%Y-%m-%d %H:%M:%S");

std::string trim(const std::string& s);
std::string toLower(std::string s);
bool startsWith(const std::string& s, const std::string& prefix);
bool contains(const std::string& s, const std::string& needle);

std::string fmtPrice(double value);
double round2(double value);

// 返回第一个匹配分组，失败返回 nullopt
std::optional<std::string> regexGroup(const std::string& text,
                                      const std::string& pattern,
                                      int group = 1);

std::string htmlUnescape(const std::string& text);
std::string normalizeSpace(const std::string& text);
std::string truncate(const std::string& text, size_t maxLen);
std::string urlEncode(const std::string& text);

}  // namespace pm
