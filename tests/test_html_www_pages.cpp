#include "html_parser.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef PROJECT_SOURCE_DIR
#define PROJECT_SOURCE_DIR ".."
#endif

static std::string read_file(const std::string& path)
{
    std::ifstream in(path);
    if (!in) {
        return "";
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string extract_title_from_json(const std::string& json)
{
    const std::string key = "\"title\": \"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        return "";
    }
    pos += key.size();
    size_t end = pos;
    while (end < json.size()) {
        if (json[end] == '\\') {
            end += 2;
            continue;
        }
        if (json[end] == '"') {
            break;
        }
        ++end;
    }
    return json.substr(pos, end - pos);
}

int main()
{
    const std::string www_dir = std::string(PROJECT_SOURCE_DIR) + "/www";
    const std::vector<std::pair<std::string, std::string>> pages = {
        {"page_01.html", "首页：HTTP 服务器测试"},
        {"page_02.html", "产品介绍页"},
        {"page_03.html", "技术文档 Alpha"},
        {"page_04.html", "用户手册 Beta"},
        {"page_05.html", "bench page 005"},
        {"page_06.html", "新闻动态 2026"},
        {"page_07.html", "关于我们"},
        {"page_08.html", "联系方式"},
        {"page_09.html", "FAQ 常见问题"},
        {"page_10.html", "下载中心"},
    };

    for (const auto& [filename, expected_title] : pages) {
        const std::string path = www_dir + "/" + filename;
        const std::string html = read_file(path);
        assert(!html.empty() && "failed to read html file");

        const std::string json = html_to_json(html);
        const std::string title = extract_title_from_json(json);

        if (title != expected_title) {
            std::cerr << "FAIL " << filename << "\n"
                      << "  expected title: " << expected_title << "\n"
                      << "  actual title:   " << title << "\n"
                      << "  json: " << json << "\n";
            assert(false);
        }

        assert(!extract_title_from_json(json).empty());
        std::cout << "OK  " << filename << " -> title=\"" << title << "\"\n";
    }

    std::cout << "www html title tests passed (" << pages.size() << " pages)\n";
    return 0;
}
