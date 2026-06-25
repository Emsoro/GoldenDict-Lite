#include "mdictparser.hh"
#include "decompress.hh"
#include "htmlescape.hh"
#include "iconv.hh"
#include "ripemd.hh"
#include <zlib.h>
#include <cstring>
#include <algorithm>
#include <regex>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

// Helper: open file with Chinese path support on Windows
// Uses CP_ACP because std::filesystem::path::string() returns ANSI-encoded strings on Windows
static FILE* openFileWide(const char* path, const char* mode) {
  int wlen = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
  if (wlen <= 0) return nullptr;
  std::wstring wpath(wlen, 0);
  MultiByteToWideChar(CP_ACP, 0, path, -1, &wpath[0], wlen);
  std::wstring wmode;
  while (*mode) wmode += (wchar_t)*mode++;
  return _wfopen(wpath.c_str(), wmode.c_str());
}

// Helper: fseek with 64-bit offset on Windows
static int fseek64(FILE* f, int64_t offset, int origin) {
  return _fseeki64(f, offset, origin);
}

// Helper: ftell with 64-bit offset on Windows
static int64_t ftell64(FILE* f) {
  return _ftelli64(f);
}

static void* memmap_create(FILE* f, int64_t offset, int64_t size, void** mapAddr) {
  HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(f));
  if (hFile == INVALID_HANDLE_VALUE) return nullptr;
  DWORD offsetHi = (DWORD)((uint64_t)offset >> 32);
  DWORD offsetLo = (DWORD)(offset & 0xFFFFFFFF);
  HANDLE hMap = CreateFileMapping(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!hMap) return nullptr;
  *mapAddr = MapViewOfFile(hMap, FILE_MAP_READ, offsetHi, offsetLo, size);
  return hMap;
}
static void memmap_destroy(void* hMap, void* mapAddr, size_t) {
  if (mapAddr) UnmapViewOfFile(mapAddr);
  if (hMap) CloseHandle(hMap);
}
#else
#include <sys/mman.h>
static void* memmap_create(FILE* f, int64_t offset, int64_t size, void** mapAddr) {
  int fd = fileno(f);
  *mapAddr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, offset);
  return (*mapAddr == MAP_FAILED) ? nullptr : (void*)1;
}
static void memmap_destroy(void* hMap, void* mapAddr, size_t mapSize) {
  if (hMap && mapAddr) munmap(mapAddr, mapSize);
}
#endif

using namespace Mdict;

static inline int u16StrSize(const uint16_t* unicode) {
  int size = 0;
  if (unicode) { while (unicode[size] != 0) size++; }
  return size;
}

static map<string, string> parseHeaderAttributes(const string& headerText) {
  map<string, string> attrs;
  std::regex attrRegex("(\\w+)\\s*=\\s*\"([^\"]*)\"", std::regex::icase);
  auto it_begin = std::sregex_iterator(headerText.begin(), headerText.end(), attrRegex);
  auto it_end = std::sregex_iterator();
  for (auto it = it_begin; it != it_end; ++it) {
    attrs[(*it)[1].str()] = (*it)[2].str();
  }
  return attrs;
}

size_t MdictParser::RecordIndex::bsearch(const vector<RecordIndex>& offsets, int64_t val) {
  if (offsets.empty()) return (size_t)(-1);
  auto it = std::lower_bound(offsets.begin(), offsets.end(), val);
  if (it != offsets.end() && *it == val) return std::distance(offsets.begin(), it);
  return (size_t)(-1);
}

MdictParser::MdictParser()
  : file_(nullptr), version_(0), numHeadWordBlocks_(0), headWordBlockInfoSize_(0),
    headWordBlockSize_(0), headWordBlockInfoPos_(0), headWordPos_(0), totalRecordsSize_(0),
    recordPos_(0), wordCount_(0), numberTypeSize_(0), encrypted_(0), rtl_(false) {}

MdictParser::~MdictParser() { if (file_) fclose(file_); }

