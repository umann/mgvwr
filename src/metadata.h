#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace metadata {

namespace fs = std::filesystem;
using json = nlohmann::json;
using MetadataByPath = std::map<fs::path, json>;

struct ProviderOptions {
    bool cacheEnabled = false;
    fs::path cacheFilePath;
    bool exiftoolAvailable = false;
    std::string exiftoolPath;
    size_t deferImageCountThreshold = 1000;
    size_t noExiftoolBatchThreshold = 20;
};

bool findExiftool(std::string &resolvedPath);

MetadataByPath extractExiftoolData(const std::vector<fs::path> &imagePaths, const std::string &exiftoolPath);

json makeIncompleteMetadata(const fs::path &imagePath);

bool ensureMetadataForImage(const fs::path &imagePath, MetadataByPath &targetCache, const ProviderOptions &options,
                            const std::function<void(const fs::path &)> &clearThumbnailCache);

void fillMetadataForFolder(const std::vector<fs::path> &imagePaths, MetadataByPath &targetCache,
                           const ProviderOptions &options, bool &deferMetadata, bool &sortByName,
                           const std::function<void(const fs::path &)> &clearThumbnailCache);

} // namespace metadata
