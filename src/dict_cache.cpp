#include "dict_cache.hpp"
#include <cstring>
#ifdef _WIN32
#include <Windows.h>
#endif

DictCache::DictCache() {}
DictCache::~DictCache() { close(); }

void DictCache::close() {
  if (db_) { sqlite3_close(db_); db_ = nullptr; }
  valid_ = false;
  cachedFileSize_ = 0;
}

bool DictCache::exec(const char* sql) {
  char* errMsg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
    if (errMsg) sqlite3_free(errMsg);
    return false;
  }
  return true;
}

bool DictCache::ensureSchema() {
  return exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)") &&
         exec("CREATE TABLE IF NOT EXISTS headwords (id INTEGER PRIMARY KEY, word TEXT NOT NULL)") &&
         exec("CREATE INDEX IF NOT EXISTS idx_hw_word ON headwords(word COLLATE NOCASE)") &&
         exec("CREATE TABLE IF NOT EXISTS record_blocks (idx INTEGER PRIMARY KEY, "
              "start_pos INTEGER, end_pos INTEGER, shadow_start_pos INTEGER, shadow_end_pos INTEGER, "
              "compressed_size INTEGER, decompressed_size INTEGER)");
}

bool DictCache::open(const char* cachePath, int64_t mdxFileSize) {
  close();

#ifdef _WIN32
  int wlen = MultiByteToWideChar(CP_ACP, 0, cachePath, -1, nullptr, 0);
  std::wstring wpath(wlen, 0);
  MultiByteToWideChar(CP_ACP, 0, cachePath, -1, &wpath[0], wlen);
  if (sqlite3_open16(wpath.c_str(), &db_) != SQLITE_OK) {
#else
  if (sqlite3_open_v2(cachePath, &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
#endif
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    return false;
  }
  exec("PRAGMA journal_mode=WAL");
  exec("PRAGMA synchronous=NORMAL");

  ensureSchema();

  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key='file_size'", -1, &s, nullptr);
  int64_t fileSize = 0;
  if (sqlite3_step(s) == SQLITE_ROW) {
    const char* text = (const char*)sqlite3_column_text(s, 0);
    if (text) fileSize = strtoll(text, nullptr, 10);
  }
  sqlite3_finalize(s);

  cachedFileSize_ = fileSize;
  mdxFileSize_ = mdxFileSize;
  valid_ = (fileSize == mdxFileSize && fileSize > 0);
  return true;
}

std::string DictCache::loadMeta(const std::string& key) {
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key=?", -1, &s, nullptr);
  sqlite3_bind_text(s, 1, key.c_str(), (int)key.size(), SQLITE_STATIC);
  std::string result;
  if (sqlite3_step(s) == SQLITE_ROW) {
    const char* text = (const char*)sqlite3_column_text(s, 0);
    if (text) result = text;
  }
  sqlite3_finalize(s);
  return result;
}

int64_t DictCache::loadRecordPos() {
  std::string val = loadMeta("record_pos");
  return val.empty() ? 0 : strtoll(val.c_str(), nullptr, 10);
}

std::vector<std::string> DictCache::prefixMatchWords(const std::string& lowerPrefix, size_t maxResults) {
  std::vector<std::string> result;
  std::string pattern = lowerPrefix + "%";
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db_, "SELECT word FROM headwords WHERE word LIKE ? COLLATE NOCASE LIMIT ?", -1, &s, nullptr);
  sqlite3_bind_text(s, 1, pattern.c_str(), (int)pattern.size(), SQLITE_STATIC);
  sqlite3_bind_int64(s, 2, (int64_t)maxResults);
  while (sqlite3_step(s) == SQLITE_ROW && result.size() < maxResults) {
    const char* word = (const char*)sqlite3_column_text(s, 0);
    if (word) result.emplace_back(word);
  }
  sqlite3_finalize(s);
  return result;
}

bool DictCache::lookupRecord(const std::string& word, int64_t recordPos, RecordLookup& out) {
  // 1. Get word ID
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db_, "SELECT id FROM headwords WHERE word = ? COLLATE NOCASE", -1, &s, nullptr);
  sqlite3_bind_text(s, 1, word.c_str(), (int)word.size(), SQLITE_STATIC);
  int64_t targetId = -1;
  if (sqlite3_step(s) == SQLITE_ROW) {
    targetId = sqlite3_column_int64(s, 0);
  }
  sqlite3_finalize(s);
  if (targetId < 0) return false;

  // 2. Get next word ID (for record size)
  sqlite3_prepare_v2(db_, "SELECT id FROM headwords WHERE id > ? ORDER BY id ASC LIMIT 1", -1, &s, nullptr);
  sqlite3_bind_int64(s, 1, targetId);
  int64_t nextId = -1;
  if (sqlite3_step(s) == SQLITE_ROW) {
    nextId = sqlite3_column_int64(s, 0);
  }
  sqlite3_finalize(s);

  // 3. Find the record block containing this ID
  sqlite3_prepare_v2(db_,
    "SELECT start_pos, end_pos, shadow_start_pos, shadow_end_pos, compressed_size, decompressed_size "
    "FROM record_blocks WHERE shadow_start_pos <= ? AND shadow_end_pos > ? LIMIT 1", -1, &s, nullptr);
  sqlite3_bind_int64(s, 1, targetId);
  sqlite3_bind_int64(s, 2, targetId);
  bool found = false;
  if (sqlite3_step(s) == SQLITE_ROW) {
    int64_t startPos = sqlite3_column_int64(s, 0);
    int64_t shadowStart = sqlite3_column_int64(s, 2);
    int64_t shadowEnd = sqlite3_column_int64(s, 3);
    int64_t compSize = sqlite3_column_int64(s, 4);
    int64_t decompSize = sqlite3_column_int64(s, 5);

    out.compressedBlockPos = recordPos + startPos;
    out.compressedBlockSize = compSize;
    out.decompressedBlockSize = decompSize;
    out.recordOffset = targetId - shadowStart;
    out.recordSize = (nextId >= 0) ? (nextId - targetId) : (shadowEnd - targetId);
    found = true;
  }
  sqlite3_finalize(s);
  return found;
}