ScopedMemMap::ScopedMemMap(FILE* file, int64_t offset, int64_t size)
  : file_(nullptr), mapAddr_(nullptr), hMap_(nullptr), mapSize_(0) {
  if (!file || offset < 0 || size < 0) return;
  fseek(file, 0, SEEK_END);
  int64_t fileSize = ftell(file);
  if (offset > fileSize) return;
  int64_t maxSafeSize = fileSize - offset;
  if (size > maxSafeSize) size = maxSafeSize;
  hMap_ = memmap_create(file, offset, size, &mapAddr_);
  mapSize_ = size;
}

ScopedMemMap::~ScopedMemMap() { memmap_destroy(hMap_, mapAddr_, mapSize_); }

static FILE* g_debugLog = nullptr;
#define MDICT_LOG(...) do { if (g_debugLog) { fprintf(g_debugLog, __VA_ARGS__); fflush(g_debugLog); } } while(0)

bool MdictParser::open(const char* filename) {
  // Open debug log in same directory as the MDX file
  if (!g_debugLog) {
    std::string path = std::string(filename);
    auto lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
      std::string dir = path.substr(0, lastSlash + 1);
      g_debugLog = fopen((dir + "mdict_debug.log").c_str(), "w");
    }
  }
  filename_ = filename;
  file_ = openFileWide(filename, "rb");
  if (!file_) {
    MDICT_LOG("Cannot open file: %s\n", filename);
    return false;
  }
  fseek64(file_, 0, SEEK_END);
  int64_t fileSize = ftell64(file_);
  fseek64(file_, 0, SEEK_SET);
  MDICT_LOG("Opened %s, size=%ld\n", filename, fileSize);
  if (!readHeader(file_)) {
    MDICT_LOG("readHeader FAILED\n");
    return false;
  }
  MDICT_LOG("readHeader OK: encoding=%s version=%.1f title='%s'\n", encoding_.c_str(), version_, title_.c_str());
  if (!readHeadWordBlockInfos(file_)) {
    MDICT_LOG("readHeadWordBlockInfos FAILED\n");
    return false;
  }
  MDICT_LOG("readHeadWordBlockInfos OK\n");
  if (!readRecordBlockInfos()) {
    MDICT_LOG("readRecordBlockInfos FAILED\n");
    return false;
  }
  MDICT_LOG("ALL OK: wordCount=%u records=%zu\n", wordCount_, recordBlockInfos_.size());
  return true;
}

bool MdictParser::readNextHeadWordIndex(HeadWordIndex& headWordIndex) {
  MDICT_LOG("readNHWI: iter==end? %d, blockInfos.size=%zu, headWordPos=%lld\n",
    headWordBlockInfosIter_ == headWordBlockInfos_.end(), headWordBlockInfos_.size(), headWordPos_);
  if (headWordBlockInfosIter_ == headWordBlockInfos_.end()) return false;
  int64_t compressedSize = headWordBlockInfosIter_->first;
  int64_t decompressedSize = headWordBlockInfosIter_->second;
  MDICT_LOG("readNHWI: block compressed=%lld decompressed=%lld\n", compressedSize, decompressedSize);
  if (compressedSize < 8 || decompressedSize <= 0 || decompressedSize > 100000000LL) { MDICT_LOG("readNHWI: bad block sizes\n"); return false; }

  // Read compressed data using fread instead of mmap (more portable)
  fseek64(file_, headWordPos_, SEEK_SET);
  std::vector<char> compressedBuf(compressedSize);
  size_t bytesRead = fread(compressedBuf.data(), 1, compressedSize, file_);
  if ((int64_t)bytesRead != compressedSize) {
    MDICT_LOG("readNHWI: fread FAILED read=%zu expected=%lld\n", bytesRead, compressedSize);
    return false;
  }
  MDICT_LOG("readNHWI: fread OK\n");
  headWordPos_ += compressedSize;
  std::vector<char> decompressed;
  if (!parseCompressedBlock(compressedSize, compressedBuf.data(), decompressedSize, decompressed)) {
    MDICT_LOG("readNHWI: decompress FAILED\n");
    return false;
  }
  MDICT_LOG("readNHWI: decompress OK size=%zu\n", decompressed.size());
  headWordIndex = splitHeadWordBlock(decompressed);
  MDICT_LOG("readNHWI: split OK count=%zu\n", headWordIndex.size());
  ++headWordBlockInfosIter_;
  return true;
}

