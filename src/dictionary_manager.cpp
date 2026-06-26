#include "dictionary_manager.hpp"
#include "goldendict/mdictparser.hh"
#include "goldendict/iconv.hh"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <regex>
#ifdef _WIN32
#include <Windows.h>
#endif

// Helper: open file with Chinese path support on Windows
// Uses CP_ACP because std::filesystem::path::string() returns ANSI-encoded strings on Windows
static FILE* openFileWide(const std::string& path, const char* mode) {
#ifdef _WIN32
  int wlen = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
  if (wlen <= 0) return nullptr;
  std::wstring wpath(wlen, 0);
  MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wpath[0], wlen);
  std::wstring wmode;
  while (*mode) wmode += (wchar_t)*mode++;
  return _wfopen(wpath.c_str(), wmode.c_str());
#else
  return fopen(path.c_str(), mode);
#endif
}

// Helper: fseek with 64-bit offset on Windows
static int fseek64(FILE* f, int64_t offset, int origin) {
#ifdef _WIN32
  return _fseeki64(f, offset, origin);
#else
  return fseeko(f, offset, origin);
#endif
}

DictionaryManager::DictionaryManager() {}
DictionaryManager::~DictionaryManager() { clear(); }

void DictionaryManager::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  dicts_.clear();
}

std::string DictionaryManager::getCachePath(const std::string& mdxPath) {
  auto dotPos = mdxPath.rfind('.');
  if (dotPos != std::string::npos)
    return mdxPath.substr(0, dotPos) + ".cache.db";
  return mdxPath + ".cache.db";
}

