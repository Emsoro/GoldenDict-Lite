#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include <utility>
#include <sqlite3.h>
#include "goldendict/mdictparser.hh"

class DictCache {
public:
  DictCache();
  ~DictCache();
  DictCache(const DictCache&) = delete;
  DictCache& operator=(const DictCache&) = delete;

  bool open(const char* cachePath, int64_t mdxFileSize);
  void close();

  bool isValid() const { return valid_; }
  int64_t cachedFileSize() const { return cachedFileSize_; }
  sqlite3* getDb() const { return db_; }

  struct CachedHeadWord {
    int64_t id;
    std::string word;
  };

  std::vector<std::string> prefixMatchWords(const std::string& lowerPrefix, size_t maxResults);

  struct RecordLookup {
    int64_t compressedBlockPos;
    int64_t decompressedBlockSize;
    int64_t compressedBlockSize;
    int64_t recordOffset;
    int64_t recordSize;
  };
  bool lookupRecord(const std::string& word, int64_t recordPos, RecordLookup& out);
  bool lookupRecordLike(const std::string& filename, int64_t recordPos, RecordLookup& out);
  std::vector<std::string> searchHeadwords(const std::string& pattern, size_t maxResults = 20);

  struct CachedRecordBlock {
    int64_t startPos, endPos, shadowStartPos, shadowEndPos;
    int64_t compressedSize, decompressedSize;
  };
  int64_t loadRecordPos();
  std::string loadMeta(const std::string& key);
  Mdict::MdictParser::StyleSheets loadStyleSheets();

  bool saveIndex(const std::string& title, const std::string& description,
                 const std::string& encoding, uint32_t wordCount, int64_t recordPos,
                 const std::vector<CachedHeadWord>& headwords,
                 const std::vector<CachedRecordBlock>& recordBlocks,
                 const Mdict::MdictParser::StyleSheets& styleSheets);

private:
  bool ensureSchema();
  bool exec(const char* sql);
  sqlite3* db_ = nullptr;
  bool valid_ = false;
  int64_t cachedFileSize_ = 0;
  int64_t mdxFileSize_ = 0;
};