bool MdictParser::checkAdler32(const char* buffer, unsigned int len, uint32_t checksum) {
  uLong adler = adler32(0L, Z_NULL, 0);
  adler = adler32(adler, (const Bytef*)buffer, len);
  return (adler & 0xFFFFFFFF) == checksum;
}

string MdictParser::toUtf16(const char* fromCode, const char* from, size_t fromSize) {
  if (!fromCode || !from) return {};
  return Iconv::toQString(fromCode, from, fromSize);
}

bool MdictParser::decryptHeadWordIndex(char* buffer, int64_t len) {
  RIPEMD128 ripemd;
  ripemd.update((const uint8_t*)buffer + 4, 4);
  ripemd.update((const uint8_t*)"\x95\x36\x00\x00", 4);
  uint8_t key[16];
  ripemd.digest(key);
  buffer += 8; len -= 8;
  uint8_t prev = 0x36;
  for (int64_t i = 0; i < len; ++i) {
    uint8_t byte = buffer[i];
    byte = (byte >> 4) | (byte << 4);
    byte = byte ^ prev ^ (uint8_t)(i & 0xFF) ^ key[i % 16];
    prev = buffer[i];
    buffer[i] = byte;
  }
  return true;
}

bool MdictParser::parseCompressedBlock(int64_t compressedBlockSize, const char* compressedBlockPtr,
                                       int64_t decompressedBlockSize, std::vector<char>& decompressedBlock) {
  if (compressedBlockSize <= 8) return false;
  uint32_t type = ((uint32_t)(uint8_t)compressedBlockPtr[0] << 24) | ((uint32_t)(uint8_t)compressedBlockPtr[1] << 16) |
                  ((uint32_t)(uint8_t)compressedBlockPtr[2] << 8) | (uint32_t)(uint8_t)compressedBlockPtr[3];
  uint32_t checksum = ((uint32_t)(uint8_t)compressedBlockPtr[4] << 24) | ((uint32_t)(uint8_t)compressedBlockPtr[5] << 16) |
                      ((uint32_t)(uint8_t)compressedBlockPtr[6] << 8) | (uint32_t)(uint8_t)compressedBlockPtr[7];
  MDICT_LOG("parseBlock: type=%08x checksum=%08x size=%lld\n", type, checksum, compressedBlockSize);
  MDICT_LOG("parseBlock: first 16 bytes:");
  for (int i = 0; i < 16 && i < compressedBlockSize; i++) MDICT_LOG(" %02x", (uint8_t)compressedBlockPtr[i]);
  MDICT_LOG("\n");
  const char* buf = compressedBlockPtr + 8;
  int64_t size = compressedBlockSize - 8;
  switch (type) {
    case 0x00000000:
      if (!checkAdler32(buf, (unsigned int)size, checksum)) return false;
      decompressedBlock.assign(buf, buf + size);
      return true;
    case 0x02000000: {
      auto result = zlibDecompress(buf, (unsigned)size, checksum);
      if (result.empty()) return false;
      decompressedBlock = std::move(result);
      return true;
    }
    default: return false;
  }
}

int64_t MdictParser::readNumber(FILE* in) {
  if (numberTypeSize_ == 8) {
    uint8_t buf[8];
    if (fread(buf, 1, 8, in) != 8) return 0;
    return ((int64_t)buf[0] << 56) | ((int64_t)buf[1] << 48) | ((int64_t)buf[2] << 40) | ((int64_t)buf[3] << 32) |
           ((int64_t)buf[4] << 24) | ((int64_t)buf[5] << 16) | ((int64_t)buf[6] << 8) | buf[7];
  } else {
    uint8_t buf[4];
    if (fread(buf, 1, 4, in) != 4) return 0;
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
  }
}

int64_t MdictParser::readNumberMem(MemReader& r) {
  if (numberTypeSize_ == 8) {
    uint8_t buf[8];
    r.read(buf, 8);
    return ((int64_t)buf[0] << 56) | ((int64_t)buf[1] << 48) | ((int64_t)buf[2] << 40) | ((int64_t)buf[3] << 32) |
           ((int64_t)buf[4] << 24) | ((int64_t)buf[5] << 16) | ((int64_t)buf[6] << 8) | buf[7];
  } else {
    uint8_t buf[4];
    r.read(buf, 4);
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
  }
}

