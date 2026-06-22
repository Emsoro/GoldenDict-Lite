#include "dictionary_manager.hpp"
#include "goldendict/mdictparser.hh"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <filesystem>

DictionaryManager::DictionaryManager() {}
DictionaryManager::~DictionaryManager() { clear(); }

void DictionaryManager::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  dicts_.clear();
}

std::string DictionaryManager::loadDict(const char* mdxPath, DictEntry& entry) {
  Mdict::MdictParser parser;
  if (!parser.open(mdxPath)) return "MdictParser::open failed for: " + std::string(mdxPath);

  entry.title = parser.title();
  entry.description = parser.description();
  entry.encoding = parser.encoding();
  entry.wordCount = parser.wordCount();
  entry.filePath = mdxPath;

  // Read ALL headwords
  Mdict::MdictParser::HeadWordIndex block;
  while (parser.readNextHeadWordIndex(block)) {
    for (auto& hw : block) {
      DictEntry::HeadWord h;
      h.id = hw.first;
      h.word = hw.second;
      entry.headwords.push_back(std::move(h));
    }
    block.clear();
  }

  // Cache record block info (small: ~1.3MB per dict)
  entry.recordBlockInfos = parser.getRecordBlockInfos();
  entry.recordPos = parser.getRecordPos();

  // Load CSS from same directory
  std::string dir(mdxPath);
  std::string baseName;
  auto pos = dir.find_last_of("/\\");
  if (pos != std::string::npos) {
    baseName = dir.substr(pos + 1);
    dir = dir.substr(0, pos + 1);
  }
  auto dotPos = baseName.rfind('.');
  if (dotPos != std::string::npos) baseName = baseName.substr(0, dotPos);
  std::string cssPath = dir + baseName + ".css";
  FILE* cssFile = fopen(cssPath.c_str(), "rb");
  if (cssFile) {
    fseek(cssFile, 0, SEEK_END);
    long cssSize = ftell(cssFile);
    fseek(cssFile, 0, SEEK_SET);
    entry.cssContent.resize(cssSize);
    fread(&entry.cssContent[0], 1, cssSize, cssFile);
    fclose(cssFile);
  }

  // Load icon PNG as base64
  entry.iconBase64 = loadIconBase64(mdxPath);
  entry.dictName = entry.title;

  return "";
}

std::string DictionaryManager::loadIconBase64(const std::string& mdxPath) {
  // Find PNG with same base name
  std::string pngPath = mdxPath;
  auto dotPos = pngPath.rfind('.');
  if (dotPos != std::string::npos) pngPath = pngPath.substr(0, dotPos);
  pngPath += ".png";
  FILE* f = fopen(pngPath.c_str(), "rb");
  if (!f) return "";
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 500000) { fclose(f); return ""; }
  std::vector<unsigned char> data(sz);
  fread(data.data(), 1, sz, f);
  fclose(f);
  // Convert to base64
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((sz + 2) / 3) * 4);
  for (long i = 0; i < sz; i += 3) {
    unsigned int n = ((unsigned int)data[i]) << 16;
    if (i + 1 < sz) n |= ((unsigned int)data[i + 1]) << 8;
    if (i + 2 < sz) n |= ((unsigned int)data[i + 2]);
    result += tbl[(n >> 18) & 0x3F];
    result += tbl[(n >> 12) & 0x3F];
    result += (i + 1 < sz) ? tbl[(n >> 6) & 0x3F] : '=';
    result += (i + 2 < sz) ? tbl[n & 0x3F] : '=';
  }
  return "data:image/png;base64," + result;
}

bool DictionaryManager::loadDictionary(const char* mdxPath) {
  return loadDictionaryEx(mdxPath).empty();
}

std::string DictionaryManager::loadDictionaryEx(const char* mdxPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  DictEntry entry;
  std::string error = loadDict(mdxPath, entry);
  if (!error.empty()) return error;
  dicts_.push_back(std::move(entry));
  return "";
}

size_t DictionaryManager::loadDictionaryDir(const char* dirPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t loaded = 0;
  try {
    for (auto& entry : std::filesystem::directory_iterator(dirPath)) {
      if (entry.is_regular_file()) {
        auto& p = entry.path();
        if (p.extension() == ".mdx" || p.extension() == ".MDX") {
          DictEntry dictEntry;
          std::string error = loadDict(p.string().c_str(), dictEntry);
          if (error.empty()) {
            dicts_.push_back(std::move(dictEntry));
            loaded++;
          }
        }
      }
    }
  } catch (...) {}
  return loaded;
}