std::string DictionaryManager::loadDict(const char* mdxPath, DictEntry& entry) {
  entry.filePath = mdxPath;

  std::string cachePath = getCachePath(mdxPath);

  int64_t mdxFileSize = 0;
  try { mdxFileSize = (int64_t)std::filesystem::file_size(mdxPath); }
  catch (...) { return "Cannot stat file: " + std::string(mdxPath); }

  entry.cache = std::make_unique<DictCache>();
  entry.cache->open(cachePath.c_str(), mdxFileSize);

  if (entry.cache->isValid()) {
    entry.title = Iconv::ensureUtf8(entry.cache->loadMeta("title"));
    entry.description = Iconv::ensureUtf8(entry.cache->loadMeta("description"));
    entry.encoding = entry.cache->loadMeta("encoding");
    entry.wordCount = (uint32_t)strtoll(entry.cache->loadMeta("word_count").c_str(), nullptr, 10);
    entry.recordPos = entry.cache->loadRecordPos();
    entry.styleSheets = entry.cache->loadStyleSheets();
  } else {
    Mdict::MdictParser parser;
    try {
      if (!parser.open(mdxPath)) return "MdictParser::open failed for: " + std::string(mdxPath);
    } catch (const std::exception& e) {
      return std::string("MdictParser::open exception: ") + e.what();
    } catch (...) {
      return "MdictParser::open unknown exception";
    }

    entry.title = Iconv::ensureUtf8(parser.title());
    entry.description = Iconv::ensureUtf8(parser.description());
    entry.encoding = parser.encoding();
    entry.wordCount = parser.wordCount();

    std::vector<DictCache::CachedHeadWord> headwords;
    try {
      Mdict::MdictParser::HeadWordIndex block;
      while (parser.readNextHeadWordIndex(block)) {
        for (auto& hw : block)
          headwords.push_back({hw.first, hw.second});
        block.clear();
      }
    } catch (...) {
      return "Failed reading headword index: " + std::string(mdxPath);
    }

    auto& parserBlocks = parser.getRecordBlockInfos();
    entry.recordPos = parser.getRecordPos();

    std::vector<DictCache::CachedRecordBlock> cachedBlocks;
    cachedBlocks.reserve(parserBlocks.size());
    for (auto& rb : parserBlocks)
      cachedBlocks.push_back({rb.startPos, rb.endPos, rb.shadowStartPos, rb.shadowEndPos,
                              rb.compressedSize, rb.decompressedSize});

    entry.cache->close();
    entry.cache->open(cachePath.c_str(), mdxFileSize);
    entry.cache->saveIndex(entry.title, entry.description, entry.encoding,
                           entry.wordCount, entry.recordPos, headwords, cachedBlocks,
                           parser.styleSheets());
    entry.styleSheets = parser.styleSheets();
  }

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
  FILE* cssFile = openFileWide(cssPath, "rb");
  if (cssFile) {
    fseek64(cssFile, 0, SEEK_END);
    long cssSize = ftell(cssFile);
    fseek64(cssFile, 0, SEEK_SET);
    entry.cssContent.resize(cssSize);
    fread(&entry.cssContent[0], 1, cssSize, cssFile);
    fclose(cssFile);
  }

  entry.iconBase64 = loadIconBase64(mdxPath);
  entry.dictName = entry.title;

  // Load MDD resource files (main + multi-volume: .mdd, .1.mdd, .2.mdd, ...)
  try {
    std::string basePath = mdxPath;
    auto dp = basePath.rfind('.');
    if (dp != std::string::npos) basePath = basePath.substr(0, dp);

    // Collect all MDD file paths: main .mdd, then .1.mdd, .2.mdd, etc.
    std::vector<std::string> mddPaths;
    std::string mainMdd = basePath + ".mdd";
    if (std::filesystem::exists(mainMdd))
      mddPaths.push_back(mainMdd);
    for (int i = 1; i <= 20; i++) {
      std::string volMdd = basePath + "." + std::to_string(i) + ".mdd";
      if (std::filesystem::exists(volMdd))
        mddPaths.push_back(volMdd);
      else
        break;
    }

    for (auto& mddPath : mddPaths) {
      int64_t mddFileSize = 0;
      try { mddFileSize = (int64_t)std::filesystem::file_size(mddPath); }
      catch (...) { mddFileSize = 0; }
      if (mddFileSize == 0) continue;

      // Generate unique cache path for each MDD volume
      std::string mddCachePath = getCachePath(mddPath);
      {
        auto mddDot = mddCachePath.rfind('.');
        if (mddDot != std::string::npos)
          mddCachePath = mddCachePath.substr(0, mddDot) + ".mdd.cache.db";
        else
          mddCachePath += ".mdd";
      }

      DictEntry::MddVolume vol;
      vol.filePath = mddPath;
      vol.cache = std::make_unique<DictCache>();
      vol.cache->open(mddCachePath.c_str(), mddFileSize);

      if (!vol.cache->isValid()) {
        Mdict::MdictParser mddParser;
        if (mddParser.open(mddPath.c_str())) {
          std::vector<DictCache::CachedHeadWord> mddHeadwords;
          Mdict::MdictParser::HeadWordIndex block;
          while (mddParser.readNextHeadWordIndex(block)) {
            for (auto& hw : block)
              mddHeadwords.push_back({hw.first, hw.second});
            block.clear();
          }

          auto& mddBlocks = mddParser.getRecordBlockInfos();
          vol.recordPos = mddParser.getRecordPos();

          std::vector<DictCache::CachedRecordBlock> cachedBlocks;
          cachedBlocks.reserve(mddBlocks.size());
          for (auto& rb : mddBlocks)
            cachedBlocks.push_back({rb.startPos, rb.endPos, rb.shadowStartPos, rb.shadowEndPos,
                                    rb.compressedSize, rb.decompressedSize});

          vol.cache->close();
          vol.cache->open(mddCachePath.c_str(), mddFileSize);
          vol.cache->saveIndex(mddParser.title(), mddParser.description(), mddParser.encoding(),
                               mddParser.wordCount(), vol.recordPos, mddHeadwords, cachedBlocks,
                               mddParser.styleSheets());
        }
      } else {
        vol.recordPos = vol.cache->loadRecordPos();
      }

      entry.mddVolumes.push_back(std::move(vol));
    }
  } catch (...) {
    // MDD loading failed - continue without MDD resources
    entry.mddVolumes.clear();
  }

  // Resolve CSS url() references by loading resources from MDD
  if (!entry.cssContent.empty() && !entry.mddVolumes.empty()) {
    static const std::regex urlRx("url\\(([^)]+)\\)", std::regex::icase);
    std::string css = entry.cssContent;
    std::string result;
    size_t lastEnd = 0;
    auto it_begin = std::sregex_iterator(css.begin(), css.end(), urlRx);
    auto it_end = std::sregex_iterator();
    for (auto it = it_begin; it != it_end; ++it) {
      result += css.substr(lastEnd, it->position() - lastEnd);
      std::string url = (*it)[1].str();
      // Strip quotes
      if (!url.empty() && (url[0] == '\'' || url[0] == '"'))
        url = url.substr(1, url.size() - 2);
      // Skip data: URLs and http(s) URLs
      if (url.compare(0, 5, "data:") == 0 || url.compare(0, 4, "http") == 0) {
        result += (*it)[0].str();
      } else {
        // Try to load from MDD
        std::string base64;
        for (auto& vol : entry.mddVolumes) {
          if (!vol.cache || !vol.cache->isValid()) continue;
          DictCache::RecordLookup rl;
          // Try with leading backslash
          std::string path = "\\" + url;
          if (vol.cache->lookupRecord(path, vol.recordPos, rl)) {
            base64 = readResourceFromVolume(vol, rl);
            break;
          }
          if (vol.cache->lookupRecord(url, vol.recordPos, rl)) {
            base64 = readResourceFromVolume(vol, rl);
            break;
          }
        }
        if (!base64.empty()) {
          // Determine MIME type from extension
          std::string mime = "image/png";
          auto dot = url.rfind('.');
          if (dot != std::string::npos) {
            std::string ext = url.substr(dot + 1);
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == "jpg" || ext == "jpeg") mime = "image/jpeg";
            else if (ext == "gif") mime = "image/gif";
            else if (ext == "svg") mime = "image/svg+xml";
          }
          result += "url(data:" + mime + ";base64," + base64 + ")";
        } else {
          result += (*it)[0].str();
        }
      }
      lastEnd = it->position() + (*it)[0].str().size();
    }
    result += css.substr(lastEnd);
    entry.cssContent = std::move(result);
  }

  return "";
}