uint32_t MdictParser::readU8OrU16(FILE* in, bool isU16) {
  if (isU16) {
    uint8_t buf[2];
    if (fread(buf, 1, 2, in) != 2) return 0;
    return ((uint32_t)buf[0] << 8) | buf[1];
  } else {
    uint8_t buf[1];
    if (fread(buf, 1, 1, in) != 1) return 0;
    return buf[0];
  }
}

uint32_t MdictParser::readU8OrU16Mem(MemReader& r, bool isU16) {
  if (isU16) {
    uint8_t buf[2];
    r.read(buf, 2);
    return ((uint32_t)buf[0] << 8) | buf[1];
  } else {
    uint8_t buf[1];
    r.read(buf, 1);
    return buf[0];
  }
}

bool MdictParser::readHeader(FILE* in) {
  uint8_t sizeBuf[4];
  if (fread(sizeBuf, 1, 4, in) != 4) { MDICT_LOG("readHeader: failed to read size\n"); return false; }
  int32_t headerTextSize = ((int32_t)sizeBuf[0] << 24) | ((int32_t)sizeBuf[1] << 16) | ((int32_t)sizeBuf[2] << 8) | sizeBuf[3];
  MDICT_LOG("readHeader: headerTextSize=%d (bytes: %02x %02x %02x %02x)\n", headerTextSize, sizeBuf[0], sizeBuf[1], sizeBuf[2], sizeBuf[3]);
  if (headerTextSize <= 0 || headerTextSize > 100000) { MDICT_LOG("readHeader: invalid headerTextSize\n"); return false; }
  std::vector<char> headerTextUtf16(headerTextSize);
  size_t bytesRead = fread(headerTextUtf16.data(), 1, headerTextSize, in);
  if (bytesRead != (size_t)headerTextSize) { MDICT_LOG("readHeader: short read %zu != %d\n", bytesRead, headerTextSize); return false; }
  MDICT_LOG("readHeader: raw header first 32 bytes:");
  for (int i = 0; i < 32 && i < headerTextSize; i++) MDICT_LOG(" %02x", (uint8_t)headerTextUtf16[i]);
  MDICT_LOG("\n");
  string headerText = toUtf16("UTF-16LE", headerTextUtf16.data(), headerTextUtf16.size());
  MDICT_LOG("readHeader: headerText size=%zu empty=%d\n", headerText.size(), headerText.empty());
  if (!headerText.empty()) MDICT_LOG("readHeader: headerText (first 200)='%s'\n", headerText.substr(0, 200).c_str());
  uint8_t checksumBuf[4];
  if (fread(checksumBuf, 1, 4, in) != 4) { MDICT_LOG("readHeader: failed to read checksum\n"); return false; }
  uint32_t checksum = ((uint32_t)checksumBuf[0]) | ((uint32_t)checksumBuf[1] << 8) |
                      ((uint32_t)checksumBuf[2] << 16) | ((uint32_t)checksumBuf[3] << 24);
  if (!checkAdler32(headerTextUtf16.data(), headerTextUtf16.size(), checksum)) {
    MDICT_LOG("readHeader: checksum mismatch\n");
    return false;
  }
  headerTextUtf16.clear();
  string styleSheets;
  map<string, string> attrs;
  try {
    if (headerText.find("StyleSheet") != string::npos) {
      std::regex rx("StyleSheet=\"([^\"]*?)\"", std::regex::icase);
      std::smatch match;
      if (std::regex_search(headerText, match, rx)) styleSheets = match[1].str();
    }
    std::regex ctrlRx("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F]");
    headerText = std::regex_replace(headerText, ctrlRx, "");
    attrs = parseHeaderAttributes(headerText);
    if (attrs.empty()) { MDICT_LOG("readHeader: attrs.empty() - header attributes not found\n"); return false; }
  } catch (const std::exception& e) {
    MDICT_LOG("readHeader: regex/parse exception: %s\n", e.what());
    return false;
  } catch (...) {
    MDICT_LOG("readHeader: unknown exception during header parsing\n");
    return false;
  }
  encoding_ = attrs.count("Encoding") ? attrs["Encoding"] : "";
  if (encoding_ == "GBK" || encoding_ == "GB2312") encoding_ = "GB18030";
  else if (encoding_.empty() || encoding_ == "UTF-16") encoding_ = "UTF-16LE";
  if (!styleSheets.empty()) {
    std::regex lineRx("[\r\n]");
    auto l_begin = std::sregex_token_iterator(styleSheets.begin(), styleSheets.end(), lineRx, -1);
    auto l_end = std::sregex_token_iterator();
    std::vector<string> lines(l_begin, l_end);
    for (size_t i = 0; i + 2 < lines.size(); i += 3) {
      try { styleSheets_[std::stoi(lines[i])] = {Html::unescapeUtf8(lines[i+1]), Html::unescapeUtf8(lines[i+2])}; }
      catch (...) {}
    }
  }
  version_ = attrs.count("GeneratedByEngineVersion") ? std::stod(attrs["GeneratedByEngineVersion"]) : 1.0;
  numberTypeSize_ = version_ >= 2.0 ? 8 : 4;
  encrypted_ = 0;
  if (attrs.count("Encrypted")) {
    auto& enc = attrs["Encrypted"];
    if (enc == "No" || enc == "no") encrypted_ = 0;
    else if (enc == "Yes" || enc == "yes") encrypted_ = 1;
    else try { encrypted_ = std::stoi(enc); } catch (...) {}
  }
  rtl_ = attrs.count("Left2Right") ? (attrs["Left2Right"] != "Yes") : true;
  string title = attrs.count("Title") ? attrs["Title"] : "";
  if (title.empty() || title == "Title (No HTML code allowed)") {
    string fn = filename_;
    // Strip directory path - only keep the base filename
    auto slash = fn.find_last_of("/\\");
    if (slash != string::npos) fn = fn.substr(slash + 1);
    auto dot = fn.rfind('.');
    title_ = (dot != string::npos) ? fn.substr(0, dot) : fn;
    // Filename may be in ANSI/GBK encoding on Windows - ensure UTF-8
    title_ = Iconv::ensureUtf8(title_);
  } else {
    if (title.find('<') != string::npos || title.find('>') != string::npos) {
      std::regex tagRx("<[^>]*>");
      title_ = std::regex_replace(title, tagRx, "");
    } else title_ = title;
  }
  description_ = attrs.count("Description") ? attrs["Description"] : "";
  // Ensure title and description are valid UTF-8
  title_ = Iconv::ensureUtf8(title_);
  description_ = Iconv::ensureUtf8(description_);
  return true;
}

