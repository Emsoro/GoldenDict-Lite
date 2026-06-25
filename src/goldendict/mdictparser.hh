#pragma once
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Mdict {
using std::string;
using std::vector;
using std::pair;
using std::map;

class MemReader {
  const char* data_;
  size_t size_;
  size_t pos_;
public:
  MemReader(const char* data, size_t size) : data_(data), size_(size), pos_(0) {}
  size_t read(void* buf, size_t count) {
    size_t avail = size_ - pos_;
    size_t toRead = (count < avail) ? count : avail;
    memcpy(buf, data_ + pos_, toRead);
    pos_ += toRead;
    return toRead;
  }
  void seek(long offset, int origin) {
    if (origin == SEEK_SET) pos_ = (offset >= 0) ? (size_t)offset : 0;
    else if (origin == SEEK_CUR) {
      if (offset >= 0) pos_ += (size_t)offset;
      else if (pos_ >= (size_t)(-offset)) pos_ -= (size_t)(-offset);
      else pos_ = 0;
    }
    else if (origin == SEEK_END) {
      if (offset <= 0 && size_ >= (size_t)(-offset)) pos_ = size_ + offset;
      else pos_ = size_;
    }
  }
  size_t tell() const { return pos_; }
  bool eof() const { return pos_ >= size_; }
};

class ScopedMemMap {
  FILE* file_;
  void* mapAddr_;
  void* hMap_;
  size_t mapSize_;
public:
  ScopedMemMap(FILE* file, int64_t offset, int64_t size);
  ~ScopedMemMap();
  uint8_t* startAddress() const { return (uint8_t*)mapAddr_; }
  ScopedMemMap(const ScopedMemMap&) = delete;
  ScopedMemMap& operator=(const ScopedMemMap&) = delete;
};

class MdictParser {
public:
  enum { kParserVersion = 0x000000d };
  struct RecordIndex {
    int64_t startPos, endPos, shadowStartPos, shadowEndPos;
    int64_t compressedSize, decompressedSize;
    bool operator==(int64_t rhs) const { return shadowStartPos <= rhs && rhs < shadowEndPos; }
    bool operator<(int64_t rhs) const { return shadowEndPos <= rhs; }
    bool operator>(int64_t rhs) const { return shadowStartPos > rhs; }
    static size_t bsearch(const vector<RecordIndex>& offsets, int64_t val);
  };
  struct RecordInfo {
    int64_t compressedBlockPos, recordOffset;
    int64_t decompressedBlockSize, compressedBlockSize, recordSize;
  };
  class RecordHandler {
  public:
    virtual void handleRecord(const string& name, const RecordInfo& recordInfo) = 0;
    virtual ~RecordHandler() = default;
  };
  using BlockInfoVector = vector<pair<int64_t, int64_t>>;
  using HeadWordIndex = vector<pair<int64_t, string>>;
  using StyleSheets = map<int32_t, pair<string, string>>;

  const string& title() const { return title_; }
  const string& description() const { return description_; }
  const StyleSheets& styleSheets() const { return styleSheets_; }
  uint32_t wordCount() const { return wordCount_; }
  const string& encoding() const { return encoding_; }
  const string& filename() const { return filename_; }
  bool isRightToLeft() const { return rtl_; }

  MdictParser();
  ~MdictParser();
  bool open(const char* filename);
  bool readNextHeadWordIndex(HeadWordIndex& headWordIndex);
  bool readRecordBlock(HeadWordIndex& headWordIndex, RecordHandler& recordHandler);

  // Direct record access: read a single record by headword ID
  // Returns true if the record was found and written to outData
  bool readRecordById(HeadWordIndex& fullHeadWordIndex, int64_t targetId, std::vector<char>& outData);

  // Access record block info for caching
  const vector<RecordIndex>& getRecordBlockInfos() const { return recordBlockInfos_; }
  int64_t getRecordPos() const { return recordPos_; }

  static string toUtf16(const char* fromCode, const char* from, size_t fromSize);
  static bool parseCompressedBlock(int64_t compressedBlockSize, const char* compressedBlockPtr,
                                   int64_t decompressedBlockSize, std::vector<char>& decompressedBlock);
  static string& substituteStylesheet(string& article, const StyleSheets& styleSheets);

protected:
  int64_t readNumber(FILE* in);
  int64_t readNumberMem(MemReader& r);
  static uint32_t readU8OrU16(FILE* in, bool isU16);
  static uint32_t readU8OrU16Mem(MemReader& r, bool isU16);
  static bool checkAdler32(const char* buffer, unsigned int len, uint32_t checksum);
  static bool decryptHeadWordIndex(char* buffer, int64_t len);
  bool readHeader(FILE* in);
  bool readHeadWordBlockInfos(FILE* in);
  bool readRecordBlockInfos();
  BlockInfoVector decodeHeadWordBlockInfo(const std::vector<char>& headWordBlockInfo);
  HeadWordIndex splitHeadWordBlock(const std::vector<char>& block);

  string filename_;
  FILE* file_;
  StyleSheets styleSheets_;
  BlockInfoVector headWordBlockInfos_;
  BlockInfoVector::iterator headWordBlockInfosIter_;
  vector<RecordIndex> recordBlockInfos_;
  string encoding_, title_, description_;
  double version_;
  int64_t numHeadWordBlocks_, headWordBlockInfoSize_, headWordBlockSize_;
  int64_t headWordBlockInfoPos_, headWordPos_, totalRecordsSize_, recordPos_;
  uint32_t wordCount_;
  int numberTypeSize_, encrypted_;
  bool rtl_;
};
}
