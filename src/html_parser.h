#pragma once

#include <string>

// Gumbo 解析 HTML → DOM 树 JSON
// 元素节点: {"tag":"h1", "text":"..."} 或 {"tag":"body", "children":[...]}
std::string html_to_json(const std::string& html_content);