std::string DictionaryManager::loadIconBase64(const std::string& mdxPath) {
  std::string pngPath = mdxPath;
  auto dotPos = pngPath.rfind('.');
  if (dotPos != std::string::npos) pngPath = pngPath.substr(0, dotPos);
  pngPath += ".png";
  FILE* f = openFileWide(pngPath, "rb");
  if (!f) return "";
  fseek64(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek64(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 500000) { fclose(f); return ""; }
  std::vector<unsigned char> data(sz);
  fread(data.data(), 1, sz, f);
  fclose(f);
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

  // Skip if directory doesn't exist
  try {
    if (!std::filesystem::exists(dirPath)) return 0;
  } catch (...) {
    return 0;
  }

  size_t loaded = 0;

  std::vector<std::string> mdxFiles;
  try {
    for (auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
      if (entry.is_regular_file()) {
        auto& p = entry.path();
        if (p.extension() == ".mdx" || p.extension() == ".MDX") {
          mdxFiles.push_back(p.string());
        }
      }
    }
  } catch (...) {}

  for (size_t i = 0; i < mdxFiles.size(); i++) {
    std::string baseName = mdxFiles[i];
    auto slashPos = baseName.find_last_of("/\\");
    if (slashPos != std::string::npos) baseName = baseName.substr(slashPos + 1);
    auto dotPos = baseName.rfind('.');
    if (dotPos != std::string::npos) baseName = baseName.substr(0, dotPos);

    std::string cachePath = getCachePath(mdxFiles[i]);
    bool cached = false;
    try {
      int64_t mdxSize = (int64_t)std::filesystem::file_size(mdxFiles[i]);
      cached = std::filesystem::exists(cachePath);
      if (cached) {
        DictCache probe;
        probe.open(cachePath.c_str(), mdxSize);
        cached = probe.isValid();
      }
    } catch (...) {}

    if (progressCallback_) {
      progressDict_ = baseName;
      progressCurrent_ = i + 1;
      progressTotal_ = mdxFiles.size();
      progressCached_ = cached;
      progressCallback_(baseName, i + 1, mdxFiles.size(), cached);
    }

    DictEntry dictEntry;
    try {
      std::string error = loadDict(mdxFiles[i].c_str(), dictEntry);
      if (error.empty()) {
        dicts_.push_back(std::move(dictEntry));
        loaded++;
      } else {
        OutputDebugStringA(("LoadDict failed: " + error + "\n").c_str());
      }
    } catch (const std::exception& e) {
      OutputDebugStringA(("LoadDict exception: " + std::string(e.what()) + "\n").c_str());
    } catch (...) {
      OutputDebugStringA("LoadDict unknown exception\n");
    }
  }

  return loaded;
}

void DictionaryManager::finalizeLoading() {
  std::lock_guard<std::mutex> lock(mutex_);

  // If dict order doesn't match current dicts, update it
  if (dictOrder_.empty() || dictOrder_.size() != dicts_.size()) {
    dictOrder_.clear();
    for (auto& d : dicts_) dictOrder_.push_back(d.title);
  }
}

std::vector<std::string> DictionaryManager::prefixMatch(const std::string& prefix, size_t maxResults) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> results;
  std::string lowerPrefix = prefix;
  std::transform(lowerPrefix.begin(), lowerPrefix.end(), lowerPrefix.begin(),
    [](unsigned char c) { return std::tolower(c); });
  for (auto& dict : dicts_) {
    if (dict.cache && dict.cache->isValid()) {
      auto words = dict.cache->prefixMatchWords(lowerPrefix, maxResults - results.size());
      for (auto& w : words) {
        if (results.size() >= maxResults) break;
        if (std::find(results.begin(), results.end(), w) == results.end())
          results.push_back(std::move(w));
      }
    }
  }
  return results;
}

