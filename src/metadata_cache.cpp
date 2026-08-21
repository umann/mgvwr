/*
metadata_cache.cpp

Cache metadata from image files in a SQLite database for faster access later.
DB file location is AppData\Local\Umann\MgVwr\cache\metadata.sqlite
Before reading metadata directly from image file (either via Poor Man's or Exiftool), check if
- Given file path is already in the cache
- Values mtime and size of the file match the cached values
- Column value "checked" is within the last 1 month (30 days) +/- random 7 days to avoid all files being physically
read at the same time If all above conditions are met, use the cached metadata instead of reading from the file. If
not, then read metadata from the file and update the cache with new values including column "checked"; also dir.mtime
although it will have significance later only when we implement a feature to detect if a directory has changed since
last scan.

hashThumbnailCacheKey should be calculated from imagePath only. When reading metadata directly from the file, delete
the cached thumbnail file if it exists. It will be lazy recreated later when the thumbnail is requested.

When creating new PNG thumbnail, add comment "SourceFile: <imagePath>" to the PNG file for debugging purposes.
*/

#include "metadata_cache.h"
#include "datetime_utils.h"
#include "json.hpp"
#include "utils.h"

#include <SFML/Graphics/Image.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>


namespace fs = std::filesystem;
using json = nlohmann::json;

namespace metadata_cache {

namespace {

std::string readFileAsString(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string loadCacheSchemaSql() {
    const std::vector<fs::path> candidates = {
        fs::path(__FILE__).parent_path().parent_path() / "data" / "cache_ddl.sql",
        fs::current_path() / "data" / "cache_ddl.sql",
        fs::path("data") / "cache_ddl.sql",
    };

    for (const auto &candidate : candidates) {
        std::string ddl = readFileAsString(candidate);
        if (!ddl.empty()) {
            return ddl;
        }
    }

    return {};
}

const std::string &cacheSchemaSql() {
    static const std::string ddl = loadCacheSchemaSql();
    return ddl;
}

constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;
constexpr std::int64_t kFreshnessBaseDays = 30;
constexpr std::int64_t kFreshnessJitterDays = 7;
constexpr std::int64_t kMtimeToleranceSeconds = 2;

struct KeyedToken {
    std::string key;
    std::string token;
};

struct FileSnapshot {
    fs::path absolutePath;
    std::string directoryKey;
    std::string basename;
    std::int64_t mtime = 0;
    std::int64_t size = 0;
};

struct StoredMetadataRow {
    std::int64_t mtime = 0;
    std::int64_t size = 0;
    std::int64_t checked = 0;
    int width = 0;
    int height = 0;
    bool hasLatitude = false;
    bool hasLongitude = false;
    double latitude = 0.0;
    double longitude = 0.0;
    std::string data;
};

std::int64_t fnv1a64(const std::string &value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return static_cast<std::int64_t>(hash);
}

std::int64_t nowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t fileTimeToEpochSeconds(fs::file_time_type fileTime) {
    const auto systemNow = std::chrono::system_clock::now();
    const auto fileNow = fs::file_time_type::clock::now();
    const auto systemTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(fileTime - fileNow + systemNow);
    return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
}

std::string normalizePathKey(const fs::path &path) {
    std::string value = path.lexically_normal().generic_string();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string directoryKeyForPath(const fs::path &path) {
    std::string value = normalizePathKey(path);
    if (!value.empty() && value.back() != '/') {
        value.push_back('/');
    }
    return value;
}

std::optional<FileSnapshot> snapshotFile(const fs::path &imagePath) {
    std::error_code ec;
    fs::path absolutePath = fs::absolute(imagePath, ec);
    if (ec) {
        return std::nullopt;
    }

    auto fileSize = fs::file_size(absolutePath, ec);
    if (ec) {
        return std::nullopt;
    }

    auto fileTime = fs::last_write_time(absolutePath, ec);
    if (ec) {
        return std::nullopt;
    }

    FileSnapshot snapshot;
    snapshot.absolutePath = absolutePath.lexically_normal();
    snapshot.directoryKey = directoryKeyForPath(snapshot.absolutePath.parent_path());
    snapshot.basename = snapshot.absolutePath.filename().string();
    snapshot.mtime = fileTimeToEpochSeconds(fileTime);
    snapshot.size = static_cast<std::int64_t>(fileSize);
    return snapshot;
}

std::int64_t freshnessWindowSecondsForPath(const fs::path &imagePath) {
    const std::int64_t span = 2 * kFreshnessJitterDays + 1;
    const std::int64_t jitter = (fnv1a64(normalizePathKey(imagePath)) % span) - kFreshnessJitterDays;
    return (kFreshnessBaseDays + jitter) * kSecondsPerDay;
}

bool isFresh(const fs::path &imagePath, std::int64_t checkedEpochSeconds) {
    return (nowEpochSeconds() - checkedEpochSeconds) <= freshnessWindowSecondsForPath(imagePath);
}

bool openDatabase(const fs::path &databasePath, sqlite3 **db, std::string &errorMessage) {
    *db = nullptr;
    int openRc = sqlite3_open(databasePath.string().c_str(), db);
    if (openRc != SQLITE_OK) {
        errorMessage = *db != nullptr ? sqlite3_errmsg(*db) : "sqlite3_open failed";
        if (*db != nullptr) {
            sqlite3_close(*db);
            *db = nullptr;
        }
        return false;
    }

    char *execError = nullptr;
    if (sql::exec(*db, "PRAGMA foreign_keys = ON;", &execError) != SQLITE_OK) {
        errorMessage = execError != nullptr ? execError : sqlite3_errmsg(*db);
        if (execError != nullptr) {
            sqlite3_free(execError);
        }
        sqlite3_close(*db);
        *db = nullptr;
        return false;
    }

    return true;
}

bool execSql(sqlite3 *db, const char *sql, std::string &errorMessage) {
    logSqlStatement(sql);
    char *execError = nullptr;
    if (sql::exec(db, sql, &execError) != SQLITE_OK) {
        errorMessage = execError != nullptr ? execError : sqlite3_errmsg(db);
        if (execError != nullptr) {
            sqlite3_free(execError);
        }
        return false;
    }
    return true;
}

bool ensureCompatibleFtsSchema(sqlite3 *db, std::string &errorMessage) {
    errorMessage.clear();

    // Probe whether the token CHECK constraint accepts normal ASCII text.
    char *execError = nullptr;
    const char *probeSql =
        "INSERT INTO token(name) VALUES('token_schema_probe'); DELETE FROM token WHERE name='token_schema_probe';";
    if (sql::exec(db, probeSql, &execError) == SQLITE_OK) {
        return true;
    }

    std::string probeError = execError != nullptr ? execError : sqlite3_errmsg(db);
    if (execError != nullptr) {
        sqlite3_free(execError);
    }

    if (probeError.find("CHECK constraint failed") == std::string::npos) {
        errorMessage = probeError;
        return false;
    }

    // Existing DB likely has an old token CHECK expression. Rebuild only FTS tables.
    std::string ignored;
    execSql(db, "DROP TABLE IF EXISTS token_word;", ignored);
    execSql(db, "DROP TABLE IF EXISTS content_token;", ignored);
    execSql(db, "DROP TABLE IF EXISTS word;", ignored);
    execSql(db, "DROP TABLE IF EXISTS token;", ignored);
    execSql(db, "DROP TABLE IF EXISTS fts_key;", ignored);

    if (!execSql(db, cacheSchemaSql().c_str(), errorMessage)) {
        return false;
    }

    return true;
}

std::optional<StoredMetadataRow> fetchStoredRow(sqlite3_stmt *stmt, const FileSnapshot &snapshot,
                                                std::string &errorMessage) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, snapshot.directoryKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, snapshot.basename.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
        errorMessage = sqlite3_errmsg(sqlite3_db_handle(stmt));
        return std::nullopt;
    }

    StoredMetadataRow row;
    row.mtime = sqlite3_column_int64(stmt, 0);
    row.size = sqlite3_column_int64(stmt, 1);
    row.checked = sqlite3_column_int64(stmt, 2);
    row.width = sqlite3_column_int(stmt, 3);
    row.height = sqlite3_column_int(stmt, 4);

    const auto latitudeType = sqlite3_column_type(stmt, 5);
    row.hasLatitude = latitudeType != SQLITE_NULL;
    if (row.hasLatitude) {
        row.latitude = sqlite3_column_double(stmt, 5);
    }

    const auto longitudeType = sqlite3_column_type(stmt, 6);
    row.hasLongitude = longitudeType != SQLITE_NULL;
    if (row.hasLongitude) {
        row.longitude = sqlite3_column_double(stmt, 6);
    }

    const unsigned char *data = sqlite3_column_text(stmt, 9);
    row.data = data != nullptr ? reinterpret_cast<const char *>(data) : "{}";
    return row;
}

bool imageDimensions(const fs::path &imagePath, int &width, int &height) {
    sf::Image image;
    if (!image.loadFromFile(imagePath)) {
        width = 1;
        height = 1;
        return false;
    }

    auto size = image.getSize();
    width = static_cast<int>(std::max(1u, size.x));
    height = static_cast<int>(std::max(1u, size.y));
    return true;
}

bool isAsciiAlnum(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0;
}

bool isAsciiLowerAlphaNumOrAllowedSimple(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '.' || ch == '/' || ch == '-';
}

bool isSpecialPunctuation(char ch) {
    return ch == '.' || ch == '/' || ch == '+' || ch == '-';
}

bool startsWith(const std::string &value, const std::string &prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string ltrimChar(std::string value, char needle) {
    size_t i = 0;
    while (i < value.size() && value[i] == needle) {
        ++i;
    }
    return value.substr(i);
}

bool decodeUtf8Codepoint(const std::string &input, size_t &offset, std::uint32_t &codepoint, size_t &width) {
    if (offset >= input.size()) {
        return false;
    }

    const unsigned char c0 = static_cast<unsigned char>(input[offset]);
    if (c0 < 0x80) {
        codepoint = c0;
        width = 1;
        return true;
    }

    auto continuation = [&](size_t index) {
        if (index >= input.size()) {
            return false;
        }
        const unsigned char cx = static_cast<unsigned char>(input[index]);
        return (cx & 0xC0) == 0x80;
    };

    if ((c0 & 0xE0) == 0xC0 && continuation(offset + 1)) {
        codepoint = (static_cast<std::uint32_t>(c0 & 0x1F) << 6) |
                    static_cast<std::uint32_t>(static_cast<unsigned char>(input[offset + 1]) & 0x3F);
        width = 2;
        return true;
    }

    if ((c0 & 0xF0) == 0xE0 && continuation(offset + 1) && continuation(offset + 2)) {
        codepoint = (static_cast<std::uint32_t>(c0 & 0x0F) << 12) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(input[offset + 1]) & 0x3F) << 6) |
                    static_cast<std::uint32_t>(static_cast<unsigned char>(input[offset + 2]) & 0x3F);
        width = 3;
        return true;
    }

