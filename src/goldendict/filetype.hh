#pragma once
#include <string>
namespace Filetype {
using std::string;
string simplifyString(const string& str, bool lowercase = true);
bool isNameOfSound(const string&);
bool isNameOfVideo(const string&);
bool isNameOfPicture(const string&);
bool isNameOfTiff(const string&);
bool isNameOfCSS(const string&);
bool isNameOfSvg(const string& name);
}
