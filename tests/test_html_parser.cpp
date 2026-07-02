#include "html_parser.h"
#include <cassert>
#include <iostream>
#include <string>

int main()
{
    {
        std::string html =
            "<html><head><title>测试标题</title></head>"
            "<body><h1>欢迎</h1><p>这是测试页面。</p></body></html>";
        std::string expected =
            "{\"title\": \"测试标题\", \"body\": {\"headings\": [\"欢迎\"], "
            "\"paragraphs\": [\"这是测试页面。\"]}}";
        assert(html_to_json(html) == expected);
    }

    {
        std::string html = "<div>Line1</div><div>Line2</div>";
        std::string expected =
            "{\"title\": \"\", \"body\": {\"headings\": [], "
            "\"paragraphs\": [\"Line1\", \"Line2\"]}}";
        assert(html_to_json(html) == expected);
    }

    {
        std::string html = "<html><head><title>Quote \"test\"</title></head>"
                           "<body><h2>小节</h2><p>正文</p></body></html>";
        std::string expected =
            "{\"title\": \"Quote \\\"test\\\"\", \"body\": {\"headings\": [\"小节\"], "
            "\"paragraphs\": [\"正文\"]}}";
        assert(html_to_json(html) == expected);
    }

    std::cout << "html_parser tests passed" << std::endl;
    return 0;
}
