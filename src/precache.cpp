#include "precache.h"

#include "config.h"
#include "metadata.h"
#include "metadata_cache.h"
#include "thumbnail_cache.h"
#include "utils.h"

#include <filesystem>
#include <map>
#include <set>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using ImageMetadataCache = std::map<fs::path, json>;

std::vector<fs::path> findImageFiles(const fs::path &path, const std::vector<std::string> &supportedSuffixes) {
    std::vector<fs::path> results;

    if (fs::is_regular_file(path)) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        for (const auto &suffix : supportedSuffixes) {
            if (ext == suffix) {
                results.push_back(path);
                break;
            }
        }
        return results;
    }

    if (fs::is_directory(path)) {
        for (const auto &entry :
             fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                for (const auto &suffix : supportedSuffixes) {
                    if (ext == suffix) {
                        results.push_back(entry.path());
                        break;
                    }
                }
            }
        }
    }

    return results;
}

std::vector<MapViewer::TileCoord> calculateTilesForView(double latitude, double longitude, int zoom, int windowWidth,
                                                        int windowHeight) {
    std::vector<MapViewer::TileCoord> tiles;

    const int TILE_SIZE = 256;
    double n = std::pow(2.0, zoom);
    double centerPixX = (longitude + 180.0) / 360.0 * n * TILE_SIZE;
    double latRad = latitude * M_PI / 180.0;
    double centerPixY = (1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * n * TILE_SIZE;

    int screenCenterX = windowWidth / 2;
    int screenCenterY = windowHeight / 2;

    int minTileX = static_cast<int>((centerPixX - screenCenterX) / TILE_SIZE) - 1;
    int maxTileX = static_cast<int>((centerPixX + screenCenterX) / TILE_SIZE) + 1;
    int minTileY = static_cast<int>((centerPixY - screenCenterY) / TILE_SIZE) - 1;
    int maxTileY = static_cast<int>((centerPixY + screenCenterY) / TILE_SIZE) + 1;

    int maxTileIndex = (1 << zoom) - 1;
    minTileX = std::max(0, minTileX);
    maxTileX = std::min(maxTileIndex, maxTileX);
    minTileY = std::max(0, minTileY);
    maxTileY = std::min(maxTileIndex, maxTileY);

    for (int tileX = minTileX; tileX <= maxTileX; tileX++) {
        for (int tileY = minTileY; tileY <= maxTileY; tileY++) {
            MapViewer::TileCoord coord;
            coord.x = tileX;
            coord.y = tileY;
            coord.zoom = zoom;
            tiles.push_back(coord);
        }
    }

    return tiles;
}

int runCacheMode(const std::vector<std::string> &paths, const std::string &configPath, bool useExistingThumb,
                 CacheRefreshTarget forceRefreshTarget, bool ignoreDirMtime) {
    log_stdout("Cache prepopulation mode");

    fs::path cacheRoot;
    int defaultZoom = 15;
    unsigned int windowWidth = 1024;
    unsigned int windowHeight = 768;
    std::vector<std::string> supportedSuffixes;

    try {
        fs::path configDir = fs::path(configPath).parent_path();
        json config = loadAndValidateConfig(configDir);

        std::string location = config["map"]["cache"]["location"];
        if (!location.empty()) {
            cacheRoot = location;
        }

        defaultZoom = config["map"]["viewer"]["zoom"]["default"];

        sf::VideoMode desktopMode = sf::VideoMode::getDesktopMode();
        const auto &sizeArray = config["map"]["viewer"]["window"]["size"];
        windowWidth = parseSizeValue(sizeArray[0], desktopMode.size.x);
        windowHeight = parseSizeValue(sizeArray[1], desktopMode.size.y);

        for (const auto &suffix : config["image_file"]["supported_suffixes"]) {
            supportedSuffixes.push_back(suffix.get<std::string>());
        }
    } catch (const std::exception &e) {
        log_stderr("Error loading config: ", e.what());
        throw;
    }

    if (cacheRoot.empty()) {
        cacheRoot = getDefaultCacheLocation();
    }

    log_stdout("Cache root (config): ", cacheRoot.string());

    const int expandedWidth = windowWidth * 3;
    const int expandedHeight = windowHeight * 3;
    log_stdout("Default view: ", windowWidth, "x", windowHeight, " at zoom ", defaultZoom);
    log_stdout("Expanded cache area: ", expandedWidth, "x", expandedHeight, " (to handle panning)");

    fs::path osmCacheDir = cacheRoot / "osm";
    fs::path thumbCacheDir = getThumbnailCacheLocation(cacheRoot);
    fs::create_directories(cacheRoot);
    fs::create_directories(osmCacheDir);
    fs::create_directories(thumbCacheDir);
    log_stdout("Cache root (resolved): ", fs::absolute(cacheRoot).string());

    std::string exiftoolPath;
    bool exiftoolFound = metadata::findExiftool(exiftoolPath);
    g_exiftoolPath = exiftoolPath;
    if (!exiftoolFound || g_exiftoolPath.empty()) {
        log_stderr("Error: exiftool not found. Please install exiftool.");
        return 1;
    }

    log_stdout("Using exiftool: ", g_exiftoolPath);

    std::string metadataCacheError;
    fs::path metadataCacheFile = metadata_cache::defaultMetadataCacheFile(cacheRoot);
    if (metadata_cache::initializeMetadataCache(metadataCacheFile, metadataCacheError)) {
        log_stdout("Metadata cache initialized at: ", pathToString(metadataCacheFile.make_preferred()));
    } else {
        log_stderr("Metadata cache initialization failed: ", metadataCacheError);
        return 1;
    }

    sqlite3 *metadataDb = nullptr;
    if (!metadata_cache::openMetadataCacheDatabase(metadataCacheFile, &metadataDb, metadataCacheError)) {
        log_stderr("Metadata cache open failed: ", metadataCacheError);
        return 1;
    }

    std::vector<fs::path> allFiles;
    std::map<fs::path, std::int64_t> changedDirectoryMtimes;

    auto persistChangedDirectoryMtimes = [&]() {
        if (ignoreDirMtime || changedDirectoryMtimes.empty()) {
            return true;
        }

        std::string dirWriteError;
        if (!metadata_cache::storeDirectoryMtimesBatch(metadataDb, changedDirectoryMtimes, dirWriteError)) {
            log_stderr("Directory mtime cache write failed: ", dirWriteError);
            return false;
        }

        log_stdout("Updated directory mtimes in cache: ", changedDirectoryMtimes.size());
        return true;
    };

    if (ignoreDirMtime) {
        log_stdout("Directory mtime optimization disabled: scanning all files (--ignore-dir-mtime)");
        for (const auto &pathStr : paths) {
            fs::path path(pathStr);
            log_stdout("Scanning: ", pathToString(path.make_preferred()));

            auto files = findImageFiles(path, supportedSuffixes);
            log_stdout("Found ", files.size(), " image file(s)");
            allFiles.insert(allFiles.end(), files.begin(), files.end());
        }
    } else {
        constexpr std::int64_t kDirMtimeToleranceSeconds = 2;
        std::map<fs::path, std::int64_t> inputDirectoryMtimes;

        auto toEpochSeconds = [](fs::file_time_type timePoint) {
            auto systemNow = std::chrono::system_clock::now();
            auto fileNow = fs::file_time_type::clock::now();
            auto systemTime =
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(timePoint - fileNow + systemNow);
            return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
        };

        for (const auto &pathStr : paths) {
            fs::path inputPath(pathStr);
            std::error_code ec;
            fs::path absolutePath = fs::absolute(inputPath, ec);
            if (ec) {
                log_stderr("Skipping path (cannot resolve absolute path): ", pathToString(inputPath.make_preferred()));
                continue;
            }
            absolutePath = absolutePath.lexically_normal();

            if (fs::is_regular_file(absolutePath, ec)) {
                if (ec) {
                    log_stderr("Skipping path (cannot stat file): ", pathToString(absolutePath.make_preferred()));
                    continue;
                }
                fs::path parent = absolutePath.parent_path();
                auto dirTime = fs::last_write_time(parent, ec);
                if (ec) {
                    log_stderr("Skipping directory mtime read: ", pathToString(parent.make_preferred()));
                    continue;
                }
                inputDirectoryMtimes[parent] = toEpochSeconds(dirTime);
                continue;
            }

            if (!fs::is_directory(absolutePath, ec) || ec) {
                log_stderr("Skipping path (not a file/directory): ", pathToString(absolutePath.make_preferred()));
                continue;
            }

            auto rootTime = fs::last_write_time(absolutePath, ec);
            if (!ec) {
                inputDirectoryMtimes[absolutePath] = toEpochSeconds(rootTime);
            }

            for (const auto &entry :
                 fs::recursive_directory_iterator(absolutePath, fs::directory_options::skip_permission_denied)) {
                if (!entry.is_directory(ec) || ec) {
                    ec.clear();
                    continue;
                }
                auto dirTime = entry.last_write_time(ec);
                if (ec) {
                    ec.clear();
                    continue;
                }
                inputDirectoryMtimes[entry.path().lexically_normal()] = toEpochSeconds(dirTime);
            }
        }

        log_stdout("Enumerated directories: ", inputDirectoryMtimes.size());

        std::vector<fs::path> enumeratedDirs;
        enumeratedDirs.reserve(inputDirectoryMtimes.size());
        for (const auto &[dirPath, _] : inputDirectoryMtimes) {
            (void)_;
            enumeratedDirs.push_back(dirPath);
        }

        std::map<fs::path, std::int64_t> cachedDirectoryMtimes;
        std::string dirReadError;
        if (!metadata_cache::loadDirectoryMtimesBatch(metadataDb, enumeratedDirs, cachedDirectoryMtimes,
                                                      dirReadError)) {
            log_stderr("Directory mtime cache read failed: ", dirReadError);
            sqlite3_close(metadataDb);
            return 1;
        }

        std::vector<fs::path> changedDirs;
        changedDirs.reserve(inputDirectoryMtimes.size());
        for (const auto &[dirPath, currentMtime] : inputDirectoryMtimes) {
            auto cachedIt = cachedDirectoryMtimes.find(dirPath);
            if (cachedIt == cachedDirectoryMtimes.end() ||
                std::llabs(cachedIt->second - currentMtime) > kDirMtimeToleranceSeconds) {
                changedDirs.push_back(dirPath);
                changedDirectoryMtimes[dirPath] = currentMtime;
            }
        }

        log_stdout("Directories changed/new: ", changedDirs.size());

        std::unordered_set<std::string> seenFiles;
        for (const auto &dirPath : changedDirs) {
            std::error_code ec;
            if (!fs::is_directory(dirPath, ec) || ec) {
                continue;
            }

            for (const auto &entry : fs::directory_iterator(dirPath, fs::directory_options::skip_permission_denied)) {
                if (!entry.is_regular_file(ec) || ec) {
                    ec.clear();
                    continue;
                }

                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                bool supported = false;
                for (const auto &suffix : supportedSuffixes) {
                    if (ext == suffix) {
                        supported = true;
                        break;
                    }
                }
                if (!supported) {
                    continue;
                }

                fs::path normalized = fs::absolute(entry.path(), ec).lexically_normal();
                if (ec) {
                    ec.clear();
                    continue;
                }

                std::string dedupeKey = normalized.generic_string();
                std::transform(dedupeKey.begin(), dedupeKey.end(), dedupeKey.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (seenFiles.insert(dedupeKey).second) {
                    allFiles.push_back(normalized);
                }
            }
        }
    }

    if (allFiles.empty()) {
        if (ignoreDirMtime) {
            log_stderr("No image files found");
            return 1;
        }

        if (!persistChangedDirectoryMtimes()) {
            return 1;
        }

        if (changedDirectoryMtimes.empty()) {
            log_stdout("No image files need refresh (directory mtimes unchanged)");
        } else {
            log_stdout("No image files need refresh (no supported files in changed directories)");
        }
        return 0;
    }

    log_stdout("Total image files selected for processing: ", allFiles.size());

    const size_t batchSize = 50;
    bool hadErrors = false;
    size_t totalCacheHits = 0;
    size_t totalCacheMisses = 0;

    if (useExistingThumb) {
        log_stdout("Thumbnail mode: --use-existing-thumb enabled (existing thumbnails are kept as-is)");
    }
    if (forceRefreshTarget != CacheRefreshTarget::None) {
        log_stdout("Cache force refresh enabled for: ", cacheRefreshTargetName(forceRefreshTarget));
    }

    auto hasValidGps = [](const json &meta) {
        return meta.contains("GPSLatitude") && meta["GPSLatitude"].is_number() && meta.contains("GPSLongitude") &&
               meta["GPSLongitude"].is_number();
    };

    std::set<std::tuple<int, int, int>> uniqueTiles;
    const char *TILE_SERVER_HOST = "tile.openstreetmap.org";
    const char *USER_AGENT = "mgvwr/1.0";
    int downloadedCount = 0;
    int skippedCount = 0;
    int validGPSCount = 0;
    auto lastDownloadTime = std::chrono::steady_clock::now();

    auto processTile = [&](int x, int y, int zoom) {
        const auto tileCoord = std::make_tuple(x, y, zoom);
        if (!uniqueTiles.insert(tileCoord).second) {
            return;
        }

        fs::path tilePath = osmCacheDir / std::to_string(zoom) / std::to_string(x) / (std::to_string(y) + ".png");

        if (forceRefreshTarget == CacheRefreshTarget::MapTile) {
            std::error_code ec;
            fs::remove(tilePath, ec);
        }

        if (fs::exists(tilePath) && fs::file_size(tilePath) > 0) {
            skippedCount++;
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDownloadTime);
        if (elapsed.count() < 250) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250 - elapsed.count()));
        }

        std::string url = "https://" + std::string(TILE_SERVER_HOST) + "/" + std::to_string(zoom) + "/" +
                          std::to_string(x) + "/" + std::to_string(y) + ".png";

        fs::create_directories(tilePath.parent_path());

        std::string command =
            "curl -s -f -L -A \"" + std::string(USER_AGENT) + "\" -o \"" + tilePath.string() + "\" \"" + url + "\"";

        int result = system(command.c_str());
        lastDownloadTime = std::chrono::steady_clock::now();

        if (result == 0 && fs::exists(tilePath) && fs::file_size(tilePath) > 0) {
            downloadedCount++;
            log_stdout("Downloaded (", downloadedCount, "/", (downloadedCount + skippedCount), "): ", zoom, "/", x,
                       "/", y);
        } else {
            if (fs::exists(tilePath)) {
                fs::remove(tilePath);
            }
            log_stderr("Failed to download: ", zoom, "/", x, "/", y);
        }
    };

    log_stdout("Caching metadata, thumbnails, and tiles in batches of ", batchSize, " image file(s)...");
    for (size_t i = 0; i < allFiles.size(); i += batchSize) {
        size_t end = std::min(i + batchSize, allFiles.size());
        std::vector<fs::path> batch(allFiles.begin() + i, allFiles.begin() + end);

        metadata_cache::SourceSnapshotByPath batchSourceSnapshots;
        metadata_cache::MetadataByPath batchCachedMetadata;
        std::vector<fs::path> batchMisses = batch;
        std::string cacheReadError;

        if (forceRefreshTarget == CacheRefreshTarget::Metadata) {
            std::string cacheDeleteError;
            if (!metadata_cache::deleteMetadataBatch(metadataDb, batch, cacheDeleteError)) {
                log_stderr("Metadata cache delete failed for batch ", (i + 1), "-", end, ": ", cacheDeleteError);
                sqlite3_close(metadataDb);
                return 1;
            }
        }

        bool batchCacheReadOk = metadata_cache::loadMetadataBatch(metadataDb, batch, batchCachedMetadata, batchMisses,
                                                                  cacheReadError, &batchSourceSnapshots);

        if (!batchCacheReadOk && !cacheReadError.empty()) {
            log_stderr("Metadata cache read failed for batch ", (i + 1), "-", end, ": ", cacheReadError);
        }

        totalCacheHits += batchCachedMetadata.size();
        totalCacheMisses += batchMisses.size();

        ImageMetadataCache batchMetadata;
        batchMetadata.insert(batchCachedMetadata.begin(), batchCachedMetadata.end());

        if (!batchMisses.empty()) {
            log_stdout("Extracting metadata from cache-miss files ", (i + 1), "-", end, "...");
            auto extractedMetadata = metadata::extractExiftoolData(batchMisses, g_exiftoolPath);
            batchMetadata.insert(extractedMetadata.begin(), extractedMetadata.end());

            if (!extractedMetadata.empty() &&
                !metadata_cache::storeMetadataBatch(metadataDb, extractedMetadata, metadataCacheError)) {
                log_stderr("Metadata cache write failed for batch ", (i + 1), "-", end, ": ", metadataCacheError);
                hadErrors = true;
            } else if (!extractedMetadata.empty()) {
                log_stdout("Stored metadata batch ", (i + 1), "-", end, " to cache");
            }
        }

        for (const auto &imagePath : batch) {
            fs::path thumbCacheFile = getThumbnailCacheFilePath(imagePath, cacheRoot);
            const bool reuseExistingThumb =
                useExistingThumb && forceRefreshTarget != CacheRefreshTarget::Thumbnail && fs::exists(thumbCacheFile);

            if (!reuseExistingThumb) {
                std::error_code ec;
                fs::create_directories(thumbCacheFile.parent_path(), ec);
                if (ec) {
                    log_stderr("Failed to create thumbnail cache directory for ", imagePath.string(), ": ",
                               ec.message());
                    continue;
                }

                if (forceRefreshTarget == CacheRefreshTarget::Thumbnail) {
                    std::error_code removeEc;
                    fs::remove(thumbCacheFile, removeEc);
                    fs::remove(getThumbnailCacheMetaFilePath(thumbCacheFile), removeEc);
                }

                std::error_code absEc;
                fs::path absoluteImagePath = fs::absolute(imagePath, absEc);
                if (!absEc) {
                    absoluteImagePath = absoluteImagePath.lexically_normal();
                }

                if (!absEc && batchCachedMetadata.find(absoluteImagePath) != batchCachedMetadata.end()) {
                    if (!fs::exists(thumbCacheFile) && !writeThumbnailCacheFile(imagePath, thumbCacheFile)) {
                        log_stderr("Failed to pre-cache thumbnail for ", imagePath.string());
                    }
                } else {
                    const auto snapshotIt =
                        (!absEc) ? batchSourceSnapshots.find(absoluteImagePath) : batchSourceSnapshots.end();
                    if (snapshotIt != batchSourceSnapshots.end()) {
                        if (!thumbnailCacheMatchesSource(thumbCacheFile, snapshotIt->second.mtime,
                                                         snapshotIt->second.size)) {
                            if (!writeThumbnailCacheFile(imagePath, thumbCacheFile)) {
                                log_stderr("Failed to pre-cache thumbnail for ", imagePath.string());
                            }
                        }
                    } else if (!ensureThumbnailCacheFileOnDisk(imagePath, thumbCacheFile)) {
                        log_stderr("Failed to pre-cache thumbnail for ", imagePath.string());
                    }
                }
            }

            auto metaIt = batchMetadata.find(imagePath);
            if (metaIt == batchMetadata.end() || !hasValidGps(metaIt->second)) {
                continue;
            }

            validGPSCount++;
            const json &meta = metaIt->second;
            double lat = meta["GPSLatitude"].get<double>();
            double lon = meta["GPSLongitude"].get<double>();
            auto tiles = calculateTilesForView(lat, lon, defaultZoom, expandedWidth, expandedHeight);

            for (const auto &tile : tiles) {
                processTile(tile.x, tile.y, tile.zoom);
            }
        }

        log_stdout("Processed cache batch ", (i + 1), "-", end, " (hits: ", batchCachedMetadata.size(),
                   ", misses: ", batchMisses.size(), ")");
    }

    if (!persistChangedDirectoryMtimes()) {
        sqlite3_close(metadataDb);
        return 1;
    }

    sqlite3_close(metadataDb);

    log_stdout("Metadata cache hits: ", totalCacheHits, " / ", allFiles.size(), " (misses: ", totalCacheMisses, ")");
    log_stdout("Thumbnail pre-cache complete");

    log_stdout("Files with valid GPS: ", validGPSCount, "/", allFiles.size());

    if (validGPSCount == 0) {
        log_stderr("No files with GPS coordinates found");
        return 1;
    }

    log_stdout("Total unique tiles needed: ", uniqueTiles.size());

    log_stdout("Caching complete!");
    log_stdout("Downloaded: ", downloadedCount, " tiles");
    log_stdout("Already cached: ", skippedCount, " tiles");

    return hadErrors ? 1 : 0;
}