    if ((c0 & 0xF8) == 0xF0 && continuation(offset + 1) && continuation(offset + 2) && continuation(offset + 3)) {
        codepoint = (static_cast<std::uint32_t>(c0 & 0x07) << 18) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(input[offset + 1]) & 0x3F) << 12) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(input[offset + 2]) & 0x3F) << 6) |
                    static_cast<std::uint32_t>(static_cast<unsigned char>(input[offset + 3]) & 0x3F);
        width = 4;
        return true;
    }

    codepoint = c0;
    width = 1;
    return true;
}

bool isUnicodeWhitespace(std::uint32_t cp) {
    if (cp <= 0x20) {
        return true;
    }
    return cp == 0x00A0 || cp == 0x1680 || cp == 0x180E || cp == 0x2000 || cp == 0x2001 || cp == 0x2002 ||
           cp == 0x2003 || cp == 0x2004 || cp == 0x2005 || cp == 0x2006 || cp == 0x2007 || cp == 0x2008 ||
           cp == 0x2009 || cp == 0x200A || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}

bool isLikelyUnicodeAlnum(std::uint32_t cp) {
    if (cp < 128) {
        return std::isalnum(static_cast<unsigned char>(cp)) != 0;
    }

    // Treat common Unicode punctuation/symbol ranges as non-alnum separators.
    if ((cp >= 0x00A0 && cp <= 0x00BF) || (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x20A0 && cp <= 0x20CF) ||
        (cp >= 0x2100 && cp <= 0x214F) || (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x2200 && cp <= 0x22FF) ||
        (cp >= 0x2300 && cp <= 0x23FF) || (cp >= 0x2460 && cp <= 0x24FF) || (cp >= 0x25A0 && cp <= 0x25FF) ||
        (cp >= 0x2600 && cp <= 0x26FF) || (cp >= 0x2700 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF) ||
        (cp >= 0x2E00 && cp <= 0x2E7F) || (cp >= 0x3000 && cp <= 0x303F)) {
        return false;
    }

    return true;
}

