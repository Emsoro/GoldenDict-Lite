#include "text.hh"
#include <vector>
namespace Text {
static size_t encode(const char32_t* in, size_t inSize, char* out_) {
  unsigned char* out = (unsigned char*)out_;
  while (inSize--) {
    if (*in < 0x80) { *out++ = *in++; }
    else if (*in < 0x800) { *out++ = 0xC0 | (*in >> 6); *out++ = 0x80 | (*in++ & 0x3F); }
    else if (*in < 0x10000) { *out++ = 0xE0 | (*in >> 12); *out++ = 0x80 | ((*in >> 6) & 0x3F); *out++ = 0x80 | (*in++ & 0x3F); }
    else { *out++ = 0xF0 | (*in >> 18); *out++ = 0x80 | ((*in >> 12) & 0x3F); *out++ = 0x80 | ((*in >> 6) & 0x3F); *out++ = 0x80 | (*in++ & 0x3F); }
  }
  return out - (unsigned char*)out_;
}
static long decode(const char* in_, size_t inSize, char32_t* out_) {
  const unsigned char* in = (const unsigned char*)in_;
  char32_t* out = out_;
  while (inSize--) {
    char32_t result;
    if (*in & 0x80) {
      if (*in & 0x40) {
        if (*in & 0x20) {
          if (*in & 0x10) {
            if (*in & 8) return -1;
            if (inSize < 3) return -1;
            inSize -= 3;
            result = ((char32_t)*in++ & 7) << 18;
            if ((*in & 0xC0) != 0x80) return -1;
            result |= ((char32_t)*in++ & 0x3F) << 12;
            if ((*in & 0xC0) != 0x80) return -1;
            result |= ((char32_t)*in++ & 0x3F) << 6;
            if ((*in & 0xC0) != 0x80) return -1;
            result |= (char32_t)*in++ & 0x3F;
          } else {
            if (inSize < 2) return -1;
            inSize -= 2;
            result = ((char32_t)*in++ & 0xF) << 12;
            if ((*in & 0xC0) != 0x80) return -1;
            result |= ((char32_t)*in++ & 0x3F) << 6;
            if ((*in & 0xC0) != 0x80) return -1;
            result |= (char32_t)*in++ & 0x3F;
          }
        } else {
          if (!inSize) return -1;
          --inSize;
          result = ((char32_t)*in++ & 0x1F) << 6;
          if ((*in & 0xC0) != 0x80) return -1;
          result |= (char32_t)*in++ & 0x3F;
        }
      } else { return -1; }
    } else { result = *in++; }
    *out++ = result;
  }
  return out - out_;
}
std::string toUtf8(const std::u32string& in) noexcept {
  if (in.empty()) return {};
  std::vector<char> buffer(in.size() * 4);
  return {&buffer.front(), encode(in.data(), in.size(), &buffer.front())};
}
std::u32string toUtf32(const std::string& in) {
  if (in.empty()) return {};
  std::vector<char32_t> buffer(in.size());
  long result = decode(in.data(), in.size(), &buffer.front());
  if (result < 0) throw exCantDecode(in);
  return std::u32string(&buffer.front(), result);
}
bool isspace(int c) {
  return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v';
}
std::u32string removeTrailingZero(const std::u32string& v) {
  int n = v.size();
  while (n > 0 && v[n - 1] == 0) n--;
  return std::u32string(v.data(), n);
}
}
