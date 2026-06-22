#pragma once
#include <string>
namespace Html {
using std::string;
string escape(const string&);
string preformat(const string&, bool baseRightToLeft = false);
string escapeForJavaScript(const string&);
string unescapeUtf8(const string& str);
string fromHtmlEscaped(const string& str);
}
