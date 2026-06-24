#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include "goldendict/mdictparser.hh"
#include "dict_cache.hpp"

class DictionaryManager {
public:
  using ProgressCallback = std::function<void(const std::string& dictName, size_t current, size_t total, bool cached)>;
  DictionaryManager();
  ~DictionaryManager();
  bool loadDictionary(const char* mdxPath);
  size_t loadDictionaryDir(const char* dirPath);
  void finalizeLoading();
  std::string loadDictionaryEx(const char* mdxPath);
  void setProgressCallback(ProgressCallback cb) { progressCallback_ = std::move(cb); }
  void clear();

  std::string getProgressDict() const { return progressDict_; }
  size_t getProgressCurrent() const { return progressCurrent_; }
  size_t getProgressTotal() const { return progressTotal_; }
  bool getProgressCached() const { return progressCached_; }
  std::vector<std::string> prefixMatch(const std::string& prefix, size_t maxResults = 20);
  std::string lookup(const std::string& word);
  std::string lookupResource(const std::string& dictTitle, const std::string& resourcePath);
  std::vector<std::string> searchMddHeadwords(const std::string& dictTitle, const std::string& pattern);
  std::string getTitle() const;
  std::string getDescription() const;
  size_t getWordCount() const;
  size_t getDictCount() const;
  nlohmann::json getDictList() const;
  void setDictOrder(const std::vector<std::string>& order);
  std::vector<std::string> getDictOrder() const;

private:
  std::string lookupResourceLocked(const std::string& dictTitle, const std::string& resourcePath);

  struct DictEntry {
    std::string title;
    std::string description;
    std::string encoding;
    uint32_t wordCount = 0;
    std::string filePath;
    std::string cssContent;
    std::string iconBase64;
    std::string dictName;

    int64_t recordPos = 0;
    Mdict::MdictParser::StyleSheets styleSheets;

    // MDD resource data (supports multi-volume MDD)
    struct MddVolume {
      std::string filePath;
      int64_t recordPos = 0;
      std::unique_ptr<DictCache> cache;
    };
    std::vector<MddVolume> mddVolumes;

    std::unique_ptr<DictCache> cache;
  };
  std::vector<DictEntry> dicts_;
  std::vector<std::string> dictOrder_;
  mutable std::mutex mutex_;
  ProgressCallback progressCallback_;
  std::string progressDict_;
  size_t progressCurrent_ = 0;
  size_t progressTotal_ = 0;
  bool progressCached_ = false;
  std::string loadDict(const char* mdxPath, DictEntry& entry);
  std::string loadIconBase64(const std::string& mdxPath);
  static std::string getCachePath(const std::string& mdxPath);
  std::string readResourceFromVolume(const DictEntry::MddVolume& vol, const DictCache::RecordLookup& rl);
};