std::string normalizeTextWhitespace(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());

    bool previousSpace = false;
    size_t i = 0;
    while (i < raw.size()) {
        std::uint32_t cp = 0;
        size_t width = 0;
        const size_t start = i;
        if (!decodeUtf8Codepoint(raw, i, cp, width) || width == 0) {
            break;
        }
        i += width;

        if (isUnicodeWhitespace(cp)) {
            if (!previousSpace) {
                out.push_back(' ');
                previousSpace = true;
            }
            continue;
        }

        out.append(raw, start, width);
        previousSpace = false;
    }

    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string sanitizeTokenForStorage(std::string value) {
    if (value.empty()) {
        return value;
    }

    for (char &ch : value) {
        if (ch == '\t' || ch == '\n' || ch == '\r' || ch == '(' || ch == ')' || ch == '|') {
            ch = ' ';
        }
    }

    value = normalizeTextWhitespace(value);
    while (!value.empty() && value.front() == '-') {
        value.erase(value.begin());
    }
    value = normalizeTextWhitespace(value);
    return value;
}

std::unordered_map<std::uint32_t, std::string> loadUnidecodeMap() {
    const std::vector<fs::path> candidates = {
        fs::path(__FILE__).parent_path().parent_path() / "shared" / "data" / "i18n" / "unidecode_mapping.tsv",
        fs::current_path() / "shared" / "data" / "i18n" / "unidecode_mapping.tsv",
        fs::path("shared") / "data" / "i18n" / "unidecode_mapping.tsv",
    };

    std::string contents;
    for (const auto &candidate : candidates) {
        contents = readFileAsString(candidate);
        if (!contents.empty()) {
            break;
        }
    }

    std::unordered_map<std::uint32_t, std::string> mapping;
    if (contents.empty()) {
        return mapping;
    }

    std::istringstream in(contents);
    std::string line;
    bool firstLine = true;
    while (std::getline(in, line)) {
        if (firstLine) {
            firstLine = false;
            continue;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) {
            continue;
        }

        const std::string key = line.substr(0, tab);
        const std::string value = line.substr(tab + 1);

        size_t offset = 0;
        std::uint32_t cp = 0;
        size_t width = 0;
        if (!decodeUtf8Codepoint(key, offset, cp, width)) {
            continue;
        }
        mapping[cp] = value;
    }

    return mapping;
}

const std::unordered_map<std::uint32_t, std::string> &unidecodeMap() {
    static const std::unordered_map<std::uint32_t, std::string> map = loadUnidecodeMap();
    return map;
}

