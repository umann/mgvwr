#include "thumbnail_cache.h"

#include "utils.h"
#include <SFML/Graphics.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path getDefaultCacheLocation() {
    const char *localAppDataEnv = std::getenv("LOCALAPPDATA");
    const char *homeEnv = std::getenv("HOME");

    if (localAppDataEnv != nullptr) {
        return fs::path(localAppDataEnv) / "Umann" / "MgVwr" / "cache";
    }
    if (homeEnv != nullptr) {
        return fs::path(homeEnv) / ".cache" / "umann" / "mgvwr";
    }
    return fs::path(".") / "cache";
}

fs::path getThumbnailCacheLocation(const fs::path &cacheRoot) {
    return cacheRoot / "thumb";
}

fs::path getThumbnailCacheLocation() {
    return getThumbnailCacheLocation(getDefaultCacheLocation());
}

static std::string hashThumbnailCacheKey(const fs::path &imagePath) {
    std::string payload = normalizePath(imagePath).string();

    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : payload) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

fs::path getThumbnailCacheFilePath(const fs::path &imagePath, const fs::path &cacheRoot) {
    std::string cacheKey = hashThumbnailCacheKey(imagePath);
    return getThumbnailCacheLocation(cacheRoot) / cacheKey.substr(0, 2) / (cacheKey + ".png");
}

fs::path getThumbnailCacheMetaFilePath(const fs::path &thumbCacheFile) {
    return fs::path(thumbCacheFile.string() + ".json");
}

static bool readThumbnailSourceSnapshot(const fs::path &imagePath, std::int64_t &mtime, std::int64_t &size) {
    std::error_code ec;
    auto fileSize = fs::file_size(imagePath, ec);
    if (ec) {
        return false;
    }

    auto fileTime = fs::last_write_time(imagePath, ec);
    if (ec) {
        return false;
    }

    auto toEpochSeconds = [](fs::file_time_type timePoint) {
        auto systemNow = std::chrono::system_clock::now();
        auto fileNow = fs::file_time_type::clock::now();
        auto systemTime =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(timePoint - fileNow + systemNow);
        return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
    };

    mtime = toEpochSeconds(fileTime);
    size = static_cast<std::int64_t>(fileSize);
    return true;
}

bool thumbnailCacheMatchesSource(const fs::path &imagePath, const fs::path &thumbCacheFile) {
    std::error_code ec;
    if (!fs::exists(thumbCacheFile, ec) || ec) {
        return false;
    }

    fs::path metaFile = getThumbnailCacheMetaFilePath(thumbCacheFile);
    if (!fs::exists(metaFile, ec) || ec) {
        return false;
    }

    std::ifstream input(metaFile);
    if (!input) {
        return false;
    }

    json meta;
    try {
        input >> meta;
    } catch (...) {
        return false;
    }

    std::int64_t sourceMtime = 0;
    std::int64_t sourceSize = 0;
    if (!readThumbnailSourceSnapshot(imagePath, sourceMtime, sourceSize)) {
        return false;
    }

    return meta.is_object() && meta.value("mtime", std::int64_t(-1)) == sourceMtime &&
           meta.value("size", std::int64_t(-1)) == sourceSize;
}

bool thumbnailCacheMatchesSource(const fs::path &thumbCacheFile, std::int64_t sourceMtime, std::int64_t sourceSize) {
    std::error_code ec;
    if (!fs::exists(thumbCacheFile, ec) || ec) {
        return false;
    }

    fs::path metaFile = getThumbnailCacheMetaFilePath(thumbCacheFile);
    if (!fs::exists(metaFile, ec) || ec) {
        return false;
    }

    std::ifstream input(metaFile);
    if (!input) {
        return false;
    }

    json meta;
    try {
        input >> meta;
    } catch (...) {
        return false;
    }

    return meta.is_object() && meta.value("mtime", std::int64_t(-1)) == sourceMtime &&
           meta.value("size", std::int64_t(-1)) == sourceSize;
}

bool ensureThumbnailCacheFileOnDisk(const fs::path &imagePath, const fs::path &thumbCacheFile) {
    if (thumbnailCacheMatchesSource(imagePath, thumbCacheFile)) {
        return true;
    }
    return writeThumbnailCacheFile(imagePath, thumbCacheFile);
}

