#include "metadata.h"

#include "config.h"
#include "exiftool_response_schema.h"
#include "metadata_cache.h"
#include "poor_mans_exiftool.h"
#include "utils.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace metadata {

namespace {

static const std::vector<std::string> EXIF_STRING_KEYS = {
    "City",           "Country", "Creator", "DateTimeOriginal",   "Description",
    "Location",       "Make",    "Model",   "OffsetTimeOriginal", "Orientation",
    "XMP:RegionInfo", "State",
};

void markMetadataComplete(MetadataByPath &targetCache, const fs::path &imagePath, const json &metadata) {
    targetCache[imagePath] = metadata;
    targetCache[imagePath]["complete"] = true;
}

void storeMetadataToPersistentCache(const MetadataByPath &metadataByPath, const ProviderOptions &options) {
    if (!options.cacheEnabled || metadataByPath.empty() || options.cacheFilePath.empty()) {
        return;
    }

    std::string cacheError;
    if (!metadata_cache::storeMetadataBatch(options.cacheFilePath, metadataByPath, cacheError)) {
        log_stderr("Metadata cache write failed: ", cacheError);
    }
}

bool isCompleteMetadata(const json &metadata) {
    return metadata.contains("complete") && metadata["complete"] == true;
}

} // namespace

bool findExiftool(std::string &resolvedPath) {
#ifdef _WIN32
    std::string command = "where";
    std::string stderrRedirect = "2>nul";
#else
    std::string command = "which";
    std::string stderrRedirect = "2>/dev/null";
#endif

    FILE *pipe = popen((command + " exiftool " + stderrRedirect).c_str(), "r");
    if (!pipe) {
        log_stdout("No exiftool found");
        resolvedPath.clear();
        return false;
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line = buffer;
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

#ifdef _WIN32
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.size() < 4) {
            continue;
        }
        std::string suffix = lower.substr(lower.size() - 4);
        if (suffix != ".exe" && suffix != ".bat") {
            continue;
        }
#endif

        pclose(pipe);
        resolvedPath = line;
        log_stdout("Exiftool found at ", resolvedPath);
        return true;
    }

    pclose(pipe);
    log_stdout("No exiftool found");
    resolvedPath.clear();
    return false;
}

MetadataByPath extractExiftoolData(const std::vector<fs::path> &imagePaths, const std::string &exiftoolPath) {
    MetadataByPath results;

    if (imagePaths.empty() || exiftoolPath.empty()) {
        return results;
    }

    try {
        std::string cliFlags = "-j -n -q -struct";
        for (const auto &key : EXIF_STRING_KEYS) {
            cliFlags += " -" + key;
        }
        cliFlags += " -Keywords -GPSLatitude -GPSLongitude";

        auto runExifBatch = [&](const std::vector<fs::path> &batch) {
            if (batch.empty()) {
                return;
            }

            std::string cmd;
            std::string exiftoolPathFixed = exiftoolPath;
            std::string imagePathsArg;

            for (size_t i = 0; i < batch.size(); ++i) {
                if (i > 0) {
                    imagePathsArg += "\" \"";
                }
                imagePathsArg += batch[i].string();
            }

#ifdef _WIN32
            for (char &c : exiftoolPathFixed) {
                if (c == '/') {
                    c = '\\';
                }
            }
            cmd = "cmd /c \"\"" + exiftoolPathFixed + "\" " + cliFlags + " \"" + imagePathsArg + "\"\"";
#else
            for (char &c : exiftoolPathFixed) {
                if (c == '\\') {
                    c = '/';
                }
            }
            cmd = "\"" + exiftoolPathFixed + "\" " + cliFlags + " \"" + imagePathsArg + "\"";
#endif

            log_stdout("Exiftool batch command: ", cmd);

            FILE *pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                log_stdout("DEBUG", "popen failed for exiftool");
                return;
            }

            std::string output;
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                output += buffer;
            }
            int exitCode = pclose(pipe);

            log_stdout("DEBUG", "exiftool exit code: ", exitCode, ", output size: ", output.size(), " bytes");
            if (exitCode != 0 || output.empty()) {
                log_stdout("DEBUG", "exiftool failed or returned no output: ", output);
                return;
            }

            auto jsonData = json::parse(output);
            if (jsonData.is_array()) {
                for (auto &record : jsonData) {
                    if (!record.is_object() || !record.contains("Creator")) {
                        continue;
                    }
                    if (record["Creator"].is_array() && !record["Creator"].empty() &&
                        record["Creator"][0].is_string()) {
                        record["Creator"] = record["Creator"][0].get<std::string>();
                    }
                    if (record.contains("Orientation") && record["Orientation"].is_number_integer()) {
                        const int orientation = record["Orientation"].get<int>();
                        if (orientation < 1 || orientation > 8) {
                            record.erase("Orientation");
                        }
                    }
                }
            }
            try {
                jsonData = enrichAndValidateJsonWithSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, jsonData);
            } catch (const std::exception &e) {
                log_stdout("DEBUG", "Exiftool response schema validation failed: ", e.what());
                return;
            }

            std::vector<fs::path> missingRegionInfo;

            for (const auto &record : jsonData) {
                if (!record.is_object() || !record.contains("SourceFile")) {
                    continue;
                }
                try {
                    fs::path imagePath(record["SourceFile"].get<std::string>());
                    results[imagePath] = record;
                    if (!record.contains("RegionInfo") || !record["RegionInfo"].is_object()) {
                        missingRegionInfo.push_back(imagePath);
                    }
                    log_stdout("Extracted metadata for ", imagePath.filename().string());
                } catch (const std::exception &e) {
                    log_stdout("Error processing record: ", e.what());
                }
            }

            if (!missingRegionInfo.empty()) {
                auto fallbackResults = extractImageMetadata(missingRegionInfo);
                for (const auto &imagePath : missingRegionInfo) {
                    auto exifIt = results.find(imagePath);
                    if (exifIt == results.end()) {
                        continue;
                    }

                    auto fallbackIt = fallbackResults.find(imagePath);
                    if (fallbackIt == fallbackResults.end()) {
                        continue;
                    }

                    const json &fallbackMeta = fallbackIt->second;
                    if (!fallbackMeta.is_object() || !fallbackMeta.contains("RegionInfo") ||
                        !fallbackMeta["RegionInfo"].is_object()) {
                        continue;
                    }

                    exifIt->second["RegionInfo"] = fallbackMeta["RegionInfo"];
                }
            }
        };

