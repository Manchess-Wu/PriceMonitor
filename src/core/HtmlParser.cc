#include "core/HtmlParser.h"

#include <cctype>
#include <cstring>

#include <lexbor/dom/dom.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/html/html.h>
#include <lexbor/html/parser.h>

#include "core/Util.h"

namespace pm {
namespace {

using Element = lxb_dom_element_t;
using Node = lxb_dom_node_t;

std::string elementAttr(Element* element, const char* name) {
    size_t valueLen = 0;
    const lxb_char_t* value = lxb_dom_element_get_attribute(
        element, reinterpret_cast<const lxb_char_t*>(name), std::strlen(name),
        &valueLen);
    if (!value) return "";
    return std::string(reinterpret_cast<const char*>(value), valueLen);
}

std::string tagName(Element* element) {
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(element, &len);
    return name ? std::string(reinterpret_cast<const char*>(name), len) : "";
}

template <typename Fn>
bool walkElements(Node* node, Fn&& fn) {
    for (Node* cur = node; cur != nullptr; cur = lxb_dom_node_next(cur)) {
        if (lxb_dom_node_type(cur) == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (fn(lxb_dom_interface_element(cur))) return true;
        }
        Node* child = lxb_dom_node_first_child(cur);
        if (child != nullptr && walkElements(child, fn)) return true;
    }
    return false;
}

bool hasClassToken(Element* element, const std::string& className) {
    std::string classes = elementAttr(element, "class");
    if (classes.empty()) return false;
    size_t pos = 0;
    while ((pos = classes.find(className, pos)) != std::string::npos) {
        bool leftOk = pos == 0 ||
                      std::isspace(static_cast<unsigned char>(classes[pos - 1])) != 0;
        size_t end = pos + className.size();
        bool rightOk = end >= classes.size() ||
                       std::isspace(static_cast<unsigned char>(classes[end])) != 0;
        if (leftOk && rightOk) return true;
        pos = end;
    }
    return false;
}

void appendText(Node* node, std::string& out) {
    for (Node* cur = node; cur != nullptr; cur = lxb_dom_node_next(cur)) {
        auto type = lxb_dom_node_type(cur);
        if (type == LXB_DOM_NODE_TYPE_TEXT || type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
            auto* data = reinterpret_cast<lxb_dom_character_data_t*>(cur);
            if (data->data.data && data->data.length > 0) {
                out.append(reinterpret_cast<const char*>(data->data.data),
                           data->data.length);
            }
        } else if (type == LXB_DOM_NODE_TYPE_ELEMENT) {
            std::string tag = tagName(lxb_dom_interface_element(cur));
            // 忽略脚本与样式内容，避免把 JS 字符串误当作文本
            if (tag == "script" || tag == "style" || tag == "noscript" ||
                tag == "template") {
                continue;
            }
            Node* child = lxb_dom_node_first_child(cur);
            if (child != nullptr) appendText(child, out);
        }
    }
}

Element* findById(Node* root, const std::string& id) {
    Element* found = nullptr;
    walkElements(root, [&](Element* element) {
        if (elementAttr(element, "id") == id) {
            found = element;
            return true;
        }
        return false;
    });
    return found;
}

Element* findByClass(Node* root, const std::string& className) {
    Element* found = nullptr;
    walkElements(root, [&](Element* element) {
        if (hasClassToken(element, className)) {
            found = element;
            return true;
        }
        return false;
    });
    return found;
}

std::string elementText(Element* element) {
    std::string text;
    // 必须从第一个子节点开始遍历：appendText 会沿 next 兄弟链推进，
    // 若从 element 本身开始，会把后续兄弟节点的文本一并收集进来。
    Node* child = lxb_dom_node_first_child(reinterpret_cast<Node*>(element));
    if (child != nullptr) appendText(child, text);
    return normalizeSpace(htmlUnescape(text));
}

}  // namespace

HtmlDocument::HtmlDocument(const std::string& html) {
    doc_ = lxb_html_document_create();
    if (!doc_) return;
    lxb_status_t status = lxb_html_document_parse(
        doc_, reinterpret_cast<const lxb_char_t*>(html.data()), html.size());
    if (status != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc_);
        doc_ = nullptr;
    }
}

HtmlDocument::~HtmlDocument() {
    if (doc_) {
        lxb_html_document_destroy(doc_);
        doc_ = nullptr;
    }
}

std::string HtmlDocument::title() const {
    if (!doc_) return "";
    size_t len = 0;
    const lxb_char_t* value = lxb_html_document_title(doc_, &len);
    if (!value) return "";
    std::string text(reinterpret_cast<const char*>(value), len);
    return normalizeSpace(htmlUnescape(text));
}

std::string HtmlDocument::textById(const std::string& id) const {
    if (!doc_) return "";
    Element* element = findById(lxb_dom_interface_node(doc_), id);
    return element ? elementText(element) : "";
}

std::string HtmlDocument::textByClass(const std::string& className) const {
    if (!doc_) return "";
    Element* element = findByClass(lxb_dom_interface_node(doc_), className);
    return element ? elementText(element) : "";
}

std::string HtmlDocument::attrById(const std::string& id,
                                   const std::string& attr) const {
    if (!doc_) return "";
    Element* element = findById(lxb_dom_interface_node(doc_), id);
    return element ? htmlUnescape(elementAttr(element, attr.c_str())) : "";
}

std::string HtmlDocument::attrByClass(const std::string& className,
                                      const std::string& attr) const {
    if (!doc_) return "";
    Element* element = findByClass(lxb_dom_interface_node(doc_), className);
    return element ? htmlUnescape(elementAttr(element, attr.c_str())) : "";
}

std::string HtmlDocument::metaContent(const std::string& key) const {
    if (!doc_) return "";
    std::string loweredKey = toLower(key);
    std::string content;
    walkElements(lxb_dom_interface_node(doc_), [&](Element* element) {
        if (tagName(element) != "meta") return false;
        std::string name = toLower(elementAttr(element, "property"));
        if (name.empty()) name = toLower(elementAttr(element, "name"));
        if (name == loweredKey) {
            content = htmlUnescape(elementAttr(element, "content"));
            return true;
        }
        return false;
    });
    return content;
}

}  // namespace pm
