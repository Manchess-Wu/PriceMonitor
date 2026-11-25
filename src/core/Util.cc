#include "core/Util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <regex>

namespace pm {

int64_t nowSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::string formatTime(int64_t epoch, const char* fmt) {
    if (epoch <= 0) return "";
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmBuf{};
    localtime_r(&t, &tmBuf);
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tmBuf);
    return buf;
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    size_t begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

std::string fmtPrice(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

double round2(double value) {
    return std::round(value * 100.0) / 100.0;
}

std::optional<std::string> regexGroup(const std::string& text,
                                      const std::string& pattern,
                                      int group) {
    try {
        std::regex re(pattern, std::regex::ECMAScript | std::regex::optimize);
        std::smatch m;
        if (std::regex_search(text, m, re) && group >= 0 &&
            group < static_cast<int>(m.size()) && m[group].matched) {
            return m[group].str();
        }
    } catch (const std::regex_error&) {
        // 非法正则时静默返回，调用方会走降级逻辑
    }
    return std::nullopt;
}

std::string htmlUnescape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        struct Entity {
            const char* name;
            char value;
        };
        static const Entity kEntities[] = {
            {"&amp;", '&'}, {"&lt;", '<'},  {"&gt;", '>'},
            {"&quot;", '"'}, {"&#39;", '\''}, {"&#x27;", '\''},
        };
        bool replaced = false;
        for (const auto& e : kEntities) {
            size_t len = std::strlen(e.name);
            if (text.compare(i, len, e.name) == 0) {
                out.push_back(e.value);
                i += len - 1;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.push_back('&');
    }
    return out;
}

std::string normalizeSpace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool lastSpace = true;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            if (!lastSpace) out.push_back(' ');
            lastSpace = true;
        } else {
            out.push_back(static_cast<char>(c));
            lastSpace = false;
        }
    }
    return trim(out);
}

std::string truncate(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen) return text;
    return text.substr(0, maxLen);
}

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace pm
