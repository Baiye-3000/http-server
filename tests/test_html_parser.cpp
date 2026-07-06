#include "html_parser.h"
#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

static const nlohmann::json* find_child(const nlohmann::json& node, const std::string& tag)
{
    if (!node.contains("children")) {
        return nullptr;
    }
    for (const auto& child : node["children"]) {
        if (child.contains("tag") && child["tag"] == tag) {
            return &child;
        }
    }
    return nullptr;
}

int main()
{
    using json = nlohmann::json;

    {
        const std::string html =
            "<html><body><h1>Hello</h1><p>World</p></body></html>";
        const json tree = json::parse(html_to_json(html));

        assert(tree["tag"] == "html");
        const json* body = find_child(tree, "body");
        assert(body != nullptr);
        assert(body->at("children").size() == 2);
        assert(body->at("children")[0]["tag"] == "h1");
        assert(body->at("children")[0]["text"] == "Hello");
        assert(body->at("children")[1]["tag"] == "p");
        assert(body->at("children")[1]["text"] == "World");
    }

    {
        const std::string html =
            "<html><head><title>测试标题</title></head>"
            "<body><h1>欢迎</h1><p>这是测试页面。</p></body></html>";
        const json tree = json::parse(html_to_json(html));

        assert(tree["tag"] == "html");
        const json* head = find_child(tree, "head");
        const json* body = find_child(tree, "body");
        assert(head != nullptr && body != nullptr);
        assert(head->at("children")[0]["tag"] == "title");
        assert(head->at("children")[0]["text"] == "测试标题");
        assert(body->at("children")[0]["text"] == "欢迎");
        assert(body->at("children")[1]["text"] == "这是测试页面。");
    }

    {
        const std::string html = "<div>Line1</div><div>Line2</div>";
        const json tree = json::parse(html_to_json(html));

        assert(tree["tag"] == "html");
        const json* body = find_child(tree, "body");
        assert(body != nullptr);
        assert(body->at("children").size() == 2);
        assert(body->at("children")[0]["text"] == "Line1");
        assert(body->at("children")[1]["text"] == "Line2");
    }

    {
        const std::string html =
            "<html><head><title>Quote \"test\"</title></head>"
            "<body><h2>小节</h2></body></html>";
        const json tree = json::parse(html_to_json(html));

        const json* head = find_child(tree, "head");
        assert(head != nullptr);
        assert(head->at("children")[0]["tag"] == "title");
        assert(head->at("children")[0]["text"] == "Quote \"test\"");
    }

    std::cout << "html_parser tests passed" << std::endl;
    return 0;
}