bool MdictParser::readHeadWordBlockInfos(FILE* in) {
  int headerBytes = numberTypeSize_ * (version_ >= 2.0 ? 5 : 4);
  std::vector<char> header(headerBytes);
  if (fread(header.data(), 1, headerBytes, in) != (size_t)headerBytes) { MDICT_LOG("readHWBI: short read header\n"); return false; }
  MDICT_LOG("readHWBI: header bytes:");
  for (int i = 0; i < headerBytes; i++) MDICT_LOG(" %02x", (uint8_t)header[i]);
  MDICT_LOG("\n");
  MemReader stream(header.data(), headerBytes);
  numHeadWordBlocks_ = readNumberMem(stream);
  wordCount_ = readNumberMem(stream);
  MDICT_LOG("readHWBI: numBlocks=%lld wordCount=%u\n", numHeadWordBlocks_, wordCount_);
  int64_t decompressedSize = 0;
  if (version_ >= 2.0) {
    uint8_t buf[8];
    stream.read(buf, 8);
    decompressedSize = ((int64_t)buf[0]<<56)|((int64_t)buf[1]<<48)|((int64_t)buf[2]<<40)|((int64_t)buf[3]<<32)|
                       ((int64_t)buf[4]<<24)|((int64_t)buf[5]<<16)|((int64_t)buf[6]<<8)|buf[7];
    MDICT_LOG("readHWBI: decompressedSize=%lld\n", decompressedSize);
  }
  headWordBlockInfoSize_ = readNumberMem(stream);
  headWordBlockSize_ = readNumberMem(stream);
  MDICT_LOG("readHWBI: blockInfoSize=%lld blockSize=%lld\n", headWordBlockInfoSize_, headWordBlockSize_);
  if (version_ >= 2.0) {
    uint8_t checksumBuf[4];
    if (fread(checksumBuf, 1, 4, in) != 4) { MDICT_LOG("readHWBI: short read checksum\n"); return false; }
    uint32_t cs = ((uint32_t)checksumBuf[0]<<24)|((uint32_t)checksumBuf[1]<<16)|((uint32_t)checksumBuf[2]<<8)|checksumBuf[3];
    MDICT_LOG("readHWBI: checksum=%08x headerBytes=%d\n", cs, numberTypeSize_ * 5);
    if (!checkAdler32(header.data(), numberTypeSize_ * 5, cs)) {
      MDICT_LOG("readHWBI: checksum FAILED\n");
      return false;
    }
    MDICT_LOG("readHWBI: checksum OK\n");
  }
  headWordBlockInfoPos_ = ftell64(in);
  MDICT_LOG("readHWBI: blockInfoPos=%ld\n", (long)headWordBlockInfoPos_);
  std::vector<char> headWordBlockInfo(headWordBlockInfoSize_);
  size_t readBytes = fread(headWordBlockInfo.data(), 1, headWordBlockInfoSize_, in);
  if (readBytes != (size_t)headWordBlockInfoSize_) { MDICT_LOG("readHWBI: short read blockInfo %zu != %lld\n", readBytes, headWordBlockInfoSize_); return false; }
  MDICT_LOG("readHWBI: read blockInfo %lld bytes, encrypted=%d\n", headWordBlockInfoSize_, encrypted_);
  if (version_ >= 2.0) {
    if (encrypted_ & 2) {
      MDICT_LOG("readHWBI: decrypting headword index...\n");
      if (!decryptHeadWordIndex(headWordBlockInfo.data(), headWordBlockInfo.size())) {
        MDICT_LOG("readHWBI: decrypt FAILED\n");
        return false;
      }
      MDICT_LOG("readHWBI: decrypt OK\n");
    }
    std::vector<char> decompressed;
    if (!parseCompressedBlock(headWordBlockInfo.size(), headWordBlockInfo.data(), decompressedSize, decompressed)) {
      MDICT_LOG("readHWBI: decompress FAILED (compressedSize=%lld decompressedSize=%lld)\n", headWordBlockInfo.size(), decompressedSize);
      return false;
    }
    MDICT_LOG("readHWBI: decompress OK, size=%zu\n", decompressed.size());
    headWordBlockInfos_ = decodeHeadWordBlockInfo(decompressed);
  } else {
    headWordBlockInfos_ = decodeHeadWordBlockInfo(headWordBlockInfo);
  }
  headWordPos_ = ftell64(in);
  headWordBlockInfosIter_ = headWordBlockInfos_.begin();
  MDICT_LOG("readHWBI: ALL OK, numBlockInfos=%zu\n", headWordBlockInfos_.size());
  return true;
}

