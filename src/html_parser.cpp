#include "html_parser.h"
#include <string>

// escape_json: 将纯文本中的特殊字符转义为合法的 JSON 字符串内容
// 该函数处理双引号、反斜杠、控制字符等，确保输出可以直接嵌入 JSON 文本
static std::string escape_json(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for(char c:text)
    {
        switch(c)
        {
            /*以下是ai代码*/
            case '\"':
                result+="\\\"";
                break;
            case '\\':
                result+="\\\\";
                break;
            case '\b':
                result+="\\b";
                break;
            case '\f':
                result+="\\f";
                break;
            case '\n':
                result+="\\n";
                break;
            case '\r':
                result+="\\r";
                break;
            case '\t':
                result+="\\t";
                break;
             /*以上是ai生成的代码*/
            
             default:
                if(static_cast<unsigned char>(c)<0x20)
                {
                    char buf[7];
                    snprintf(buf,sizeof(buf),"\\u%04x",c);
                    result+=buf;
                }
                else 
                {
                    result.push_back(c);
                }
        }
    }
    return result;
}



// extract_text: 从 HTML 内容中过滤掉标签，只保留可见文本及空格分隔符
static std::string extract_text(const std::string& html)
{
    std::string result;
    result.reserve(html.size());
    bool in_tag = false;
    for (char c : html)
    {
        if (c == '<')
        {
            in_tag = true;
        }
        else if (c == '>')
        {
            in_tag = false;
            if (!result.empty() && result.back() != ' ')
            {
                result.push_back(' ');
            }
        }
        else if (!in_tag)
        {
            if (c == '\n' || c == '\r' || c == '\t')
            {
                result.push_back(' ');
            }
            else
            {
                result.push_back(c);
            }
        }
    }

    // 合并连续空格，去掉首尾空格
    std::string compact;
    compact.reserve(result.size());
    bool last_space = false;
    for (char c : result)
    {
        bool is_space = (c == ' ');
        if (is_space)
        {
            if (!last_space)
            {
                compact.push_back(' ');
                last_space = true;
            }
        }
        else
        {
            compact.push_back(c);
            last_space = false;
        }
    }
    if (!compact.empty() && compact.front() == ' ') {
        compact.erase(compact.begin());
    }
    if (!compact.empty() && compact.back() == ' ') {
        compact.pop_back();
    }
    return compact;
}

// html_to_json: 解析 HTML 并将提取到的纯文本转换为 JSON 字符串
std::string html_to_json(const std::string& html_content)
{
    std::string text = extract_text(html_content);
    std::string escaped = escape_json(text);
    return "{\"text\": \"" + escaped + "\"}";
}
