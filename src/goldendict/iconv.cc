#include "iconv.hh"
#include <vector>
#include <algorithm>

static UINT encodingToCodePage(const char* encoding) {
  if (!encoding) return CP_UTF8;
  std::string enc(encoding);
  std::transform(enc.begin(), enc.end(), enc.begin(), [](unsigned char c) { return std::toupper(c); });
  if (enc == "UTF-8" || enc == "UTF8") return CP_UTF8;
  if (enc == "UTF-16LE" || enc == "UTF16LE") return 1200;
  if (enc == "UTF-16BE" || enc == "UTF16BE") return 1201;
  if (enc == "UTF-16" || enc == "UTF16") return 1200;
  if (enc == "GBK" || enc == "GB2312" || enc == "GB18030") return 54936;
  if (enc == "SHIFT-JIS" || enc == "SHIFT_JIS" || enc == "SJIS" || enc == "MS932") return 932;
  if (enc == "EUC-KR" || enc == "EUCKR" || enc == "MS949") return 949;
  if (enc == "BIG5" || enc == "MS950") return 950;
  if (enc == "WINDOWS-1252" || enc == "CP1252") return 1252;
  if (enc == "WINDOWS-1251" || enc == "CP1251") return 1251;
  if (enc == "WINDOWS-1250" || enc == "CP1250") return 1250;
  if (enc == "ISO-8859-1" || enc == "LATIN1") return 28591;
  if (enc == "ISO-8859-2" || enc == "LATIN2") return 28592;
  if (enc == "ISO-8859-15") return 28605;
  return CP_UTF8;
}

Iconv::Iconv(const char* from) : codePage_(encodingToCodePage(from)) {}

static std::wstring multiByteToWide(UINT cp, const char* data, size_t len) {
  if (len == 0) return {};
  int wlen = MultiByteToWideChar(cp, 0, data, (int)len, nullptr, 0);
  if (wlen <= 0) return {};
  std::wstring result(wlen, 0);
  MultiByteToWideChar(cp, 0, data, (int)len, &result[0], wlen);
  return result;
}

static std::string wideToMultiByte(UINT cp, const wchar_t* data, size_t len) {
  if (len == 0) return {};
  int mlen = WideCharToMultiByte(cp, 0, data, (int)len, nullptr, 0, nullptr, nullptr);
  if (mlen <= 0) return {};
  std::string result(mlen, 0);
  WideCharToMultiByte(cp, 0, data, (int)len, &result[0], mlen, nullptr, nullptr);
  return result;
}

std::string Iconv::convert(const void*& inBuf, size_t& inBytesLeft) {
  auto wide = multiByteToWide(codePage_, (const char*)inBuf, inBytesLeft);
  inBytesLeft = 0;
  return wideToMultiByte(CP_UTF8, wide.data(), wide.size());
}

std::u32string Iconv::toWstring(const char* fromEncoding, const void* fromData, size_t dataSize) {
  if (dataSize == 0) return {};
  UINT cp = encodingToCodePage(fromEncoding);
  auto wide = multiByteToWide(cp, (const char*)fromData, dataSize);
  return Text::toUtf32(wideToMultiByte(CP_UTF8, wide.data(), wide.size()));
}
std::string Iconv::toUtf8(const char* fromEncoding, const void* fromData, size_t dataSize) {
  if (dataSize == 0) return {};
  UINT cp = encodingToCodePage(fromEncoding);
  // For UTF-16LE, skip the MultiByteToWideChar step and cast directly
  if (cp == 1200) {
    size_t wlen = dataSize / sizeof(wchar_t);
    std::wstring wide((const wchar_t*)fromData, wlen);
    return wideToMultiByte(CP_UTF8, wide.data(), wide.size());
  }
  auto wide = multiByteToWide(cp, (const char*)fromData, dataSize);
  return wideToMultiByte(CP_UTF8, wide.data(), wide.size());
}

std::string Iconv::toQString(const char* fromEncoding, const void* fromData, size_t dataSize) {
  if (dataSize == 0) return {};
  UINT cp = encodingToCodePage(fromEncoding);
  // For UTF-16LE, skip the MultiByteToWideChar step and cast directly
  if (cp == 1200) {
    size_t wlen = dataSize / sizeof(wchar_t);
    std::wstring wide((const wchar_t*)fromData, wlen);
    return wideToMultiByte(CP_UTF8, wide.data(), wide.size());
  }
  auto wide = multiByteToWide(cp, (const char*)fromData, dataSize);
  return wideToMultiByte(CP_UTF8, wide.data(), wide.size());
}

bool Iconv::isValidUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    size_t seqLen;
    if (c <= 0x7F) seqLen = 1;
    else if ((c & 0xE0) == 0xC0) seqLen = 2;
    else if ((c & 0xF0) == 0xE0) seqLen = 3;
    else if ((c & 0xF8) == 0xF0) seqLen = 4;
    else return false;
    if (i + seqLen > s.size()) return false;
    for (size_t j = 1; j < seqLen; j++) {
      if (((unsigned char)s[i + j] & 0xC0) != 0x80) return false;
    }
    // Check for overlong sequences
    if (seqLen == 2 && c < 0xC2) return false;
    if (seqLen == 3) {
      unsigned char c2 = (unsigned char)s[i + 1];
      if (c == 0xE0 && c2 < 0xA0) return false;
      if (c == 0xED && c2 > 0x9F) return false;
    }
    if (seqLen == 4) {
      unsigned char c2 = (unsigned char)s[i + 1];
      if (c == 0xF0 && c2 < 0x90) return false;
      if (c == 0xF4 && c2 > 0x8F) return false;
      if (c > 0xF4) return false;
    }
    i += seqLen;
  }
  return true;
}

static std::string sanitizeUtf8(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    size_t seqLen;
    if (c <= 0x7F) seqLen = 1;
    else if ((c & 0xE0) == 0xC0) seqLen = 2;
    else if ((c & 0xF0) == 0xE0) seqLen = 3;
    else if ((c & 0xF8) == 0xF0) seqLen = 4;
    else { result += "\xEF\xBF\xBD"; i++; continue; } // U+FFFD
    if (i + seqLen > s.size()) { result += "\xEF\xBF\xBD"; break; }
    bool valid = true;
    for (size_t j = 1; j < seqLen; j++) {
      if (((unsigned char)s[i + j] & 0xC0) != 0x80) { valid = false; break; }
    }
    if (valid) {
      result.append(s, i, seqLen);
      i += seqLen;
    } else {
      result += "\xEF\xBF\xBD";
      i++;
    }
  }
  return result;
}

std::string Iconv::ensureUtf8(const std::string& s) {
  if (s.empty()) return s;
  if (isValidUtf8(s)) return s;
  // Not valid UTF-8 - try converting from GBK (common for Chinese MDX with wrong encoding header)
  auto converted = toUtf8("GBK", s.data(), s.size());
  if (!converted.empty() && isValidUtf8(converted)) return converted;
  // GBK conversion failed or still invalid - sanitize
  if (!converted.empty()) return sanitizeUtf8(converted);
  return sanitizeUtf8(s);
}
// Debug: write raw bytes to file for inspection
static void debugDumpHeader(const char* data, size_t len, const char* tag) {
  char path[MAX_PATH];
  GetTempPathA(MAX_PATH, path);
  strcat_s(path, "gd_header_dump.bin");
  FILE* f = fopen(path, "ab");
  if (f) { fwrite(data, 1, len, f); fclose(f); }
}
