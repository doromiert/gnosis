#pragma once
#include <string>
namespace Policy {
constexpr const char* new_tab_url = "about:blank";
bool is_new_tab(const std::string&);
bool is_http(const std::string&);
bool navigable(const std::string&);
std::string normalize(const std::string&);
std::string title(const std::string&, const std::string& page_title = "");
} // namespace Policy