std::optional<std::string> getNormalizedStringField(const json &metadata, const std::string &field) {
    if (!metadata.contains(field) || !metadata[field].is_string()) {
        return std::nullopt;
    }
    const std::string value = normalizeTextWhitespace(metadata[field].get<std::string>());
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

bool hasSuspiciousCreatorText(const json &metadata) {
    auto creator = getNormalizedStringField(metadata, "Creator");
    if (!creator.has_value()) {
        return false;
    }

    return creator->find('?') != std::string::npos;
}

std::vector<std::string> extractNormalizedKeywords(const json &metadata) {
    std::vector<std::string> keywords;
    if (!metadata.contains("Keywords") || !metadata["Keywords"].is_array()) {
        return keywords;
    }

    for (const auto &entry : metadata["Keywords"]) {
        if (!entry.is_string()) {
            continue;
        }
        const std::string keyword = normalizeTextWhitespace(entry.get<std::string>());
        if (!keyword.empty()) {
            keywords.push_back(keyword);
        }
    }
    return keywords;
}

std::vector<std::string> sanitizedKeywordsForStoredMetadata(const json &metadata) {
    std::vector<std::string> keywords = extractNormalizedKeywords(metadata);

    // Creator is represented by dedicated metadata/token, so drop all © keywords.
    keywords.erase(
        std::remove_if(keywords.begin(), keywords.end(), [](const std::string &k) { return startsWith(k, "©"); }),
        keywords.end());

    // Location fields are represented by dedicated metadata/token, so drop matching @keywords.
    for (const std::string &key : {"Country", "State", "City", "Location"}) {
        auto value = getNormalizedStringField(metadata, key);
        if (!value.has_value()) {
            continue;
        }
        const std::string locationToken = "@" + *value;
        keywords.erase(std::remove(keywords.begin(), keywords.end(), locationToken), keywords.end());
    }

    // Face keyword aliases are converted into Face tokens; keep RegionInfo as canonical source.
    keywords.erase(
        std::remove_if(keywords.begin(), keywords.end(), [](const std::string &k) { return startsWith(k, "$"); }),
        keywords.end());

    return keywords;
}

std::vector<std::string> extractFaceNamesFromRegionInfo(const json &metadata) {
    std::set<std::string> uniqueNames;
    if (!metadata.contains("RegionInfo") || !metadata["RegionInfo"].is_object()) {
        return {};
    }

    const json &regionInfo = metadata["RegionInfo"];
    if (!regionInfo.contains("RegionList") || !regionInfo["RegionList"].is_array()) {
        return {};
    }

    for (const auto &region : regionInfo["RegionList"]) {
        if (!region.is_object() || !region.contains("Name") || !region["Name"].is_string()) {
            continue;
        }
        const std::string name = normalizeTextWhitespace(region["Name"].get<std::string>());
        if (!name.empty()) {
            uniqueNames.insert(name);
        }
    }

    return std::vector<std::string>(uniqueNames.begin(), uniqueNames.end());
}

void addKeyedToken(std::vector<KeyedToken> &tokens, std::set<std::pair<std::string, std::string>> &seen,
                   const std::string &key, const std::string &value) {
    const std::string sanitized = sanitizeTokenForStorage(value);
    if (sanitized.empty()) {
        return;
    }
    const auto pair = std::make_pair(key, sanitized);
    if (seen.insert(pair).second) {
        tokens.push_back({key, sanitized});
    }
}

std::vector<KeyedToken> generateFtsTokens(const json &metadata) {
    std::vector<KeyedToken> tokens;
    std::set<std::pair<std::string, std::string>> seen;

    if (!metadata.is_object()) {
        return tokens;
    }

    std::vector<std::string> keywords = extractNormalizedKeywords(metadata);

    if (auto dateTime = getNormalizedStringField(metadata, "DateTimeOriginal")) {
        if (dateTime->size() >= 10) {
            addKeyedToken(tokens, seen, "DateOriginal", dateTime->substr(0, 10));
        }
        if (dateTime->size() >= 7) {
            addKeyedToken(tokens, seen, "YearMonthOriginal", dateTime->substr(0, 7));
        }
        if (dateTime->size() >= 5) {
            addKeyedToken(tokens, seen, "YearOriginal", dateTime->substr(0, 5));
        }
    }

    std::string creator;
    if (auto creatorField = getNormalizedStringField(metadata, "Creator")) {
        creator = *creatorField;
    } else {
        for (const auto &keyword : keywords) {
            if (startsWith(keyword, "©")) {
                creator = normalizeTextWhitespace(keyword.substr(std::string("©").size()));
                break;
            }
        }
    }
    if (!creator.empty()) {
        addKeyedToken(tokens, seen, "Creator", "©" + creator);
    }
    keywords.erase(
        std::remove_if(keywords.begin(), keywords.end(), [](const std::string &k) { return startsWith(k, "©"); }),
        keywords.end());

    for (const std::string &key : {"Country", "State", "City", "Location"}) {
        auto value = getNormalizedStringField(metadata, key);
        if (!value.has_value()) {
            continue;
        }
        const std::string locationToken = "@" + *value;
        addKeyedToken(tokens, seen, key, locationToken);
        keywords.erase(std::remove(keywords.begin(), keywords.end(), locationToken), keywords.end());
    }

    for (const std::string &key : {"Description", "Make", "Model"}) {
        auto value = getNormalizedStringField(metadata, key);
        if (value.has_value()) {
            addKeyedToken(tokens, seen, key, *value);
        }
    }

    {
        std::set<std::string> uniqueFaces;
        for (const auto &face : extractFaceNamesFromRegionInfo(metadata)) {
            uniqueFaces.insert(face);
        }

        for (const auto &keyword : keywords) {
            if (startsWith(keyword, "$")) {
                std::string face = ltrimChar(keyword, '$');
                face = normalizeTextWhitespace(face);
                if (!face.empty()) {
                    uniqueFaces.insert(face);
                }
            }
        }

        keywords.erase(
            std::remove_if(keywords.begin(), keywords.end(), [](const std::string &k) { return startsWith(k, "$"); }),
            keywords.end());

        for (const auto &face : uniqueFaces) {
            addKeyedToken(tokens, seen, "Face", face);
        }
    }

    for (const auto &keyword : keywords) {
        addKeyedToken(tokens, seen, "Keyword", keyword);
    }

    return tokens;
}

std::string transformTokenForWordSplit(const std::string &token) {
    std::string transformed;
    transformed.reserve(token.size() + 8);

    size_t i = 0;
    while (i < token.size()) {
        std::uint32_t cp = 0;
        size_t width = 0;
        const size_t start = i;
        if (!decodeUtf8Codepoint(token, i, cp, width) || width == 0) {
            break;
        }
        i += width;

        if (cp < 128) {
            transformed.push_back(static_cast<char>(cp));
            continue;
        }

        if (cp == 0x00A9) {
            transformed.push_back(' ');
            continue;
        }

        if (isLikelyUnicodeAlnum(cp)) {
            transformed.append(token, start, width);
            continue;
        }

        const auto it = unidecodeMap().find(cp);
        if (it == unidecodeMap().end()) {
            transformed.push_back(' ');
            continue;
        }

        const std::string &mapped = it->second;
        if (!mapped.empty() && isAsciiAlnum(mapped.front())) {
            transformed.push_back(' ');
        }
        transformed += mapped;
        if (!mapped.empty() && isAsciiAlnum(mapped.back())) {
            transformed.push_back(' ');
        }
    }

    return transformed;
}

std::vector<std::string> splitIntoBaseWords(const std::string &tokenText) {
    std::vector<std::string> baseWords;
    std::string current;

    size_t i = 0;
    while (i < tokenText.size()) {
        std::uint32_t cp = 0;
        size_t width = 0;
        const size_t start = i;
        if (!decodeUtf8Codepoint(tokenText, i, cp, width) || width == 0) {
            break;
        }
        i += width;

        bool keep = false;
        if (cp < 128) {
            const char c = static_cast<char>(cp);
            keep = isAsciiAlnum(c) || isSpecialPunctuation(c);
        } else {
            keep = isLikelyUnicodeAlnum(cp);
        }

        if (keep) {
            current.append(tokenText, start, width);
        } else {
            if (!current.empty()) {
                baseWords.push_back(current);
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        baseWords.push_back(current);
    }

    return baseWords;
}

std::vector<std::string> splitBySpecialPunctuations(const std::string &word) {
    std::vector<std::string> out;
    std::string current;

    for (char c : word) {
        if (isSpecialPunctuation(c)) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }

    return out;
}

std::string toSimpleWord(const std::string &word) {
    std::string out;
    out.reserve(word.size());

    size_t i = 0;
    while (i < word.size()) {
        std::uint32_t cp = 0;
        size_t width = 0;
        if (!decodeUtf8Codepoint(word, i, cp, width) || width == 0) {
            break;
        }
        i += width;

        if (cp < 128) {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(cp)));
            if (isAsciiLowerAlphaNumOrAllowedSimple(c)) {
                out.push_back(c);
            }
            continue;
        }

        if (!isLikelyUnicodeAlnum(cp)) {
            continue;
        }

        const auto it = unidecodeMap().find(cp);
        if (it == unidecodeMap().end()) {
            continue;
        }

        for (unsigned char mapped : it->second) {
            if (mapped >= 128) {
                continue;
            }
            char c = static_cast<char>(std::tolower(mapped));
            if (isAsciiLowerAlphaNumOrAllowedSimple(c)) {
                out.push_back(c);
            }
        }
    }

    return out;
}

std::vector<std::string> generateTokenWords(const std::string &token) {
    std::vector<std::string> words;
    if (token.empty()) {
        return words;
    }

    const std::string transformed = transformTokenForWordSplit(token);
    const std::vector<std::string> baseWords = splitIntoBaseWords(transformed);

    std::set<std::string> unique;
    for (const auto &base : baseWords) {
        if (!base.empty()) {
            unique.insert(base);
        }
        for (const auto &split : splitBySpecialPunctuations(base)) {
            if (!split.empty()) {
                unique.insert(split);
            }
        }
    }

    for (auto word : unique) {
        std::transform(word.begin(), word.end(), word.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        while (!word.empty() && word.front() == '-') {
            word.erase(word.begin());
        }

        if (!word.empty()) {
            words.push_back(word);
        }
    }

    return words;
}

} // namespace

const char *getCacheSchemaDdl() {
    return cacheSchemaSql().c_str();
}

fs::path defaultMetadataCacheFile(const fs::path &baseCacheDir) {
    return baseCacheDir / "metadata.sqlite";
}

bool initializeMetadataCache(const fs::path &databasePath, std::string &errorMessage) {
    errorMessage.clear();

    std::error_code ec;
    fs::create_directories(databasePath.parent_path(), ec);
    if (ec) {
        errorMessage = "Failed to create metadata cache directory: " + ec.message();
        return false;
    }

    sqlite3 *db = nullptr;
    if (!openDatabase(databasePath, &db, errorMessage)) {
        return false;
    }

    if (!execSql(db, cacheSchemaSql().c_str(), errorMessage)) {
        sqlite3_close(db);
        return false;
    }

    if (!ensureCompatibleFtsSchema(db, errorMessage)) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_close(db);
    return true;
}

bool loadMetadataBatch(const fs::path &databasePath, const std::vector<fs::path> &imagePaths,
                       MetadataByPath &cachedMetadata, std::vector<fs::path> &missingPaths,
                       std::string &errorMessage, SourceSnapshotByPath *sourceSnapshots) {
    cachedMetadata.clear();
    missingPaths.clear();
    errorMessage.clear();

    if (sourceSnapshots) {
        sourceSnapshots->clear();
    }

    sqlite3 *db = nullptr;
    if (!openDatabase(databasePath, &db, errorMessage)) {
        missingPaths = imagePaths;
        return false;
    }

    const char *selectSql = "SELECT f.mtime, f.size, f.checked, c.width, c.height, c.latitude, c.longitude, d.name, "
                            "f.basename, c.data "
                            "FROM dir d "
                            "JOIN file f ON f.dir_id = d.id "
                            "JOIN content c ON c.file_id = f.id "
                            "WHERE d.name = ?1 AND f.basename = ?2 LIMIT 1;";

    sqlite3_stmt *stmt = nullptr;
    if (sql::prepare(db, selectSql, &stmt) != SQLITE_OK) {
        errorMessage = sqlite3_errmsg(db);
        sqlite3_close(db);
        missingPaths = imagePaths;
        return false;
    }

    for (const auto &imagePath : imagePaths) {
        auto snapshot = snapshotFile(imagePath);
        if (!snapshot.has_value()) {
            missingPaths.push_back(imagePath);
            continue;
        }

        if (sourceSnapshots) {
            (*sourceSnapshots)[snapshot->absolutePath] = {snapshot->mtime, snapshot->size};
        }

        auto stored = fetchStoredRow(stmt, *snapshot, errorMessage);
        if (!errorMessage.empty()) {
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            cachedMetadata.clear();
            missingPaths = imagePaths;
            return false;
        }

        if (!stored.has_value() || std::llabs(stored->mtime - snapshot->mtime) > kMtimeToleranceSeconds ||
            stored->size != snapshot->size || !isFresh(snapshot->absolutePath, stored->checked)) {
            missingPaths.push_back(snapshot->absolutePath);
            continue;
        }

        try {
            json metadata = json::parse(stored->data);
            if (!metadata.is_object()) {
                throw std::runtime_error("cached metadata is not an object");
            }

            if (hasSuspiciousCreatorText(metadata)) {
                missingPaths.push_back(snapshot->absolutePath);
                continue;
            }

            metadata["SourceFile"] = snapshot->absolutePath.string();
            if (stored->hasLatitude) {
                metadata["GPSLatitude"] = stored->latitude;
            }
            if (stored->hasLongitude) {
                metadata["GPSLongitude"] = stored->longitude;
            }

            if (stored->width > 0) {
                metadata["width"] = stored->width;
            }
            if (stored->height > 0) {
                metadata["height"] = stored->height;
            }

            cachedMetadata[snapshot->absolutePath] = std::move(metadata);
        } catch (...) {
            missingPaths.push_back(snapshot->absolutePath);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

bool deleteMetadataBatch(const fs::path &databasePath, const std::vector<fs::path> &imagePaths,
                         std::string &errorMessage) {
    errorMessage.clear();
    if (imagePaths.empty()) {
        return true;
    }

    sqlite3 *db = nullptr;
    if (!openDatabase(databasePath, &db, errorMessage)) {
        return false;
    }
    if (!execSql(db, cacheSchemaSql().c_str(), errorMessage)) {
        sqlite3_close(db);
        return false;
    }
    if (!execSql(db, "BEGIN IMMEDIATE TRANSACTION;", errorMessage)) {
        sqlite3_close(db);
        return false;
    }

    const char *selectFileIdSql = "SELECT f.id FROM dir d JOIN file f ON f.dir_id = d.id WHERE d.name = ?1 AND f.basename = ?2 LIMIT 1;";
    const char *deleteFileSql = "DELETE FROM file WHERE id = ?1;";
    const char *deleteUnusedTokensSql =
        "DELETE FROM token WHERE id NOT IN (SELECT DISTINCT token_id FROM content_token);";
    const char *deleteDanglingTokenWordsSql = "DELETE FROM token_word WHERE token_id NOT IN (SELECT id FROM token);";
    const char *deleteUnusedWordsSql = "DELETE FROM word WHERE id NOT IN (SELECT DISTINCT word_id FROM token_word);";

    sqlite3_stmt *selectFileId = nullptr;
    sqlite3_stmt *deleteFile = nullptr;
    sqlite3_stmt *deleteUnusedTokens = nullptr;
    sqlite3_stmt *deleteDanglingTokenWords = nullptr;
    sqlite3_stmt *deleteUnusedWords = nullptr;

    auto cleanup = [&]() {
        if (selectFileId)
            sqlite3_finalize(selectFileId);
        if (deleteFile)
            sqlite3_finalize(deleteFile);
        if (deleteUnusedTokens)
            sqlite3_finalize(deleteUnusedTokens);
        if (deleteDanglingTokenWords)
            sqlite3_finalize(deleteDanglingTokenWords);
        if (deleteUnusedWords)
            sqlite3_finalize(deleteUnusedWords);
        sqlite3_close(db);
    };

    auto fail = [&](const std::string &message) {
        std::string rollbackError;
        execSql(db, "ROLLBACK;", rollbackError);
        errorMessage = message;
        cleanup();
        return false;
    };

    if (sql::prepare(db, selectFileIdSql, &selectFileId) != SQLITE_OK ||
        sql::prepare(db, deleteFileSql, &deleteFile) != SQLITE_OK ||
        sql::prepare(db, deleteUnusedTokensSql, &deleteUnusedTokens) != SQLITE_OK ||
        sql::prepare(db, deleteDanglingTokenWordsSql, &deleteDanglingTokenWords) != SQLITE_OK ||
        sql::prepare(db, deleteUnusedWordsSql, &deleteUnusedWords) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db));
    }

    for (const auto &imagePath : imagePaths) {
        auto snapshot = snapshotFile(imagePath);
        if (!snapshot.has_value()) {
            continue;
        }

        sqlite3_reset(selectFileId);
        sqlite3_clear_bindings(selectFileId);
        sqlite3_bind_text(selectFileId, 1, snapshot->directoryKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(selectFileId, 2, snapshot->basename.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(selectFileId) != SQLITE_ROW) {
            continue;
        }

        const sqlite3_int64 fileId = sqlite3_column_int64(selectFileId, 0);

        sqlite3_reset(deleteFile);
        sqlite3_clear_bindings(deleteFile);
        sqlite3_bind_int64(deleteFile, 1, fileId);
        if (sqlite3_step(deleteFile) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }
    }

    sqlite3_reset(deleteUnusedTokens);
    if (sqlite3_step(deleteUnusedTokens) != SQLITE_DONE) {
        return fail(sqlite3_errmsg(db));
    }
    sqlite3_reset(deleteDanglingTokenWords);
    if (sqlite3_step(deleteDanglingTokenWords) != SQLITE_DONE) {
        return fail(sqlite3_errmsg(db));
    }
    sqlite3_reset(deleteUnusedWords);
    if (sqlite3_step(deleteUnusedWords) != SQLITE_DONE) {
        return fail(sqlite3_errmsg(db));
    }

    if (!execSql(db, "COMMIT;", errorMessage)) {
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

bool storeMetadataBatch(const fs::path &databasePath, const MetadataByPath &metadataByPath,
                        std::string &errorMessage) {
    errorMessage.clear();
    if (metadataByPath.empty()) {
        return true;
    }

    sqlite3 *db = nullptr;
    if (!openDatabase(databasePath, &db, errorMessage)) {
        return false;
    }
    if (!execSql(db, cacheSchemaSql().c_str(), errorMessage)) {
        sqlite3_close(db);
        return false;
    }
    if (!execSql(db, "BEGIN IMMEDIATE TRANSACTION;", errorMessage)) {
        sqlite3_close(db);
        return false;
    }

    const char *upsertDirSql = "INSERT INTO dir(name, mtime) VALUES(?1, ?2) "
                               "ON CONFLICT(name) DO UPDATE SET mtime = excluded.mtime;";
    const char *selectDirIdSql = "SELECT id FROM dir WHERE name = ?1;";
    const char *upsertFileSql = "INSERT INTO file(dir_id, basename, mtime, size, checked) VALUES(?1, ?2, ?3, ?4, ?5) "
                                "ON CONFLICT(dir_id, basename) DO UPDATE SET mtime = excluded.mtime, size = "
                                "excluded.size, checked = excluded.checked;";
    const char *selectFileIdSql = "SELECT id FROM file WHERE dir_id = ?1 AND basename = ?2;";
    const char *upsertContentSql =
        "INSERT INTO content(file_id, taken, width, height, latitude, longitude, data) VALUES(?1, ?2, ?3, ?4, ?5, ?6, "
        "?7) "
        "ON CONFLICT(file_id) DO UPDATE SET taken = excluded.taken, width = excluded.width, height = excluded.height, "
        "latitude = excluded.latitude, longitude = excluded.longitude, data = excluded.data;";
    const char *selectContentIdSql = "SELECT id FROM content WHERE file_id = ?1;";
    const char *selectFtsKeysSql = "SELECT id, name FROM fts_key;";
    const char *deleteContentTokensSql = "DELETE FROM content_token WHERE content_id = ?1;";
    const char *deleteUnusedTokensSql =
        "DELETE FROM token WHERE id NOT IN (SELECT DISTINCT token_id FROM content_token);";
    const char *deleteDanglingTokenWordsSql = "DELETE FROM token_word WHERE token_id NOT IN (SELECT id FROM token);";
    const char *deleteUnusedWordsSql = "DELETE FROM word WHERE id NOT IN (SELECT DISTINCT word_id FROM token_word);";
    const char *upsertTokenSql = "INSERT INTO token(name) VALUES(?1) ON CONFLICT(name) DO NOTHING;";
    const char *selectTokenIdSql = "SELECT id FROM token WHERE name = ?1;";
    const char *insertContentTokenSql =
        "INSERT OR IGNORE INTO content_token(content_id, fts_key_id, token_id) VALUES(?1, ?2, ?3);";
    const char *upsertWordSql =
        "INSERT INTO word(name, simple) VALUES(?1, ?2) ON CONFLICT(name) DO UPDATE SET simple = "
        "excluded.simple;";
    const char *selectWordIdSql = "SELECT id FROM word WHERE name = ?1;";
    const char *insertTokenWordSql = "INSERT OR IGNORE INTO token_word(token_id, word_id) VALUES(?1, ?2);";

    sqlite3_stmt *upsertDir = nullptr;
    sqlite3_stmt *selectDirId = nullptr;
    sqlite3_stmt *upsertFile = nullptr;
    sqlite3_stmt *selectFileId = nullptr;
    sqlite3_stmt *upsertContent = nullptr;
    sqlite3_stmt *selectContentId = nullptr;
    sqlite3_stmt *selectFtsKeys = nullptr;
    sqlite3_stmt *deleteContentTokens = nullptr;
    sqlite3_stmt *deleteUnusedTokens = nullptr;
    sqlite3_stmt *deleteDanglingTokenWords = nullptr;
    sqlite3_stmt *deleteUnusedWords = nullptr;
    sqlite3_stmt *upsertToken = nullptr;
    sqlite3_stmt *selectTokenId = nullptr;
    sqlite3_stmt *insertContentToken = nullptr;
    sqlite3_stmt *upsertWord = nullptr;
    sqlite3_stmt *selectWordId = nullptr;
    sqlite3_stmt *insertTokenWord = nullptr;

    auto cleanup = [&]() {
        if (upsertDir)
            sqlite3_finalize(upsertDir);
        if (selectDirId)
            sqlite3_finalize(selectDirId);
        if (upsertFile)
            sqlite3_finalize(upsertFile);
        if (selectFileId)
            sqlite3_finalize(selectFileId);
        if (upsertContent)
            sqlite3_finalize(upsertContent);
        if (selectContentId)
            sqlite3_finalize(selectContentId);
        if (selectFtsKeys)
            sqlite3_finalize(selectFtsKeys);
        if (deleteContentTokens)
            sqlite3_finalize(deleteContentTokens);
        if (deleteUnusedTokens)
            sqlite3_finalize(deleteUnusedTokens);
        if (deleteDanglingTokenWords)
            sqlite3_finalize(deleteDanglingTokenWords);
        if (deleteUnusedWords)
            sqlite3_finalize(deleteUnusedWords);
        if (upsertToken)
            sqlite3_finalize(upsertToken);
        if (selectTokenId)
            sqlite3_finalize(selectTokenId);
        if (insertContentToken)
            sqlite3_finalize(insertContentToken);
        if (upsertWord)
            sqlite3_finalize(upsertWord);
        if (selectWordId)
            sqlite3_finalize(selectWordId);
        if (insertTokenWord)
            sqlite3_finalize(insertTokenWord);
        sqlite3_close(db);
    };

    auto fail = [&](const std::string &message) {
        std::string rollbackError;
        execSql(db, "ROLLBACK;", rollbackError);
        errorMessage = message;
        cleanup();
        return false;
    };

    if (sql::prepare(db, upsertDirSql, &upsertDir) != SQLITE_OK ||
        sql::prepare(db, selectDirIdSql, &selectDirId) != SQLITE_OK ||
        sql::prepare(db, upsertFileSql, &upsertFile) != SQLITE_OK ||
        sql::prepare(db, selectFileIdSql, &selectFileId) != SQLITE_OK ||
        sql::prepare(db, upsertContentSql, &upsertContent) != SQLITE_OK ||
        sql::prepare(db, selectContentIdSql, &selectContentId) != SQLITE_OK ||
        sql::prepare(db, selectFtsKeysSql, &selectFtsKeys) != SQLITE_OK ||
        sql::prepare(db, deleteContentTokensSql, &deleteContentTokens) != SQLITE_OK ||
        sql::prepare(db, deleteUnusedTokensSql, &deleteUnusedTokens) != SQLITE_OK ||
        sql::prepare(db, deleteDanglingTokenWordsSql, &deleteDanglingTokenWords) != SQLITE_OK ||
        sql::prepare(db, deleteUnusedWordsSql, &deleteUnusedWords) != SQLITE_OK ||
        sql::prepare(db, upsertTokenSql, &upsertToken) != SQLITE_OK ||
        sql::prepare(db, selectTokenIdSql, &selectTokenId) != SQLITE_OK ||
        sql::prepare(db, insertContentTokenSql, &insertContentToken) != SQLITE_OK ||
        sql::prepare(db, upsertWordSql, &upsertWord) != SQLITE_OK ||
        sql::prepare(db, selectWordIdSql, &selectWordId) != SQLITE_OK ||
        sql::prepare(db, insertTokenWordSql, &insertTokenWord) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db));
    }

    std::map<std::string, sqlite3_int64> ftsKeyIds;
    while (sqlite3_step(selectFtsKeys) == SQLITE_ROW) {
        const sqlite3_int64 id = sqlite3_column_int64(selectFtsKeys, 0);
        const unsigned char *name = sqlite3_column_text(selectFtsKeys, 1);
        if (name != nullptr) {
            ftsKeyIds[reinterpret_cast<const char *>(name)] = id;
        }
    }
    sqlite3_reset(selectFtsKeys);

    const std::int64_t checkedNow = nowEpochSeconds();
    for (const auto &[imagePath, metadata] : metadataByPath) {
        auto snapshot = snapshotFile(imagePath);
        if (!snapshot.has_value()) {
            continue;
        }

        json prepared = prepareMetadataForCache(metadata);
        json ftsPrepared = prepareMetadataForFts(metadata);
        std::int64_t taken = getTakenEpochFromMetadata(prepared).value_or(snapshot->mtime);
        int width = 1;
        int height = 1;
        imageDimensions(snapshot->absolutePath, width, height);

        bool hasLatitude = metadata.contains("GPSLatitude") && metadata["GPSLatitude"].is_number();
        bool hasLongitude = metadata.contains("GPSLongitude") && metadata["GPSLongitude"].is_number();
        const double latitudeValue = hasLatitude ? metadata["GPSLatitude"].get<double>() : 0.0;
        const double longitudeValue = hasLongitude ? metadata["GPSLongitude"].get<double>() : 0.0;

        sqlite3_reset(upsertDir);
        sqlite3_clear_bindings(upsertDir);
        sqlite3_bind_text(upsertDir, 1, snapshot->directoryKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(upsertDir, 2, snapshot->mtime);
        if (sqlite3_step(upsertDir) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }

        sqlite3_reset(selectDirId);
        sqlite3_clear_bindings(selectDirId);
        sqlite3_bind_text(selectDirId, 1, snapshot->directoryKey.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(selectDirId) != SQLITE_ROW) {
            return fail(sqlite3_errmsg(db));
        }
        sqlite3_int64 dirId = sqlite3_column_int64(selectDirId, 0);

        sqlite3_reset(upsertFile);
        sqlite3_clear_bindings(upsertFile);
        sqlite3_bind_int64(upsertFile, 1, dirId);
        sqlite3_bind_text(upsertFile, 2, snapshot->basename.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(upsertFile, 3, snapshot->mtime);
        sqlite3_bind_int64(upsertFile, 4, snapshot->size);
        sqlite3_bind_int64(upsertFile, 5, checkedNow);
        if (sqlite3_step(upsertFile) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }

        sqlite3_reset(selectFileId);
        sqlite3_clear_bindings(selectFileId);
        sqlite3_bind_int64(selectFileId, 1, dirId);
        sqlite3_bind_text(selectFileId, 2, snapshot->basename.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(selectFileId) != SQLITE_ROW) {
            return fail(sqlite3_errmsg(db));
        }
        sqlite3_int64 fileId = sqlite3_column_int64(selectFileId, 0);

        prepared.erase("SourceFile");
        prepared.erase("GPSLatitude");
        prepared.erase("GPSLongitude");
        if (prepared.contains("width")) {
            prepared.erase("width");
        }
        if (prepared.contains("height")) {
            prepared.erase("height");
        }

        std::string dumped = prepared.dump();
        sqlite3_reset(upsertContent);
        sqlite3_clear_bindings(upsertContent);
        sqlite3_bind_int64(upsertContent, 1, fileId);
        sqlite3_bind_int64(upsertContent, 2, taken);
        sqlite3_bind_int(upsertContent, 3, width);
        sqlite3_bind_int(upsertContent, 4, height);
        if (hasLatitude) {
            sqlite3_bind_double(upsertContent, 5, latitudeValue);
        } else {
            sqlite3_bind_null(upsertContent, 5);
        }
        if (hasLongitude) {
            sqlite3_bind_double(upsertContent, 6, longitudeValue);
        } else {
            sqlite3_bind_null(upsertContent, 6);
        }
        sqlite3_bind_text(upsertContent, 7, dumped.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(upsertContent) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }

        sqlite3_reset(selectContentId);
        sqlite3_clear_bindings(selectContentId);
        sqlite3_bind_int64(selectContentId, 1, fileId);
        if (sqlite3_step(selectContentId) != SQLITE_ROW) {
            return fail(sqlite3_errmsg(db));
        }
        const sqlite3_int64 contentId = sqlite3_column_int64(selectContentId, 0);

        sqlite3_reset(deleteContentTokens);
        sqlite3_clear_bindings(deleteContentTokens);
        sqlite3_bind_int64(deleteContentTokens, 1, contentId);
        if (sqlite3_step(deleteContentTokens) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }

        sqlite3_reset(deleteUnusedTokens);
        if (sqlite3_step(deleteUnusedTokens) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }

        sqlite3_reset(deleteDanglingTokenWords);
        if (sqlite3_step(deleteDanglingTokenWords) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }

        sqlite3_reset(deleteUnusedWords);
        if (sqlite3_step(deleteUnusedWords) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }

        const std::vector<KeyedToken> keyedTokens = generateFtsTokens(ftsPrepared);
        for (const auto &keyedToken : keyedTokens) {
            const auto keyIt = ftsKeyIds.find(keyedToken.key);
            if (keyIt == ftsKeyIds.end() || keyedToken.token.empty()) {
                continue;
            }

            sqlite3_reset(upsertToken);
            sqlite3_clear_bindings(upsertToken);
            sqlite3_bind_text(upsertToken, 1, keyedToken.token.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(upsertToken) != SQLITE_DONE) {
                return fail(sqlite3_errmsg(db));
            }

            sqlite3_reset(selectTokenId);
            sqlite3_clear_bindings(selectTokenId);
            sqlite3_bind_text(selectTokenId, 1, keyedToken.token.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(selectTokenId) != SQLITE_ROW) {
                return fail(sqlite3_errmsg(db));
            }
            const sqlite3_int64 tokenId = sqlite3_column_int64(selectTokenId, 0);

            sqlite3_reset(insertContentToken);
            sqlite3_clear_bindings(insertContentToken);
            sqlite3_bind_int64(insertContentToken, 1, contentId);
            sqlite3_bind_int64(insertContentToken, 2, keyIt->second);
            sqlite3_bind_int64(insertContentToken, 3, tokenId);
            if (sqlite3_step(insertContentToken) != SQLITE_DONE) {
                return fail(sqlite3_errmsg(db));
            }

            const std::vector<std::string> tokenWords = generateTokenWords(keyedToken.token);
            for (const auto &word : tokenWords) {
                if (word.empty()) {
                    continue;
                }

                const std::string simple = toSimpleWord(word);
                if (simple.empty()) {
                    continue;
                }

                sqlite3_reset(upsertWord);
                sqlite3_clear_bindings(upsertWord);
                sqlite3_bind_text(upsertWord, 1, word.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(upsertWord, 2, simple.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(upsertWord) != SQLITE_DONE) {
                    return fail(sqlite3_errmsg(db));
                }

                sqlite3_reset(selectWordId);
                sqlite3_clear_bindings(selectWordId);
                sqlite3_bind_text(selectWordId, 1, word.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(selectWordId) != SQLITE_ROW) {
                    return fail(sqlite3_errmsg(db));
                }
                const sqlite3_int64 wordId = sqlite3_column_int64(selectWordId, 0);

                sqlite3_reset(insertTokenWord);
                sqlite3_clear_bindings(insertTokenWord);
                sqlite3_bind_int64(insertTokenWord, 1, tokenId);
                sqlite3_bind_int64(insertTokenWord, 2, wordId);
                if (sqlite3_step(insertTokenWord) != SQLITE_DONE) {
                    return fail(sqlite3_errmsg(db));
                }
            }
        }
    }

    sqlite3_reset(deleteUnusedTokens);
    if (sqlite3_step(deleteUnusedTokens) != SQLITE_DONE) {
        return fail(sqlite3_errmsg(db));
    }
    sqlite3_reset(deleteDanglingTokenWords);
    if (sqlite3_step(deleteDanglingTokenWords) != SQLITE_DONE) {
        return fail(sqlite3_errmsg(db));
    }
    sqlite3_reset(deleteUnusedWords);
    if (sqlite3_step(deleteUnusedWords) != SQLITE_DONE) {
        return fail(sqlite3_errmsg(db));
    }

    if (!execSql(db, "COMMIT;", errorMessage)) {
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

json stripTransientMetadata(json metadata) {
    metadata.erase("filters");
    metadata.erase("complete");
    return metadata;
}

std::optional<std::int64_t> getTakenEpochFromMetadata(const json &metadata) {
    if (!metadata.is_object()) {
        return std::nullopt;
    }

    if (!metadata.contains("DateTimeOriginal") || !metadata["DateTimeOriginal"].is_string()) {
        return std::nullopt;
    }

    const std::string dateTimeOriginal = metadata["DateTimeOriginal"].get<std::string>();
    if (dateTimeOriginal.empty() || dateTimeOriginal == "0000:00:00 00:00:00") {
        return std::nullopt;
    }

    return datetime_utils::getTakenEpochFromMetadata(metadata);
}

static bool isDefaultMetadataFieldValue(const std::string &key, const json &value) {
    if (value.is_null()) {
        return true;
    }
    if (value.is_string()) {
        const std::string &text = value.get_ref<const std::string &>();
        if (text.empty()) {
            return true;
        }
        if (key == "DateTimeOriginal" && text == "0000:00:00 00:00:00") {
            return true;
        }
        if (key == "OffsetTimeOriginal" && text == "+00:00") {
            return true;
        }
        return false;
    }
    if (value.is_array()) {
        return value.empty();
    }
    if (key == "Orientation") {
        if (value.is_number_integer()) {
            return value.get<int>() == 1;
        }
        if (value.is_number()) {
            return value.get<double>() == 1.0;
        }
    }
    return false;
}

static bool isValidOrientationValue(const json &value) {
    if (value.is_number_integer()) {
        const int orientation = value.get<int>();
        return orientation >= 1 && orientation <= 8;
    }
    if (value.is_number()) {
        const double orientation = value.get<double>();
        return orientation >= 1.0 && orientation <= 8.0;
    }
    return false;
}

json prepareMetadataForCache(json metadata) {
    metadata = stripTransientMetadata(std::move(metadata));

    if (!metadata.is_object()) {
        return metadata;
    }

    for (auto it = metadata.begin(); it != metadata.end();) {
        if (it.key() == "SourceFile" || it.key() == "GPSLatitude" || it.key() == "GPSLongitude" ||
            it.key() == "width" || it.key() == "height") {
            it = metadata.erase(it);
            continue;
        }
        if (isDefaultMetadataFieldValue(it.key(), it.value())) {
            it = metadata.erase(it);
            continue;
        }
        if (it.key() == "Orientation" && !isValidOrientationValue(it.value())) {
            it = metadata.erase(it);
            continue;
        }
        ++it;
    }

    if (metadata.is_object()) {
        const std::vector<std::string> cleanedKeywords = sanitizedKeywordsForStoredMetadata(metadata);
        if (cleanedKeywords.empty()) {
            metadata.erase("Keywords");
        } else {
            metadata["Keywords"] = cleanedKeywords;
        }
    }

    return metadata;
}

json prepareMetadataForFts(json metadata) {
    metadata = stripTransientMetadata(std::move(metadata));

    if (!metadata.is_object()) {
        return metadata;
    }

    for (auto it = metadata.begin(); it != metadata.end();) {
        if (it.key() == "SourceFile" || it.key() == "GPSLatitude" || it.key() == "GPSLongitude" ||
            it.key() == "width" || it.key() == "height") {
            it = metadata.erase(it);
            continue;
        }
        if (isDefaultMetadataFieldValue(it.key(), it.value())) {
            it = metadata.erase(it);
            continue;
        }
        if (it.key() == "Orientation" && !isValidOrientationValue(it.value())) {
            it = metadata.erase(it);
            continue;
        }
        ++it;
    }

    return metadata;
}

} // namespace metadata_cache