bool DictCache::lookupRecordLike(const std::string& filename, int64_t recordPos, RecordLookup& out) {
  // Fuzzy search: find headword ending with the given filename
  sqlite3_stmt* s = nullptr;
  // Try: word ends with filename (with possible path prefix)
  std::string pattern = "%" + filename;
  sqlite3_prepare_v2(db_, "SELECT id, word FROM headwords WHERE word LIKE ? COLLATE NOCASE LIMIT 1", -1, &s, nullptr);
  sqlite3_bind_text(s, 1, pattern.c_str(), (int)pattern.size(), SQLITE_STATIC);
  int64_t targetId = -1;
  if (sqlite3_step(s) == SQLITE_ROW) {
    targetId = sqlite3_column_int64(s, 0);
  }
  sqlite3_finalize(s);
  if (targetId < 0) return false;

  // Get next word ID
  sqlite3_prepare_v2(db_, "SELECT id FROM headwords WHERE id > ? ORDER BY id ASC LIMIT 1", -1, &s, nullptr);
  sqlite3_bind_int64(s, 1, targetId);
  int64_t nextId = -1;
  if (sqlite3_step(s) == SQLITE_ROW) {
    nextId = sqlite3_column_int64(s, 0);
  }
  sqlite3_finalize(s);

  // Find record block
  sqlite3_prepare_v2(db_,
    "SELECT start_pos, end_pos, shadow_start_pos, shadow_end_pos, compressed_size, decompressed_size "
    "FROM record_blocks WHERE shadow_start_pos <= ? AND shadow_end_pos > ? LIMIT 1", -1, &s, nullptr);
  sqlite3_bind_int64(s, 1, targetId);
  sqlite3_bind_int64(s, 2, targetId);
  bool found = false;
  if (sqlite3_step(s) == SQLITE_ROW) {
    int64_t startPos = sqlite3_column_int64(s, 0);
    int64_t shadowStart = sqlite3_column_int64(s, 2);
    int64_t shadowEnd = sqlite3_column_int64(s, 3);
    int64_t compSize = sqlite3_column_int64(s, 4);
    int64_t decompSize = sqlite3_column_int64(s, 5);

    out.compressedBlockPos = recordPos + startPos;
    out.compressedBlockSize = compSize;
    out.decompressedBlockSize = decompSize;
    out.recordOffset = targetId - shadowStart;
    out.recordSize = (nextId >= 0) ? (nextId - targetId) : (shadowEnd - targetId);
    found = true;
  }
  sqlite3_finalize(s);
  return found;
}

std::vector<std::string> DictCache::searchHeadwords(const std::string& pattern, size_t maxResults) {
  std::vector<std::string> results;
  sqlite3_stmt* s = nullptr;
  std::string likePattern = "%" + pattern + "%";
  sqlite3_prepare_v2(db_, "SELECT word FROM headwords WHERE word LIKE ? COLLATE NOCASE LIMIT ?",
                     -1, &s, nullptr);
  sqlite3_bind_text(s, 1, likePattern.c_str(), (int)likePattern.size(), SQLITE_STATIC);
  sqlite3_bind_int64(s, 2, (int64_t)maxResults);
  while (sqlite3_step(s) == SQLITE_ROW) {
    results.push_back((const char*)sqlite3_column_text(s, 0));
  }
  sqlite3_finalize(s);
  return results;
}