std::vector<std::string> DictionaryManager::prefixMatch(const std::string& prefix, size_t maxResults) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> results;
  std::string lowerPrefix = prefix;
  std::transform(lowerPrefix.begin(), lowerPrefix.end(), lowerPrefix.begin(), ::tolower);
  for (auto& dict : dicts_) {
    for (auto& hw : dict.headwords) {
      if (results.size() >= maxResults) break;
      std::string lowerWord = hw.word;
      std::transform(lowerWord.begin(), lowerWord.end(), lowerWord.begin(), ::tolower);
      if (lowerWord.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
        if (std::find(results.begin(), results.end(), hw.word) == results.end())
          results.push_back(hw.word);
      }
    }
  }
  return results;
}

std::string DictionaryManager::lookup(const std::string& word) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string allResults;
  int matchCount = 0;

  for (size_t di = 0; di < dicts_.size(); di++) {
    auto& dict = dicts_[di];

    int64_t targetId = -1;
    for (auto& hw : dict.headwords) {
      if (hw.word == word) { targetId = hw.id; break; }
    }
    if (targetId < 0) continue;

    Mdict::MdictParser parser;
    if (!parser.open(dict.filePath.c_str())) continue;
    Mdict::MdictParser::HeadWordIndex dummy;
    while (parser.readNextHeadWordIndex(dummy)) dummy.clear();

    Mdict::MdictParser::HeadWordIndex fullIndex;
    fullIndex.reserve(dict.headwords.size());
    for (auto& hw : dict.headwords)
      fullIndex.emplace_back(hw.id, hw.word);

    struct ArticleHandler : public Mdict::MdictParser::RecordHandler {
      const std::string& targetWord;
      std::string& result;
      const std::string& filePath;
      bool found;
      ArticleHandler(const std::string& w, std::string& r, const std::string& fp)
        : targetWord(w), result(r), filePath(fp), found(false) {}
      void handleRecord(const std::string& name, const Mdict::MdictParser::RecordInfo& info) override {
        if (found) return;
        if (name == targetWord) {
          found = true;
          FILE* f = fopen(filePath.c_str(), "rb");
          if (!f) return;
          fseek(f, info.compressedBlockPos, SEEK_SET);
          std::vector<char> compressedData(info.compressedBlockSize);
          size_t bytesRead = fread(compressedData.data(), 1, info.compressedBlockSize, f);
          fclose(f);
          if (bytesRead != (size_t)info.compressedBlockSize) return;
          std::vector<char> decompressed;
          if (!Mdict::MdictParser::parseCompressedBlock(info.compressedBlockSize, compressedData.data(),
              info.decompressedBlockSize, decompressed)) return;
          if (info.recordOffset >= 0 && info.recordOffset < (int64_t)decompressed.size()) {
            size_t len = (size_t)info.recordSize;
            size_t avail = decompressed.size() - (size_t)info.recordOffset;
            if (len > avail) len = avail;
            result = std::string(decompressed.data() + info.recordOffset, len);
          }
        }
      }
    };

    std::string article;
    ArticleHandler handler(word, article, dict.filePath);
    parser.readRecordBlock(fullIndex, handler);

    if (!article.empty()) {
      auto& stylesheets = parser.styleSheets();
      if (!stylesheets.empty())
        Mdict::MdictParser::substituteStylesheet(article, stylesheets);
      matchCount++;
      allResults += "<div class=\"dict-section\">";
      std::string nameHtml;
      if (!dict.iconBase64.empty())
        nameHtml += "<img class=\"dict-icon\" src=\"" + dict.iconBase64 + "\">";
      nameHtml += dict.title;
      allResults += "<div class=\"dict-name\">" + nameHtml + "</div>";
      allResults += "<div class=\"dict-article\"><style>" + dict.cssContent + "</style>" + article + "</div>";
      allResults += "</div>";
    }
  }

  if (matchCount == 0)
    return "<p>Word not found: " + word + "</p>";
  return allResults;
}

std::string DictionaryManager::getTitle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (dicts_.empty()) return "";
  return dicts_.front().title;
}

std::string DictionaryManager::getDescription() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (dicts_.empty()) return "";
  return dicts_.front().description;
}

size_t DictionaryManager::getWordCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t total = 0;
  for (auto& dict : dicts_) total += dict.headwords.size();
  return total;
}

size_t DictionaryManager::getDictCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dicts_.size();
}

nlohmann::json DictionaryManager::getDictList() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json list = nlohmann::json::array();
  for (auto& dict : dicts_) {
    list.push_back({
      {"name", dict.title},
      {"icon", dict.iconBase64},
      {"word_count", dict.headwords.size()}
    });
  }
  return list;
}
