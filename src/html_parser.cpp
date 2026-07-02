#include "html_parser.h"
#include "pugixml.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string escape_json(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    result += buf;
                } else {
                    result.push_back(c);
                }
        }
    }
    return result;
}

static std::string trim(const std::string& value)
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

static bool is_heading_tag(const char* name)
{
    if (name == nullptr || name[0] != 'h') {
        return false;
    }
    return name[1] >= '1' && name[1] <= '6' && name[2] == '\0';
}

static std::string extract_title(const pugi::xml_document& doc)
{
    pugi::xpath_node title_node = doc.select_node("//title");
    if (!title_node) {
        return "";
    }
    return trim(title_node.node().child_value());
}

static pugi::xml_node find_body_node(const pugi::xml_document& doc)
{
    pugi::xml_node body = doc.select_node("//body").node();
    if (body) {
        return body;
    }

    pugi::xml_node html = doc.child("html");
    if (html) {
        body = html.child("body");
        if (body) {
            return body;
        }
    }
    return pugi::xml_node();
}

static void collect_node(
    const pugi::xml_node& node,
    std::vector<std::string>& headings,
    std::vector<std::string>& paragraphs)
{
    if (!node || node.type() != pugi::node_element) {
        return;
    }

    const char* name = node.name();
    if (is_heading_tag(name)) {
        std::string text = trim(node.text().get());
        if (!text.empty()) {
            headings.push_back(text);
        }
        return;
    }

    if (std::strcmp(name, "p") == 0) {
        std::string text = trim(node.text().get());
        if (!text.empty()) {
            paragraphs.push_back(text);
        }
        return;
    }

    if (std::strcmp(name, "div") == 0) {
        bool has_element_child = false;
        for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
            if (child.type() == pugi::node_element) {
                has_element_child = true;
                break;
            }
        }
        if (!has_element_child) {
            std::string text = trim(node.text().get());
            if (!text.empty()) {
                paragraphs.push_back(text);
            }
            return;
        }
    }

    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_element) {
            collect_node(child, headings, paragraphs);
        }
    }
}

static void collect_structured_content(
    const pugi::xml_document& doc,
    std::vector<std::string>& headings,
    std::vector<std::string>& paragraphs)
{
    pugi::xml_node body = find_body_node(doc);
    if (body) {
        for (pugi::xml_node child = body.first_child(); child; child = child.next_sibling()) {
            collect_node(child, headings, paragraphs);
        }
        return;
    }

    for (pugi::xml_node child = doc.first_child(); child; child = child.next_sibling()) {
        collect_node(child, headings, paragraphs);
    }
}

static std::string json_string_array(const std::vector<std::string>& items)
{
    std::string result = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            result += ", ";
        }
        result += "\"";
        result += escape_json(items[i]);
        result += "\"";
    }
    result += "]";
    return result;
}

std::string html_to_json(const std::string& html_content)
{
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(
        html_content.data(),
        html_content.size(),
        pugi::parse_default | pugi::parse_fragment);

    if (!result) {
        return "{\"title\": \"\", \"body\": {\"headings\": [], \"paragraphs\": []}}";
    }

    std::vector<std::string> headings;
    std::vector<std::string> paragraphs;
    collect_structured_content(doc, headings, paragraphs);

    const std::string title = escape_json(extract_title(doc));
    return "{\"title\": \"" + title + "\", \"body\": {\"headings\": "
           + json_string_array(headings) + ", \"paragraphs\": "
           + json_string_array(paragraphs) + "}}";
}