bool writeThumbnailCacheFile(const fs::path &imagePath, const fs::path &thumbCacheFile) {
    sf::Texture sourceTexture;
    if (!sourceTexture.loadFromFile(imagePath.string())) {
        return false;
    }

    auto sourceImageSize = sourceTexture.getSize();
    const unsigned int maxThumbEdge = 256;
    float scaleX = static_cast<float>(maxThumbEdge) / static_cast<float>(sourceImageSize.x);
    float scaleY = static_cast<float>(maxThumbEdge) / static_cast<float>(sourceImageSize.y);
    float scale = std::min(scaleX, scaleY);
    unsigned int thumbWidth = std::max(1u, static_cast<unsigned int>(std::round(sourceImageSize.x * scale)));
    unsigned int thumbHeight = std::max(1u, static_cast<unsigned int>(std::round(sourceImageSize.y * scale)));

    sf::RenderTexture thumbTarget;
    if (!thumbTarget.resize(sf::Vector2u(thumbWidth, thumbHeight))) {
        return false;
    }

    thumbTarget.clear(sf::Color::Transparent);
    sf::Sprite thumbSprite(sourceTexture);
    thumbSprite.setScale({scale, scale});
    thumbTarget.draw(thumbSprite);
    thumbTarget.display();

    sf::Image thumbImage = thumbTarget.getTexture().copyToImage();
    if (!thumbImage.saveToFile(thumbCacheFile)) {
        log_stdout("DEBUG", "Failed to save thumbnail cache file: ", thumbCacheFile.string());
        return false;
    }

    std::int64_t sourceMtime = 0;
    std::int64_t sourceSize = 0;
    if (readThumbnailSourceSnapshot(imagePath, sourceMtime, sourceSize)) {
        json meta = json::object();
        meta["mtime"] = sourceMtime;
        meta["size"] = sourceSize;
        std::ofstream metaOut(getThumbnailCacheMetaFilePath(thumbCacheFile));
        if (metaOut) {
            metaOut << meta.dump();
        }
    }

    return true;
}

unsigned int parseSizeValue(const std::string &sizeStr, unsigned int maxValue) {
    if (sizeStr.empty())
        return maxValue;

    if (sizeStr.back() == '%') {
        try {
            float percentage = std::stof(sizeStr.substr(0, sizeStr.size() - 1));
            return static_cast<unsigned int>(maxValue * percentage / 100.0f);
        } catch (...) {
            return maxValue;
        }
    }

    try {
        return static_cast<unsigned int>(std::stoul(sizeStr));
    } catch (...) {
        return maxValue;
    }
}

unsigned int parseSizeValue(const nlohmann::json &v, unsigned int maxValue) {
    if (v.is_null())
        return maxValue;
    if (v.is_number_unsigned())
        return v.get<unsigned int>();
    if (v.is_number_integer()) {
        auto x = v.get<long long>();
        return x > 0 ? static_cast<unsigned int>(x) : 0u;
    }
    if (v.is_number_float()) {
        auto x = v.get<double>();
        return x > 0 ? static_cast<unsigned int>(x) : 0u;
    }
    if (v.is_string()) {
        return parseSizeValue(v.get_ref<const std::string &>(), maxValue);
    }
    return maxValue;
}

std::optional<CacheRefreshTarget> parseCacheRefreshTarget(const std::string &value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "metadata") {
        return CacheRefreshTarget::Metadata;
    }
    if (normalized == "thumb") {
        return CacheRefreshTarget::Thumbnail;
    }
    if (normalized == "map_tile") {
        return CacheRefreshTarget::MapTile;
    }
    return std::nullopt;
}

std::string cacheRefreshTargetName(CacheRefreshTarget target) {
    switch (target) {
    case CacheRefreshTarget::Metadata:
        return "metadata";
    case CacheRefreshTarget::Thumbnail:
        return "thumb";
    case CacheRefreshTarget::MapTile:
        return "map_tile";
    case CacheRefreshTarget::None:
        return "";
    }
    return "";
}
