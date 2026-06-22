#pragma once
#include "text.hh"
#include <string>
#include <Windows.h>

class Iconv {
public:
  class Ex : public std::exception {
  public:
    const char* what() const noexcept override { return "Iconv exception"; }
  };
  class exCantInit : public Ex {
    std::string message_;
  public:
    explicit exCantInit(const std::string& msg) : message_("Can't initialize iconv conversion: " + msg) {}
    const char* what() const noexcept override { return message_.c_str(); }
  };
  explicit Iconv(const char* from);
  ~Iconv() = default;
  std::string convert(const void*& inBuf, size_t& inBytesLeft);
  static std::u32string toWstring(const char* fromEncoding, const void* fromData, size_t dataSize);
  static std::string toUtf8(const char* fromEncoding, const void* fromData, size_t dataSize);
  static std::string toQString(const char* fromEncoding, const void* fromData, size_t dataSize);
  Iconv(const Iconv&) = delete;
  Iconv& operator=(const Iconv&) = delete;
private:
  UINT codePage_;
};
