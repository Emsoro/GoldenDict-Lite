#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "goldendict/mdictparser.hh"

class DictionaryManager {
public:
  DictionaryManager();
  ~DictionaryManager();
  bool loadDictionary(const char* mdxPath);
  size_t loadDictionaryDir(const char* dirPath);
  std::string loadDictionaryEx(const char* mdxPath);
  void clear();
  std::vector<std::string> prefixMatch(const std::string& prefix, size_t maxResults = 20);
  std::string lookup(const std::string& word);
  std::string getTitle() const;
  std::string getDescription() const;
  size_t getWordCount() const;
  size_t getDictCount() const;
  nlohmann::json getDictList() const;

private:
  struct DictEntry {
    std::string title;
    std::string description;
    std::string encoding;
    uint32_t wordCount;
    std::string filePath;
    std::string cssContent;

    struct HeadWord {
      int64_t id;
      std::string word;
    };
    std::vector<HeadWord> headwords;  // for prefix matching

    // Cached record block info (small: ~27K entries * 48 bytes ≈ 1.3MB)
    std::vector<Mdict::MdictParser::RecordIndex> recordBlockInfos;
    int64_t recordPos;  // file position of compressed record data
    std::string iconBase64;  // PNG icon as base64 data URL
    std::string dictName;    // display name for the dict
  };
  std::vector<DictEntry> dicts_;
  mutable std::mutex mutex_;
  std::string loadDict(const char* mdxPath, DictEntry& entry);
  std::string loadIconBase64(const std::string& mdxPath);
};