bool DictCache::saveIndex(const std::string& title, const std::string& description,
                          const std::string& encoding, uint32_t wordCount, int64_t recordPos,
                          const std::vector<CachedHeadWord>& headwords,
                          const std::vector<CachedRecordBlock>& recordBlocks,
                          const Mdict::MdictParser::StyleSheets& styleSheets) {
  exec("BEGIN TRANSACTION");

  auto setMeta = [&](const std::string& k, const std::string& v) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)", -1, &s, nullptr);
    sqlite3_bind_text(s, 1, k.c_str(), (int)k.size(), SQLITE_STATIC);
    sqlite3_bind_text(s, 2, v.c_str(), (int)v.size(), SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
  };

  setMeta("title", title);
  setMeta("description", description);
  setMeta("encoding", encoding);
  setMeta("word_count", std::to_string(wordCount));
  setMeta("record_pos", std::to_string(recordPos));
  setMeta("file_size", std::to_string(mdxFileSize_));

  exec("DELETE FROM headwords");
  sqlite3_stmt* hwStmt = nullptr;
  sqlite3_prepare_v2(db_, "INSERT INTO headwords (id, word) VALUES (?, ?)", -1, &hwStmt, nullptr);
  for (auto& hw : headwords) {
    sqlite3_bind_int64(hwStmt, 1, hw.id);
    sqlite3_bind_text(hwStmt, 2, hw.word.c_str(), (int)hw.word.size(), SQLITE_STATIC);
    sqlite3_step(hwStmt);
    sqlite3_reset(hwStmt);
  }
  sqlite3_finalize(hwStmt);

  exec("DELETE FROM record_blocks");
  sqlite3_stmt* rbStmt = nullptr;
  sqlite3_prepare_v2(db_, "INSERT INTO record_blocks (idx, start_pos, end_pos, shadow_start_pos, shadow_end_pos, compressed_size, decompressed_size) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &rbStmt, nullptr);
  for (int64_t i = 0; i < (int64_t)recordBlocks.size(); i++) {
    auto& rb = recordBlocks[i];
    sqlite3_bind_int64(rbStmt, 1, i);
    sqlite3_bind_int64(rbStmt, 2, rb.startPos);
    sqlite3_bind_int64(rbStmt, 3, rb.endPos);
    sqlite3_bind_int64(rbStmt, 4, rb.shadowStartPos);
    sqlite3_bind_int64(rbStmt, 5, rb.shadowEndPos);
    sqlite3_bind_int64(rbStmt, 6, rb.compressedSize);
    sqlite3_bind_int64(rbStmt, 7, rb.decompressedSize);
    sqlite3_step(rbStmt);
    sqlite3_reset(rbStmt);
  }
  sqlite3_finalize(rbStmt);

  // Serialize stylesheets
  std::string ssData;
  for (auto& [id, kv] : styleSheets) {
    ssData += (char)((id >> 24) & 0xFF);
    ssData += (char)((id >> 16) & 0xFF);
    ssData += (char)((id >> 8) & 0xFF);
    ssData += (char)(id & 0xFF);
    int32_t klen = (int32_t)kv.first.size();
    ssData += (char)((klen >> 24) & 0xFF);
    ssData += (char)((klen >> 16) & 0xFF);
    ssData += (char)((klen >> 8) & 0xFF);
    ssData += (char)(klen & 0xFF);
    ssData += kv.first;
    int32_t vlen = (int32_t)kv.second.size();
    ssData += (char)((vlen >> 24) & 0xFF);
    ssData += (char)((vlen >> 16) & 0xFF);
    ssData += (char)((vlen >> 8) & 0xFF);
    ssData += (char)(vlen & 0xFF);
    ssData += kv.second;
  }
  setMeta("stylesheets", ssData);

  exec("COMMIT");
  valid_ = true;
  return true;
}

Mdict::MdictParser::StyleSheets DictCache::loadStyleSheets() {
  Mdict::MdictParser::StyleSheets result;
  std::string data = loadMeta("stylesheets");
  if (data.empty()) return result;
  const char* p = data.c_str();
  const char* end = p + data.size();
  while (p < end) {
    if (end - p < 4) break;
    int32_t id = ((int32_t)(uint8_t)p[0] << 24) | ((int32_t)(uint8_t)p[1] << 16) |
                 ((int32_t)(uint8_t)p[2] << 8) | (int32_t)(uint8_t)p[3];
    p += 4;
    if (end - p < 4) break;
    int32_t klen = ((int32_t)(uint8_t)p[0] << 24) | ((int32_t)(uint8_t)p[1] << 16) |
                   ((int32_t)(uint8_t)p[2] << 8) | (int32_t)(uint8_t)p[3];
    p += 4;
    if (end - p < klen) break;
    std::string key(p, klen);
    p += klen;
    if (end - p < 4) break;
    int32_t vlen = ((int32_t)(uint8_t)p[0] << 24) | ((int32_t)(uint8_t)p[1] << 16) |
                   ((int32_t)(uint8_t)p[2] << 8) | (int32_t)(uint8_t)p[3];
    p += 4;
    if (end - p < vlen) break;
    std::string val(p, vlen);
    p += vlen;
    result[id] = {std::move(key), std::move(val)};
  }
  return result;
}
