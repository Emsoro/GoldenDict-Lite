#pragma once
#include <string>

namespace Text {
inline constexpr auto utf8 = "UTF-8";
class exCantDecode : public std::exception {
  std::string message_;
public:
  explicit exCantDecode(const std::string& msg) : message_("Can't decode the given string from Utf8: " + msg) {}
  const char* what() const noexcept override { return message_.c_str(); }
};
std::string toUtf8(const std::u32string&) noexcept;
std::u32string toUtf32(const std::string&);
bool isspace(int c);
std::u32string removeTrailingZero(const std::u32string& v);
}
