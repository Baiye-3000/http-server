#include "html_parser.h"
#include <string>
//纯文本->JSON
static std::string escape_json(const std::string&text)
{
    std::string result;
    result.reserve(text.size());
    for(char c:text)
    {
        switch(c)
        {
            /*以下是代码*/
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
             /*以上是AI生成的代码*/
            
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



//HTML->纯文本
static std::string extract_text(const std::string& html)
{
    std::string result;
    result.reserve(html.size());
    bool in_tag=false;
    for(char c:html)
    {
        if(c=='<')
        {
            in_tag=true;
        }
        else if(c=='>')
        {
            in_tag=false;
            if(!result.empty()&&result.back()!=' ')
            {
                result.push_back(' ');
            }
        }
        else if(!in_tag)
        {
            if(c=='\n'||c=='\r'||c=='\t')
            {
                result.push_back(' ');
            }
            else
            {
                result.push_back(c);
            }
        }
    }
    std::string compact;
    compact.reserve(result.size());
    bool last_space=false;
    for(char c:result)
    {
        bool is_space=(c==' ');
        if(is_space)
        {
            if(!last_space)
            {
                compact.push_back(' ');
                last_space=true;
            }
            else 
            {
                last_space=false;
                compact.push_back(c);
            }
        }
    }
    if(!compact.empty()&&compact.front()==' ')
    {
        compact.erase(compact.begin());
    }
    if(!compact.empty()&&compact.back()==' ')
    {
        compact.pop_back();
    }
    return compact;
}

std::string html_to_json(const std::string& html_content) 
{
    std::string text = extract_text(html_content);
    std::string escaped = escape_json(text);
    return "{\"text\": \"" + escaped + "\"}";
}
