#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace fs = std::filesystem;

enum class CacheRefreshTarget {
    None,
    Metadata,
    Thumbnail,
    MapTile,
};

fs::path getDefaultCacheLocation();
fs::path getThumbnailCacheLocation(const fs::path &cacheRoot);
fs::path getThumbnailCacheLocation();
fs::path getThumbnailCacheFilePath(const fs::path &imagePath, const fs::path &cacheRoot);
fs::path getThumbnailCacheMetaFilePath(const fs::path &thumbCacheFile);

bool writeThumbnailCacheFile(const fs::path &imagePath, const fs::path &thumbCacheFile);
bool thumbnailCacheMatchesSource(const fs::path &imagePath, const fs::path &thumbCacheFile);
bool thumbnailCacheMatchesSource(const fs::path &thumbCacheFile, std::int64_t sourceMtime, std::int64_t sourceSize);
bool ensureThumbnailCacheFileOnDisk(const fs::path &imagePath, const fs::path &thumbCacheFile);

unsigned int parseSizeValue(const std::string &sizeStr, unsigned int maxValue);
unsigned int parseSizeValue(const nlohmann::json &v, unsigned int maxValue);

std::optional<CacheRefreshTarget> parseCacheRefreshTarget(const std::string &value);
std::string cacheRefreshTargetName(CacheRefreshTarget target);
