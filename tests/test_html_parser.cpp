#include <http_parser.h>
#include <iostream>
#include <cassert>

int main() {
    {
        std::string html = "<html><body><h1>欢迎</h1><p>这是测试页面。</p></body></html>";
        std::string expected = "{\"text\": \"欢迎 这是测试页面。\"}";
        std::string result = html_to_json(html);
        assert(result == expected);
    }

    {
        std::string html = "<div>Line1</div><div>Line2</div>";
        std::string expected = "{\"text\": \"Line1 Line2\"}";
        std::string result = html_to_json(html);
        assert(result == expected);
    }

    {
        std::string html = "<p>Quote \"test\"</p>";
        std::string expected = "{\"text\": \"Quote \\\"test\\\"\"}";
        std::string result = html_to_json(html);
        assert(result == expected);
    }

    std::cout << "html_parser tests passed" << std::endl;
    return 0;
}