std::string DictionaryManager::lookup(const std::string& word) {
  std::lock_guard<std::mutex> lock(mutex_);
  return lookupInternal(word, 0);
}

std::string DictionaryManager::lookupInternal(const std::string& word, int depth) {
  if (depth > 10) return "";
  std::string allResults;
  int matchCount = 0;

  for (auto& dict : dicts_) {
    if (!dict.cache || !dict.cache->isValid()) continue;

    try {
      DictCache::RecordLookup rl;
      if (!dict.cache->lookupRecord(word, dict.recordPos, rl)) continue;

      // Open MDX and read just the one compressed block
      FILE* f = openFileWide(dict.filePath, "rb");
      if (!f) continue;
      fseek64(f, rl.compressedBlockPos, SEEK_SET);
      std::vector<char> compressedData(rl.compressedBlockSize);
      size_t bytesRead = fread(compressedData.data(), 1, rl.compressedBlockSize, f);
      fclose(f);
      if ((int64_t)bytesRead != rl.compressedBlockSize) continue;

      std::vector<char> decompressed;
      if (!Mdict::MdictParser::parseCompressedBlock(rl.compressedBlockSize, compressedData.data(),
          rl.decompressedBlockSize, decompressed)) continue;

      std::string article;
      if (rl.recordOffset >= 0 && rl.recordOffset < (int64_t)decompressed.size()) {
        size_t len = (size_t)rl.recordSize;
        size_t avail = decompressed.size() - (size_t)rl.recordOffset;
        if (len > avail) len = avail;
        article.assign(decompressed.data() + rl.recordOffset, len);
      }
      if (article.empty()) continue;

      if (dict.encoding != "UTF-8" && dict.encoding != "UTF8")
        article = Iconv::toUtf8(dict.encoding.c_str(), article.data(), article.size());

      // Ensure article is valid UTF-8 (handles MDX with wrong encoding header, e.g. header says UTF-8 but content is GBK)
      article = Iconv::ensureUtf8(article);

      // Handle @@@LINK= redirect (e.g. @@@LINK=牟 → look up 牟 instead)
      {
        static const std::string linkPrefix = "@@@LINK=";
        size_t start = 0;
        while (start < article.size() && (unsigned char)article[start] <= ' ') start++;
        if (article.compare(start, linkPrefix.size(), linkPrefix) == 0) {
          size_t targetStart = start + linkPrefix.size();
          size_t targetEnd = targetStart;
          while (targetEnd < article.size() && article[targetEnd] != '\0' &&
                 (unsigned char)article[targetEnd] > ' ') targetEnd++;
          std::string target = article.substr(targetStart, targetEnd - targetStart);
          if (!target.empty()) {
            std::string linked = lookupInternal(target, depth + 1);
            if (!linked.empty()) return linked;
          }
          continue;
        }
      }

      if (!dict.styleSheets.empty())
        Mdict::MdictParser::substituteStylesheet(article, dict.styleSheets);

      // Replace sound:// links with clickable audio links
      // Pattern: href="sound://resource_path" → href="#" data-dict="title" data-sound="resource_path"
      // Also handles href='sound://...' by converting to double-quoted attributes
      {
        static const std::string soundPrefix = "sound://";
        size_t pos = 0;
        while ((pos = article.find(soundPrefix, pos)) != std::string::npos) {
          // Find end of URL (stop at quote, angle bracket, or whitespace)
          size_t urlStart = pos + soundPrefix.size();
          size_t urlEnd = urlStart;
          while (urlEnd < article.size()) {
            char c = article[urlEnd];
            if (c == '"' || c == '\'' || c == '<' || c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
              break;
            urlEnd++;
          }
          std::string soundUrl = article.substr(urlStart, urlEnd - urlStart);
          if (soundUrl.empty()) { pos = urlEnd; continue; }

          // Escape dict title for HTML attribute (replace " with &quot;)
          std::string escapedTitle = dict.title;
          for (size_t i = 0; i < escapedTitle.size(); i++)
            if (escapedTitle[i] == '"') { escapedTitle.replace(i, 1, "&quot;"); i += 4; }

          // Escape soundUrl for HTML attribute
          std::string escapedUrl = soundUrl;
          for (size_t i = 0; i < escapedUrl.size(); i++)
            if (escapedUrl[i] == '"') { escapedUrl.replace(i, 1, "&quot;"); i += 4; }

          // Determine if the href uses single or double quotes
          char quoteChar = '"';
          size_t hrefPos = article.rfind("href", pos);
          if (hrefPos != std::string::npos && hrefPos + 4 < pos) {
            for (size_t i = hrefPos + 4; i < pos; i++) {
              if (article[i] == '"' || article[i] == '\'') {
                quoteChar = article[i];
                break;
              }
            }
          }

          if (quoteChar == '\'') {
            // Single-quoted href: replace from opening ' to closing '
            // href='sound://url' → href="#" data-dict="..." data-sound="url"
            // Find the opening quote position (the ' after =)
            size_t openQuote = pos;
            if (hrefPos != std::string::npos) {
              size_t eqPos = article.find('=', hrefPos);
              if (eqPos != std::string::npos && eqPos + 1 < pos)
                openQuote = eqPos + 1;
            }
            // Replace from openQuote to urlEnd (includes both quotes and the URL)
            std::string replacement = "\"#\" data-dict=\"" + escapedTitle + "\" data-sound=\"" + escapedUrl + "\"";
            article.replace(openQuote, urlEnd - openQuote + 1, replacement);
            pos = openQuote + replacement.size();
          } else {
            // Double-quoted href: href="sound://url" → href="#" data-dict="..." data-sound="url"
            // Replace just sound://url with #url" data-dict="..." data-sound="url
            std::string replacement = "#" + escapedUrl + "\" data-dict=\"" + escapedTitle + "\" data-sound=\"" + escapedUrl;
            article.replace(pos, soundPrefix.size() + soundUrl.size(), replacement);
            pos += replacement.size();
          }
        }
      }

      // Replace <img src="..."> with base64 data from MDD resources
      {
        size_t pos = 0;
        while ((pos = article.find("<img ", pos)) != std::string::npos) {
          size_t tagEnd = article.find('>', pos);
          if (tagEnd == std::string::npos) { pos += 5; continue; }

          // Find the src attribute (try double-quoted then single-quoted)
          size_t srcPos = std::string::npos;
          char quoteChar = '"';
          {
            size_t sp = article.find("src=\"", pos);
            if (sp != std::string::npos && sp < tagEnd) { srcPos = sp; quoteChar = '"'; }
            else {
              sp = article.find("src='", pos);
              if (sp != std::string::npos && sp < tagEnd) { srcPos = sp; quoteChar = '\''; }
            }
          }
          if (srcPos == std::string::npos) { pos = tagEnd + 1; continue; }

          size_t urlStart = srcPos + 5; // skip src=" or src='
          size_t urlEnd = article.find(quoteChar, urlStart);
          if (urlEnd == std::string::npos || urlEnd > tagEnd) { pos = tagEnd + 1; continue; }

          std::string imgSrc = article.substr(urlStart, urlEnd - urlStart);

          // Skip URLs that are already data:, http://, https://, or sound://
          if (imgSrc.empty() ||
              imgSrc.compare(0, 5, "data:") == 0 ||
              imgSrc.compare(0, 7, "http://") == 0 ||
              imgSrc.compare(0, 8, "https://") == 0 ||
              imgSrc.compare(0, 7, "sound://") == 0) {
            pos = urlEnd + 1;
            continue;
          }

          // Try to load the image from MDD
          std::string base64Data = lookupResourceLocked(dict.title, imgSrc);
          if (base64Data.empty()) {
            pos = urlEnd + 1;
            continue;
          }

          // Determine MIME type from extension
          std::string ext;
          auto dotPos2 = imgSrc.rfind('.');
          if (dotPos2 != std::string::npos) {
            ext = imgSrc.substr(dotPos2 + 1);
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
          }
          std::string mime;
          if (ext == "png") mime = "image/png";
          else if (ext == "jpg" || ext == "jpeg") mime = "image/jpeg";
          else if (ext == "gif") mime = "image/gif";
          else if (ext == "svg") mime = "image/svg+xml";
          else if (ext == "webp") mime = "image/webp";
          else if (ext == "bmp") mime = "image/bmp";
          else if (ext == "ico") mime = "image/x-icon";
          else if (ext == "tif" || ext == "tiff") mime = "image/tiff";
          else mime = "image/png"; // default for MDX dictionaries

          std::string dataUri = "data:" + mime + ";base64," + base64Data;
          article.replace(urlStart, urlEnd - urlStart, dataUri);
          pos = urlStart + dataUri.size();
        }
      }

      matchCount++;
      allResults += "<div class=\"dict-section\" data-dict=\"" + dict.title + "\" data-dict-index=\"" + std::to_string(matchCount - 1) + "\">";
      std::string nameHtml;
      if (!dict.iconBase64.empty())
        nameHtml += "<img class=\"dict-icon\" src=\"" + dict.iconBase64 + "\">";
      nameHtml += dict.title;
      allResults += "<div class=\"dict-name\">" + nameHtml + "</div>";
      // Scope CSS to this dict-section to prevent cross-dictionary style bleed
      std::string scopedCss = dict.cssContent;
      if (!scopedCss.empty()) {
        std::string scope = "[data-dict-index=\"" + std::to_string(matchCount - 1) + "\"] ";
        std::string result;
        result.reserve(scopedCss.size() + scopedCss.size() / 4);
        int depth = 0;
        size_t ruleStart = 0;
        for (size_t i = 0; i < scopedCss.size(); i++) {
          char c = scopedCss[i];
          if (c == '{') {
            if (depth == 0) {
              // Start of a rule block — prefix selector with scope
              std::string sel = scopedCss.substr(ruleStart, i - ruleStart);
              // Skip @-rules
              size_t nonWs = sel.find_first_not_of(" \t\r\n");
              if (nonWs != std::string::npos && sel[nonWs] == '@') {
                result += sel;
              } else {
                // Trim trailing whitespace from selector
                size_t end = sel.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) sel = sel.substr(0, end + 1);
                // Prefix each comma-separated selector
                std::string scoped;
                size_t pos = 0;
                while (pos < sel.size()) {
                  size_t comma = sel.find(',', pos);
                  std::string part = sel.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                  size_t s = part.find_first_not_of(" \t\r\n");
                  if (s != std::string::npos) {
                    if (!scoped.empty()) scoped += ",";
                    scoped += scope + part.substr(s);
                  }
                  if (comma == std::string::npos) break;
                  pos = comma + 1;
                }
                result += scoped;
              }
            }
            depth++;
            result += c;
          } else if (c == '}') {
            depth--;
            result += c;
            if (depth == 0) {
              ruleStart = i + 1;
            }
          } else if (depth == 0 && c == '/' && i + 1 < scopedCss.size() && scopedCss[i + 1] == '*') {
            // Skip CSS comments at top level
            size_t end = scopedCss.find("*/", i + 2);
            if (end == std::string::npos) end = scopedCss.size() - 2;
            result += scopedCss.substr(i, end + 2 - i);
            i = end + 1;
            ruleStart = i + 1;
          } else if (depth > 0) {
            // Inside a rule block — copy content as-is
            result += c;
          }
        }
        result += scopedCss.substr(ruleStart);
        scopedCss = std::move(result);
      }
      allResults += "<div class=\"dict-article\"><style>" + scopedCss + "</style>" + article + "</div>";
      allResults += "</div>";
    } catch (const std::exception& e) {
      // Skip this dictionary on error, continue with others
      continue;
    } catch (...) {
      continue;
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
  for (auto& dict : dicts_) total += dict.wordCount;
  return total;
}

size_t DictionaryManager::getDictCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dicts_.size();
}

nlohmann::json DictionaryManager::getDictList() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json list = nlohmann::json::array();

  auto addDict = [&](const DictEntry& dict) {
    std::string title = Iconv::ensureUtf8(dict.title);
    list.push_back({{"name", title}, {"icon", dict.iconBase64}, {"word_count", dict.wordCount}});
  };

  // If we have a saved order, output in that order
  if (!dictOrder_.empty()) {
    std::set<std::string> added;
    for (auto& name : dictOrder_) {
      for (auto& dict : dicts_) {
        if (dict.title == name && added.find(name) == added.end()) {
          addDict(dict);
          added.insert(name);
          break;
        }
      }
    }
    // Add any not in order
    for (auto& dict : dicts_) {
      if (added.find(dict.title) == added.end()) {
        addDict(dict);
      }
    }
  } else {
    for (auto& dict : dicts_) {
      addDict(dict);
    }
  }
  return list;
}


