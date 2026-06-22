#include "htmlescape.hh"
#include <algorithm>
namespace Html {
string escape(const string& str) {
  string result(str);
  for (size_t x = result.size(); x--;) {
    switch (result[x]) {
      case '&': result.erase(x,1); result.insert(x,"&amp;"); break;
      case '<': result.erase(x,1); result.insert(x,"&lt;"); break;
      case '>': result.erase(x,1); result.insert(x,"&gt;"); break;
      case '"': result.erase(x,1); result.insert(x,"&quot;"); break;
      default: break;
    }
  }
  return result;
}
string preformat(const string& str, bool baseRightToLeft) {
  string escaped = escape(str), result, line;
  line.reserve(escaped.size());
  result.reserve(escaped.size());
  bool leading = true;
  for (const char* nextChar = escaped.c_str(); *nextChar; ++nextChar) {
    if (leading) {
      if (*nextChar == ' ') { line += "&nbsp;"; continue; }
      else if (*nextChar == '\t') { line += "&nbsp;&nbsp;&nbsp;&nbsp;"; continue; }
    }
    if (*nextChar == '\n') { result += "<div>" + line + "</div>"; line.clear(); leading = true; continue; }
    if (*nextChar == '\r') continue;
    line.push_back(*nextChar);
    leading = false;
  }
  if (!line.empty()) result += "<div>" + line + "</div>";
  return result;
}
string escapeForJavaScript(const string& str) {
  string result(str);
  for (size_t x = result.size(); x--;) {
    switch (result[x]) {
      case '\\': case '"': case '\'': result.insert(x, 1, '\\'); break;
      case '\n': result.erase(x,1); result.insert(x,"\\n"); break;
      case '\r': result.erase(x,1); result.insert(x,"\\r"); break;
      case '\t': result.erase(x,1); result.insert(x,"\\t"); break;
      default: break;
    }
  }
  return result;
}
string unescapeUtf8(const string& str) {
  string result;
  result.reserve(str.size());
  for (size_t i = 0; i < str.size(); ) {
    if (str[i] == '&' && i + 1 < str.size()) {
      if (str.compare(i, 4, "&lt;") == 0) { result += '<'; i += 4; }
      else if (str.compare(i, 4, "&gt;") == 0) { result += '>'; i += 4; }
      else if (str.compare(i, 6, "&quot;") == 0) { result += '"'; i += 6; }
      else if (str.compare(i, 5, "&amp;") == 0) { result += '&'; i += 5; }
      else { result += str[i]; i++; }
    } else { result += str[i]; i++; }
  }
  return result;
}
string fromHtmlEscaped(const string& str) { return unescapeUtf8(str); }
}
