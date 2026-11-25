#pragma once

#include <string>

// 前向声明必须位于全局作用域，否则会被声明成 pm::lxb_html_document，
// 与 Lexbor 的 ::lxb_html_document 不是同一类型。
struct lxb_html_document;

namespace pm {

// 基于 Lexbor 的轻量 HTML 查询封装（按 id / class 查找并提取文本或属性）。
// 仅用于抓取标题、店铺、图片等元信息，价格解析仍保留正则降级路径。
class HtmlDocument {
public:
    explicit HtmlDocument(const std::string& html);
    ~HtmlDocument();

    HtmlDocument(const HtmlDocument&) = delete;
    HtmlDocument& operator=(const HtmlDocument&) = delete;

    bool valid() const { return doc_ != nullptr; }

    std::string title() const;
    std::string textById(const std::string& id) const;
    std::string textByClass(const std::string& className) const;
    std::string attrById(const std::string& id, const std::string& attr) const;
    std::string attrByClass(const std::string& className,
                            const std::string& attr) const;
    // 读取 <meta name/property="key"> 的 content
    std::string metaContent(const std::string& key) const;

private:
    lxb_html_document* doc_ = nullptr;
};

}  // namespace pm
