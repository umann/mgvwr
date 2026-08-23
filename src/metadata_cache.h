#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace metadata_cache {

using MetadataByPath = std::map<std::filesystem::path, nlohmann::json>;

struct SourceSnapshot {
    std::int64_t mtime = 0;
    std::int64_t size = 0;
};

using SourceSnapshotByPath = std::map<std::filesystem::path, SourceSnapshot>;

std::filesystem::path defaultMetadataCacheFile(const std::filesystem::path &baseCacheDir);

bool openMetadataCacheDatabase(const std::filesystem::path &databasePath, sqlite3 **db, std::string &errorMessage);

bool initializeMetadataCache(const std::filesystem::path &databasePath, std::string &errorMessage);

bool loadDirectoryMtimesBatch(sqlite3 *db, const std::vector<std::filesystem::path> &directories,
                              std::map<std::filesystem::path, std::int64_t> &directoryMtimes,
                              std::string &errorMessage);

bool loadDirectoryMtimesBatch(const std::filesystem::path &databasePath,
                              const std::vector<std::filesystem::path> &directories,
                              std::map<std::filesystem::path, std::int64_t> &directoryMtimes,
                              std::string &errorMessage);

bool storeDirectoryMtimesBatch(sqlite3 *db, const std::map<std::filesystem::path, std::int64_t> &directoryMtimes,
                               std::string &errorMessage);

bool storeDirectoryMtimesBatch(const std::filesystem::path &databasePath,
                               const std::map<std::filesystem::path, std::int64_t> &directoryMtimes,
                               std::string &errorMessage);

bool loadMetadataBatch(sqlite3 *db, const std::vector<std::filesystem::path> &imagePaths,
                       MetadataByPath &cachedMetadata, std::vector<std::filesystem::path> &missingPaths,
                       std::string &errorMessage, SourceSnapshotByPath *sourceSnapshots = nullptr);

bool loadMetadataBatch(const std::filesystem::path &databasePath, const std::vector<std::filesystem::path> &imagePaths,
                       MetadataByPath &cachedMetadata, std::vector<std::filesystem::path> &missingPaths,
                       std::string &errorMessage, SourceSnapshotByPath *sourceSnapshots = nullptr);

bool deleteMetadataBatch(sqlite3 *db, const std::vector<std::filesystem::path> &imagePaths, std::string &errorMessage);

bool deleteMetadataBatch(const std::filesystem::path &databasePath,
                         const std::vector<std::filesystem::path> &imagePaths, std::string &errorMessage);

bool storeMetadataBatch(sqlite3 *db, const MetadataByPath &metadataByPath, std::string &errorMessage);

bool storeMetadataBatch(const std::filesystem::path &databasePath, const MetadataByPath &metadataByPath,
                        std::string &errorMessage);

nlohmann::json stripTransientMetadata(nlohmann::json metadata);

std::optional<std::int64_t> getTakenEpochFromMetadata(const nlohmann::json &metadata);

nlohmann::json prepareMetadataForCache(nlohmann::json metadata);
nlohmann::json prepareMetadataForFts(nlohmann::json metadata);

} // namespace metadata_cache