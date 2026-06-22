#include "decompress.hh"
#include <zlib.h>
static constexpr int CHUNK_SIZE = 2048;
std::vector<char> zlibDecompress(const char* bufptr, unsigned length, unsigned long adler32_checksum) {
  z_stream zs{};
  std::vector<char> str;
  int res = Z_OK;
  zs.next_in = (Bytef*)bufptr;
  zs.avail_in = length;
  res = inflateInit(&zs);
  if (res == Z_OK) {
    char buf[CHUNK_SIZE];
    while (res != Z_STREAM_END) {
      zs.next_out = (Bytef*)buf;
      zs.avail_out = CHUNK_SIZE;
      res = inflate(&zs, Z_SYNC_FLUSH);
      str.insert(str.end(), buf, buf + CHUNK_SIZE - zs.avail_out);
      if (res != Z_OK && res != Z_STREAM_END) break;
    }
  }
  int endRes = inflateEnd(&zs);
  // Skip adler32 checksum check - MDICT checksum may differ from zlib's internal one
  if (res != Z_STREAM_END) {
    str.clear();
  }
  return str;
}
std::string decompressZlib(const char* bufptr, unsigned length) {
  auto b = zlibDecompress(bufptr, length, 0);
  return std::string(b.data(), b.size());
}