bool MdictParser::readRecordBlockInfos() {
  fseek64(file_, headWordBlockInfoPos_ + headWordBlockInfoSize_ + headWordBlockSize_, SEEK_SET);
  int64_t numRecordBlocks = readNumber(file_);
  readNumber(file_);
  int64_t recordInfoSize = readNumber(file_);
  totalRecordsSize_ = readNumber(file_);
  if (numRecordBlocks < 0 || numRecordBlocks > 100000000LL) {
    MDICT_LOG("readRBI: invalid numRecordBlocks=%lld\n", numRecordBlocks);
    return false;
  }
  recordPos_ = ftell64(file_) + recordInfoSize;
  recordBlockInfos_.reserve(numRecordBlocks);
  int64_t acc1 = 0, acc2 = 0;
  for (int64_t i = 0; i < numRecordBlocks; i++) {
    RecordIndex r;
    r.compressedSize = readNumber(file_);
    r.decompressedSize = readNumber(file_);
    r.startPos = acc1; r.endPos = acc1 + r.compressedSize;
    r.shadowStartPos = acc2; r.shadowEndPos = acc2 + r.decompressedSize;
    recordBlockInfos_.push_back(r);
    acc1 = r.endPos; acc2 = r.shadowEndPos;
  }
  return true;
}