#ifdef _WIN32
        constexpr size_t maxBatchPaths = 50;
        constexpr size_t maxBatchArgChars = 6000;

        std::vector<fs::path> batch;
        batch.reserve(std::min(maxBatchPaths, imagePaths.size()));
        size_t batchArgChars = 0;

        for (const auto &path : imagePaths) {
            size_t tokenChars = path.string().size() + 3;
            bool flushBatch =
                !batch.empty() && (batch.size() >= maxBatchPaths || (batchArgChars + tokenChars) > maxBatchArgChars);
            if (flushBatch) {
                runExifBatch(batch);
                batch.clear();
                batchArgChars = 0;
            }

            batch.push_back(path);
            batchArgChars += tokenChars;
        }

        if (!batch.empty()) {
            runExifBatch(batch);
        }
#else
        runExifBatch(imagePaths);
#endif
    } catch (const std::exception &e) {
        log_stdout("DEBUG", "Exception in extractExiftoolData: ", e.what());
    }

    return results;
}

json makeIncompleteMetadata(const fs::path &imagePath) {
    json metaObject = json::object();
    metaObject["SourceFile"] = imagePath.string();

    try {
        json enriched = enrichMetadataWithSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, metaObject);
        enriched["complete"] = false;
        return enriched;
    } catch (...) {
        metaObject["complete"] = false;
        return metaObject;
    }
}