void DictionaryManager::setDictOrder(const std::vector<std::string>& order) {
  std::lock_guard<std::mutex> lock(mutex_);
  dictOrder_ = order;
}

std::vector<std::string> DictionaryManager::getDictOrder() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dictOrder_;
}

std::string DictionaryManager::lookupResource(const std::string& dictTitle, const std::string& resourcePath) {
  std::lock_guard<std::mutex> lock(mutex_);
  return lookupResourceLocked(dictTitle, resourcePath);
}

std::string DictionaryManager::lookupResourceLocked(const std::string& dictTitle, const std::string& resourcePath) {

  for (auto& dict : dicts_) {
    if (dict.title != dictTitle) continue;
    if (dict.mddVolumes.empty()) continue;

    // Try multiple path variations to match MDD headword format
    std::vector<std::string> pathVariations;

    // 1. Normalize: convert forward slash to backslash, ensure leading backslash
    std::string normalized = resourcePath;
    for (size_t i = 0; i < normalized.size(); i++)
      if (normalized[i] == '/') normalized[i] = '\\';
    if (!normalized.empty() && normalized[0] != '\\')
      normalized = "\\" + normalized;
    pathVariations.push_back(normalized);

    // 2. Without leading backslash
    if (normalized.size() > 1)
      pathVariations.push_back(normalized.substr(1));

    // 3. With forward slashes and leading /
    std::string fwdSlash = resourcePath;
    for (size_t i = 0; i < fwdSlash.size(); i++)
      if (fwdSlash[i] == '\\') fwdSlash[i] = '/';
    if (!fwdSlash.empty() && fwdSlash[0] != '/')
      fwdSlash = "/" + fwdSlash;
    pathVariations.push_back(fwdSlash);

    // 4. Without leading /
    if (fwdSlash.size() > 1)
      pathVariations.push_back(fwdSlash.substr(1));

    // 5. Original as-is
    pathVariations.push_back(resourcePath);

    // Search all MDD volumes
    for (auto& vol : dict.mddVolumes) {
      if (!vol.cache || !vol.cache->isValid()) continue;

      try {
        // Try exact match with path variations
        for (auto& pathVar : pathVariations) {
          DictCache::RecordLookup rl;
          if (vol.cache->lookupRecord(pathVar, vol.recordPos, rl)) {
            std::string result = readResourceFromVolume(vol, rl);
            if (!result.empty()) return result;
          }
        }

        // Fallback: fuzzy search by filename
        DictCache::RecordLookup rl;
        if (vol.cache->lookupRecordLike(resourcePath, vol.recordPos, rl)) {
          std::string result = readResourceFromVolume(vol, rl);
          if (!result.empty()) return result;
        }
      } catch (...) {
        continue;
      }
    }
  }
  return "";
}