MdictParser::BlockInfoVector MdictParser::decodeHeadWordBlockInfo(const std::vector<char>& headWordBlockInfo) {
  BlockInfoVector headWordBlockInfos;
  MemReader s(headWordBlockInfo.data(), headWordBlockInfo.size());
  bool isU16 = version_ >= 2.0;
  int textTermSize = isU16 ? 1 : 0;
  while (!s.eof()) {
    size_t start = s.tell();
    if (start + numberTypeSize_ > headWordBlockInfo.size()) break;
    s.seek(numberTypeSize_, SEEK_CUR);
    uint32_t textHeadSize = readU8OrU16Mem(s, isU16);
    size_t headSkip = (encoding_ != "UTF-16LE") ? (size_t)(textHeadSize + textTermSize) : (size_t)(textHeadSize + textTermSize) * 2;
    size_t pos = s.tell();
    if (pos + headSkip > headWordBlockInfo.size()) break;
    s.seek(headSkip, SEEK_CUR);
    if (s.eof()) break;
    uint32_t textTailSize = readU8OrU16Mem(s, isU16);
    size_t tailSkip = (encoding_ != "UTF-16LE") ? (size_t)(textTailSize + textTermSize) : (size_t)(textTailSize + textTermSize) * 2;
    pos = s.tell();
    if (pos + tailSkip > headWordBlockInfo.size()) break;
    s.seek(tailSkip, SEEK_CUR);
    pos = s.tell();
    if (pos + numberTypeSize_ * 2 > headWordBlockInfo.size()) break;
    int64_t compressedSize = readNumberMem(s);
    int64_t decompressedSize = readNumberMem(s);
    if (compressedSize <= 0 || decompressedSize <= 0 || decompressedSize > 100000000LL) break;
    headWordBlockInfos.emplace_back(compressedSize, decompressedSize);
  }
  return headWordBlockInfos;
}

MdictParser::HeadWordIndex MdictParser::splitHeadWordBlock(const std::vector<char>& block) {
  HeadWordIndex index;
  const char* p = block.data();
  const char* end = p + block.size();
  while (p < end) {
    if (p + numberTypeSize_ > end) break;
    int64_t headWordId = (numberTypeSize_ == 8) ?
      (((int64_t)(uint8_t)p[0] << 56) | ((int64_t)(uint8_t)p[1] << 48) | ((int64_t)(uint8_t)p[2] << 40) | ((int64_t)(uint8_t)p[3] << 32) |
       ((int64_t)(uint8_t)p[4] << 24) | ((int64_t)(uint8_t)p[5] << 16) | ((int64_t)(uint8_t)p[6] << 8) | (uint8_t)p[7]) :
      (((uint32_t)(uint8_t)p[0] << 24) | ((uint32_t)(uint8_t)p[1] << 16) | ((uint32_t)(uint8_t)p[2] << 8) | (uint8_t)p[3]);
    p += numberTypeSize_;
    std::vector<char> headWordBuf;
    if (encoding_ == "UTF-16LE") {
      int headWordLength = u16StrSize((const uint16_t*)p);
      size_t byteLen = (headWordLength + 1) * 2;
      if (p + (long)byteLen > end) break;
      headWordBuf.assign(p, p + byteLen);
    } else {
      const char* nul = (const char*)memchr(p, '\0', end - p);
      if (!nul) break;
      size_t headWordLength = nul - p;
      headWordBuf.assign(p, p + headWordLength + 1);
    }
    p += headWordBuf.size();
    string headWord = toUtf16(encoding_.c_str(), headWordBuf.data(), headWordBuf.size());
    // Strip trailing null bytes (UTF-16LE null terminator)
    while (!headWord.empty() && headWord.back() == '\0')
      headWord.pop_back();
    index.emplace_back(headWordId, headWord);
  }
  return index;
}

