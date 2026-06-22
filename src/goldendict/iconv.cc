#include "iconv.hh"
#include <vector>
#include <algorithm>

static UINT encodingToCodePage(const char* encoding) {
  if (!encoding) return CP_UTF8;
  std::string enc(encoding);
  std::transform(enc.begin(), enc.end(), enc.begin(), ::toupper);
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
// Debug: write raw bytes to file for inspection
static void debugDumpHeader(const char* data, size_t len, const char* tag) {
  char path[MAX_PATH];
  GetTempPathA(MAX_PATH, path);
  strcat_s(path, "gd_header_dump.bin");
  FILE* f = fopen(path, "ab");
  if (f) { fwrite(data, 1, len, f); fclose(f); }
}
