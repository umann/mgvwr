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

#include <SFML/Graphics/Image.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sqlite3.h>
#include <sstream>
#include <string>
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
    if (sqlite3_exec(*db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &execError) != SQLITE_OK) {
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
    char *execError = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &execError) != SQLITE_OK) {
        errorMessage = execError != nullptr ? execError : sqlite3_errmsg(db);
        if (execError != nullptr) {
            sqlite3_free(execError);
        }
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
    const unsigned char *data = sqlite3_column_text(stmt, 3);
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

    sqlite3_close(db);
    return true;
}

bool loadMetadataBatch(const fs::path &databasePath, const std::vector<fs::path> &imagePaths,
                       MetadataByPath &cachedMetadata, std::vector<fs::path> &missingPaths,
                       std::string &errorMessage) {
    cachedMetadata.clear();
    missingPaths.clear();
    errorMessage.clear();

    sqlite3 *db = nullptr;
    if (!openDatabase(databasePath, &db, errorMessage)) {
        missingPaths = imagePaths;
        return false;
    }

    const char *selectSql = "SELECT f.mtime, f.size, f.checked, c.data "
                            "FROM dir d "
                            "JOIN file f ON f.dir_id = d.id "
                            "JOIN content c ON c.file_id = f.id "
                            "WHERE d.name = ?1 AND f.basename = ?2 LIMIT 1;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, selectSql, -1, &stmt, nullptr) != SQLITE_OK) {
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
            cachedMetadata[snapshot->absolutePath] = json::parse(stored->data);
        } catch (...) {
            missingPaths.push_back(snapshot->absolutePath);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
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

    sqlite3_stmt *upsertDir = nullptr;
    sqlite3_stmt *selectDirId = nullptr;
    sqlite3_stmt *upsertFile = nullptr;
    sqlite3_stmt *selectFileId = nullptr;
    sqlite3_stmt *upsertContent = nullptr;

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
        sqlite3_close(db);
    };

    auto fail = [&](const std::string &message) {
        std::string rollbackError;
        execSql(db, "ROLLBACK;", rollbackError);
        errorMessage = message;
        cleanup();
        return false;
    };

    if (sqlite3_prepare_v2(db, upsertDirSql, -1, &upsertDir, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, selectDirIdSql, -1, &selectDirId, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, upsertFileSql, -1, &upsertFile, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, selectFileIdSql, -1, &selectFileId, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, upsertContentSql, -1, &upsertContent, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db));
    }

    const std::int64_t checkedNow = nowEpochSeconds();
    for (const auto &[imagePath, metadata] : metadataByPath) {
        auto snapshot = snapshotFile(imagePath);
        if (!snapshot.has_value()) {
            continue;
        }

        json prepared = prepareMetadataForCache(metadata);
        std::int64_t taken = getTakenEpochFromMetadata(prepared).value_or(snapshot->mtime);
        int width = 1;
        int height = 1;
        imageDimensions(snapshot->absolutePath, width, height);

        bool hasLatitude = prepared.contains("GPSLatitude") && prepared["GPSLatitude"].is_number();
        bool hasLongitude = prepared.contains("GPSLongitude") && prepared["GPSLongitude"].is_number();

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

        std::string dumped = prepared.dump();
        sqlite3_reset(upsertContent);
        sqlite3_clear_bindings(upsertContent);
        sqlite3_bind_int64(upsertContent, 1, fileId);
        sqlite3_bind_int64(upsertContent, 2, taken);
        sqlite3_bind_int(upsertContent, 3, width);
        sqlite3_bind_int(upsertContent, 4, height);
        if (hasLatitude) {
            sqlite3_bind_double(upsertContent, 5, prepared["GPSLatitude"].get<double>());
        } else {
            sqlite3_bind_null(upsertContent, 5);
        }
        if (hasLongitude) {
            sqlite3_bind_double(upsertContent, 6, prepared["GPSLongitude"].get<double>());
        } else {
            sqlite3_bind_null(upsertContent, 6);
        }
        sqlite3_bind_text(upsertContent, 7, dumped.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(upsertContent) != SQLITE_DONE) {
            return fail(sqlite3_errmsg(db));
        }
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

json prepareMetadataForCache(json metadata) {
    metadata = stripTransientMetadata(std::move(metadata));

    if (!metadata.is_object()) {
        return metadata;
    }

    for (auto it = metadata.begin(); it != metadata.end();) {
        if (it.key() == "SourceFile") {
            ++it;
            continue;
        }
        if (isDefaultMetadataFieldValue(it.key(), it.value())) {
            it = metadata.erase(it);
            continue;
        }
        ++it;
    }

    return metadata;
}

} // namespace metadata_cache