bool MdictParser::readRecordBlock(HeadWordIndex& headWordIndex, RecordHandler& recordHandler) {
  size_t idx = 0;
  for (auto i = headWordIndex.begin(); i != headWordIndex.end(); ++i) {
    if (recordBlockInfos_[idx].shadowEndPos <= i->first)
      idx = RecordIndex::bsearch(recordBlockInfos_, i->first);
    if (idx == (size_t)(-1)) return false;
    const RecordIndex& recordIndex = recordBlockInfos_[idx];
    auto iNext = i + 1;
    int64_t recordSize = (iNext == headWordIndex.end()) ? recordIndex.shadowEndPos - i->first : iNext->first - i->first;
    RecordInfo recordInfo;
    recordInfo.compressedBlockPos = recordPos_ + recordIndex.startPos;
    recordInfo.recordOffset = i->first - recordIndex.shadowStartPos;
    recordInfo.decompressedBlockSize = recordIndex.decompressedSize;
    recordInfo.compressedBlockSize = recordIndex.compressedSize;
    recordInfo.recordSize = recordSize;
    recordHandler.handleRecord(i->second, recordInfo);
  }
  return true;
}

bool MdictParser::readRecordById(HeadWordIndex& fullHeadWordIndex, int64_t targetId, std::vector<char>& outData) {
  if (fullHeadWordIndex.empty()) return false;

  size_t idx = 0;
  for (size_t i = 0; i < fullHeadWordIndex.size(); i++) {
    if (fullHeadWordIndex[i].first == targetId) { idx = i; break; }
  }

  if (recordBlockInfos_[idx].shadowEndPos <= fullHeadWordIndex[idx].first)
    idx = RecordIndex::bsearch(recordBlockInfos_, fullHeadWordIndex[idx].first);
  if (idx == (size_t)(-1)) return false;

  const RecordIndex& ri = recordBlockInfos_[idx];
  int64_t nextPos = (idx + 1 < fullHeadWordIndex.size()) ? fullHeadWordIndex[idx + 1].first : ri.shadowEndPos;
  int64_t recordSize = nextPos - fullHeadWordIndex[idx].first;

  FILE* f = openFileWide(filename_.c_str(), "rb");
  if (!f) return false;
  fseek64(f, recordPos_ + ri.startPos, SEEK_SET);
  std::vector<char> compressed(ri.compressedSize);
  size_t bytesRead = fread(compressed.data(), 1, ri.compressedSize, f);
  fclose(f);
  if ((int64_t)bytesRead != ri.compressedSize) return false;

  std::vector<char> decompressed;
  if (!parseCompressedBlock(ri.compressedSize, compressed.data(), ri.decompressedSize, decompressed))
    return false;

  int64_t recordOffset = fullHeadWordIndex[idx].first - ri.shadowStartPos;
  if (recordOffset >= 0 && recordOffset < (int64_t)decompressed.size()) {
    size_t len = (size_t)recordSize;
    size_t avail = decompressed.size() - (size_t)recordOffset;
    if (len > avail) len = avail;
    outData.assign(decompressed.data() + recordOffset, decompressed.data() + recordOffset + len);
    return true;
  }
  return false;
}

static string rstripnull(const string& str) {
  size_t n = str.size();
  while (n > 0 && (str[n-1] == ' ' || str[n-1] == '\0')) n--;
  return str.substr(0, n);
}

string& MdictParser::substituteStylesheet(string& article, const StyleSheets& styleSheets) {
  std::regex rx("`([0-9]+)`");
  string articleNewText;
  string endStyle;
  auto rx_begin = std::sregex_iterator(article.begin(), article.end(), rx);
  auto rx_end = std::sregex_iterator();
  size_t pos = 0;
  for (auto it = rx_begin; it != rx_end; ++it) {
    int styleId = 0;
    try { styleId = std::stoi((*it)[1].str()); } catch (...) { continue; }
    articleNewText += article.substr(pos, (*it).position() - pos);
    pos = (*it).position() + (*it)[0].str().size();
    auto iter = styleSheets.find(styleId);
    if (iter != styleSheets.end()) {
      articleNewText += endStyle + iter->second.first;
      endStyle = iter->second.second;
    } else { articleNewText += endStyle; endStyle.clear(); }
  }
  if (pos) {
    articleNewText += rstripnull(article.substr(pos));
    article = articleNewText;
    articleNewText.clear();
  }
  article += endStyle;
  return article;
}