bool ensureMetadataForImage(const fs::path &imagePath, MetadataByPath &targetCache, const ProviderOptions &options,
                            const std::function<void(const fs::path &)> &clearThumbnailCache) {
    auto imageIt = targetCache.find(imagePath);
    if (imageIt != targetCache.end() && isCompleteMetadata(imageIt->second)) {
        return true;
    }

    if (imageIt == targetCache.end()) {
        targetCache[imagePath] = makeIncompleteMetadata(imagePath);
    }

    if (options.cacheEnabled && !options.cacheFilePath.empty()) {
        metadata_cache::MetadataByPath cachedMetadata;
        std::vector<fs::path> missingPaths;
        std::string cacheError;
        if (metadata_cache::loadMetadataBatch(options.cacheFilePath, {imagePath}, cachedMetadata, missingPaths,
                                              cacheError)) {
            auto cachedIt = cachedMetadata.find(fs::absolute(imagePath).lexically_normal());
            if (cachedIt == cachedMetadata.end()) {
                cachedIt = cachedMetadata.find(imagePath);
            }
            if (cachedIt != cachedMetadata.end()) {
                markMetadataComplete(targetCache, imagePath, cachedIt->second);
                return true;
            }
        } else {
            log_stderr("Metadata cache read failed: ", cacheError);
        }
    }

    if (clearThumbnailCache) {
        clearThumbnailCache(imagePath);
    }

    MetadataByPath metadataToStore;
    if (options.exiftoolAvailable && !options.exiftoolPath.empty()) {
        auto results = extractExiftoolData({imagePath}, options.exiftoolPath);
        auto it = results.find(imagePath);
        if (it != results.end()) {
            markMetadataComplete(targetCache, imagePath, it->second);
            metadataToStore[imagePath] = it->second;
            storeMetadataToPersistentCache(metadataToStore, options);
            return true;
        }
    }

    auto fallbackResults = extractImageMetadata({imagePath});
    auto fallbackIt = fallbackResults.find(imagePath);
    if (fallbackIt != fallbackResults.end()) {
        markMetadataComplete(targetCache, imagePath, fallbackIt->second);
        metadataToStore[imagePath] = fallbackIt->second;
        storeMetadataToPersistentCache(metadataToStore, options);
        return true;
    }

    targetCache[imagePath] = makeIncompleteMetadata(imagePath);
    return false;
}

void fillMetadataForFolder(const std::vector<fs::path> &imagePaths, MetadataByPath &targetCache,
                           const ProviderOptions &options, bool &deferMetadata, bool &sortByName,
                           const std::function<void(const fs::path &)> &clearThumbnailCache) {
    size_t imageFileCount = 0;
    for (const auto &img : imagePaths) {
        const std::string ext = img.extension().string();
        if (!ext.empty()) {
            imageFileCount++;
        }
    }

    bool tooManyImages = imageFileCount > options.deferImageCountThreshold;
    bool noExiftoolMany = (!options.exiftoolAvailable || options.exiftoolPath.empty()) &&
                          imagePaths.size() > options.noExiftoolBatchThreshold;
    deferMetadata = tooManyImages || noExiftoolMany;
    sortByName = deferMetadata;

    if (deferMetadata) {
        for (const auto &imagePath : imagePaths) {
            if (targetCache.find(imagePath) == targetCache.end()) {
                targetCache[imagePath] = makeIncompleteMetadata(imagePath);
            }
        }
    }

    std::vector<fs::path> cacheMisses = imagePaths;
    if (options.cacheEnabled && !options.cacheFilePath.empty() && !imagePaths.empty()) {
        metadata_cache::MetadataByPath cachedMetadata;
        std::string cacheError;
        if (metadata_cache::loadMetadataBatch(options.cacheFilePath, imagePaths, cachedMetadata, cacheMisses,
                                              cacheError)) {
            for (const auto &[imagePath, metadata] : cachedMetadata) {
                markMetadataComplete(targetCache, imagePath, metadata);
            }
        } else {
            log_stderr("Metadata cache read failed: ", cacheError);
            cacheMisses = imagePaths;
        }
    }

    if (!deferMetadata && !cacheMisses.empty()) {
        if (clearThumbnailCache) {
            for (const auto &imagePath : cacheMisses) {
                clearThumbnailCache(imagePath);
            }
        }

        if (options.exiftoolAvailable && !options.exiftoolPath.empty()) {
            auto exifResults = extractExiftoolData(cacheMisses, options.exiftoolPath);
            for (const auto &[imagePath, metadata] : exifResults) {
                markMetadataComplete(targetCache, imagePath, metadata);
            }
            storeMetadataToPersistentCache(exifResults, options);
        }

        std::vector<fs::path> missing;
        for (const auto &entry : imagePaths) {
            auto imageIt = targetCache.find(entry);
            if (imageIt == targetCache.end() || !isCompleteMetadata(imageIt->second)) {
                missing.push_back(entry);
            }
        }

        if (!missing.empty()) {
            if (clearThumbnailCache) {
                for (const auto &imagePath : missing) {
                    clearThumbnailCache(imagePath);
                }
            }

            auto fallbackResults = extractImageMetadata(missing);
            MetadataByPath metadataToStore;
            for (const auto &entry : missing) {
                auto resultIt = fallbackResults.find(entry);
                if (resultIt != fallbackResults.end()) {
                    markMetadataComplete(targetCache, entry, resultIt->second);
                    metadataToStore[entry] = resultIt->second;
                } else {
                    targetCache[entry] = makeIncompleteMetadata(entry);
                }
            }
            storeMetadataToPersistentCache(metadataToStore, options);
        }
    }
}

} // namespace metadata