std::string DictionaryManager::readResourceFromVolume(const DictEntry::MddVolume& vol, const DictCache::RecordLookup& rl) {
  FILE* f = openFileWide(vol.filePath, "rb");
  if (!f) return "";
  fseek64(f, rl.compressedBlockPos, SEEK_SET);
  std::vector<char> compressedData(rl.compressedBlockSize);
  size_t bytesRead = fread(compressedData.data(), 1, rl.compressedBlockSize, f);
  fclose(f);
  if ((int64_t)bytesRead != rl.compressedBlockSize) return "";

  std::vector<char> decompressed;
  if (!Mdict::MdictParser::parseCompressedBlock(rl.compressedBlockSize, compressedData.data(),
      rl.decompressedBlockSize, decompressed)) return "";

  std::string data;
  if (rl.recordOffset >= 0 && rl.recordOffset < (int64_t)decompressed.size()) {
    size_t len = (size_t)rl.recordSize;
    size_t avail = decompressed.size() - (size_t)rl.recordOffset;
    if (len > avail) len = avail;
    data.assign(decompressed.data() + rl.recordOffset, len);
  }
  if (data.empty()) return "";

  // Return as base64
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  size_t sz = data.size();
  result.reserve(((sz + 2) / 3) * 4);
  for (size_t i = 0; i < sz; i += 3) {
    unsigned int n = ((unsigned int)(unsigned char)data[i]) << 16;
    if (i + 1 < sz) n |= ((unsigned int)(unsigned char)data[i + 1]) << 8;
    if (i + 2 < sz) n |= ((unsigned int)(unsigned char)data[i + 2]);
    result += tbl[(n >> 18) & 0x3F];
    result += tbl[(n >> 12) & 0x3F];
    result += (i + 1 < sz) ? tbl[(n >> 6) & 0x3F] : '=';
    result += (i + 2 < sz) ? tbl[n & 0x3F] : '=';
  }
  return result;
}

std::vector<std::string> DictionaryManager::searchMddHeadwords(const std::string& dictTitle, const std::string& pattern) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& dict : dicts_) {
    if (dict.title != dictTitle) continue;
    std::vector<std::string> results;
    for (auto& vol : dict.mddVolumes) {
      if (!vol.cache || !vol.cache->isValid()) continue;
      auto volResults = vol.cache->searchHeadwords(pattern);
      results.insert(results.end(), volResults.begin(), volResults.end());
    }
    return results;
  }
  return {};
}
