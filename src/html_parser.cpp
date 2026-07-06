#include "html_parser.h"
#include <gumbo.h>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace {

// 去除字符串前后空格，只处理空格字符 ' '
std::string trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && value[begin] == ' ') {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && value[end - 1] == ' ') {
        --end;
    }
    return value.substr(begin, end - begin);
}

// 获取元素节点的标签名，非元素节点返回空字符串
std::string node_tag(const GumboNode* node)
{
    if (node->type == GUMBO_NODE_ELEMENT) {
        return gumbo_normalized_tagname(node->v.element.tag);
    }
    return "";
}

// 收集当前节点直接子节点中的文本内容，并去掉首尾空格
std::string collect_text(const GumboNode* node)
{
    std::string text;
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        const GumboNode* child = static_cast<const GumboNode*>(children->data[i]);
        if (child->type == GUMBO_NODE_TEXT || child->type == GUMBO_NODE_WHITESPACE) {
            text += child->v.text.text;
        }
    }
    return trim(text);
}

// 将 HTML 元素节点转换为 JSON 对象
// 如果元素有子元素，则生成 children 数组；否则生成 text 字段
json element_to_json(const GumboNode* node)
{
    json obj;
    obj["tag"] = node_tag(node);

    json children = json::array();
    const GumboVector* child_nodes = &node->v.element.children;
    for (unsigned int i = 0; i < child_nodes->length; ++i) {
        const GumboNode* child = static_cast<const GumboNode*>(child_nodes->data[i]);
        if (child->type == GUMBO_NODE_ELEMENT || child->type == GUMBO_NODE_TEMPLATE) {
            children.push_back(element_to_json(child));
        }
    }

    if (!children.empty()) {
        obj["children"] = std::move(children);
        return obj;
    }

    const std::string text = collect_text(node);
    if (!text.empty()) {
        obj["text"] = text;
    }
    return obj;
}

// 将解析树根节点转换为 JSON。若根节点本身是元素，直接转换；
// 否则遍历 document 的子元素，可能生成单个根对象或 root 包装对象
json root_to_json(const GumboNode* root)
{
    if (root->type == GUMBO_NODE_ELEMENT) {
        return element_to_json(root);
    }

    json roots = json::array();
    const GumboVector* children = &root->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        const GumboNode* child = static_cast<const GumboNode*>(children->data[i]);
        if (child->type == GUMBO_NODE_ELEMENT || child->type == GUMBO_NODE_TEMPLATE) {
            roots.push_back(element_to_json(child));
        }
    }

    if (roots.empty()) {
        return json::object();
    }
    if (roots.size() == 1) {
        return roots[0];
    }

    json wrapper;
    wrapper["tag"] = "root";
    wrapper["children"] = std::move(roots);
    return wrapper;
}

}  // namespace

// 将 HTML 字符串解析为 JSON 字符串
std::string html_to_json(const std::string& html_content)
{
    // 使用 Gumbo 解析 HTML，支持不规范的 HTML 结构
    GumboOutput* output = gumbo_parse_with_options(
        &kGumboDefaultOptions,
        html_content.data(),
        html_content.size());

    if (output == nullptr || output->root == nullptr) {
        return "{}";
    }

    const json tree = root_to_json(output->root);
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    return tree.dump(2);
}
