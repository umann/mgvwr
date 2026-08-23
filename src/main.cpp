#include "config.h"
#include "datetime_utils.h"
#include "encoding_utils.h"
#include "exiftool_response_schema.h"
#include "help.h"
#include "map_viewer.h"
#include "metadata.h"
#include "metadata_cache.h"
#include "poor_mans_exiftool.h"
#include "precache.h"
#include "thumbnail_cache.h"
#include "utils.h"
#include <SFML/Graphics.hpp>
#include <SFML/Window/Clipboard.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <ranges>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <yaml-cpp/yaml.h>

#ifdef _WIN32
#include <regex>
#include <windows.h>
#endif

#ifdef _WIN32
#include <shellapi.h>
#include <tlhelp32.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

using ImageMetadataCache = std::map<fs::path, json>;

std::string g_exiftoolPath;

// POOR_MANS_SUPPORTED_SUFFIXES = {".jpg", ".jpeg"}

// Path classification structure
struct PathClassification {
    std::string pattern;
    std::vector<std::string> names;
};

// Classify the navigation type between two folders
std::string classifyNavigation(const fs::path &oldFolder, const fs::path &newFolder,
                               const std::vector<PathClassification> &classifications) {
    std::string oldPath = oldFolder.string();
    std::string newPath = newFolder.string();

    // Convert backslashes to forward slashes for consistent regex
    std::replace(oldPath.begin(), oldPath.end(), '\\', '/');
    std::replace(newPath.begin(), newPath.end(), '\\', '/');

    // Loop through all path classifications from config
    for (const auto &classification : classifications) {
        try {
            std::regex pattern(classification.pattern);
            std::smatch oldMatch, newMatch;

            // Check if both paths match this pattern
            if (std::regex_search(oldPath, oldMatch, pattern) && std::regex_search(newPath, newMatch, pattern)) {

                // Validate match count against classification names count
                // oldMatch[0] is the full match, groups start at [1]
                size_t oldGroupCount = oldMatch.size() - 1;
                size_t newGroupCount = newMatch.size() - 1;

                if (oldGroupCount != newGroupCount) {
                    throw std::runtime_error("Pattern match count mismatch: old=" + std::to_string(oldGroupCount) +
                                             " new=" + std::to_string(newGroupCount));
                }

                if (oldGroupCount != classification.names.size()) {
                    throw std::runtime_error("Pattern group count (" + std::to_string(oldGroupCount) +
                                             ") does not match names count (" +
                                             std::to_string(classification.names.size()) + ")");
                }

                // Loop through match groups in parallel to find first difference
                for (size_t i = 0; i < classification.names.size(); ++i) {
                    std::string oldGroup = oldMatch[i + 1].str(); // +1 to skip full match
                    std::string newGroup = newMatch[i + 1].str();

                    if (oldGroup != newGroup) {
                        // Return the corresponding name for first difference
                        return classification.names[i];
                    }
                }
            }
        } catch (const std::regex_error &e) {
            log_stderr("Invalid regex pattern in path_classification: ", classification.pattern);
        }
    }

    // Default to "folder" if no pattern matched
    return "folder";
}

static YAML::Node jsonToYamlNode(const json &value) {
    if (value.is_object()) {
        YAML::Node node(YAML::NodeType::Map);
        for (auto it = value.begin(); it != value.end(); ++it) {
            node[it.key()] = jsonToYamlNode(it.value());
        }
        return node;
    }
    if (value.is_array()) {
        YAML::Node node(YAML::NodeType::Sequence);
        for (const auto &item : value) {
            node.push_back(jsonToYamlNode(item));
        }
        return node;
    }
    if (value.is_string()) {
        return YAML::Node(value.get<std::string>());
    }
    if (value.is_boolean()) {
        return YAML::Node(value.get<bool>());
    }
    if (value.is_number_integer()) {
        return YAML::Node(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return YAML::Node(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        return YAML::Node(value.get<double>());
    }
    return YAML::Node();
}

// Enforce single instance mode - terminate any existing instance
// Returns true if an existing instance was terminated, false otherwise
bool enforceSingleInstance() {
#ifdef _WIN32
    const wchar_t *mutexName = L"MgVwrSingleInstance_MUTEX";

    HANDLE hMutex = CreateMutexW(NULL, FALSE, mutexName);
    if (!hMutex) {
        log_stdout("Failed to create mutex");
        return false;
    }

    DWORD dwWaitResult = WaitForSingleObject(hMutex, 0);
    if (dwWaitResult != WAIT_OBJECT_0) {
        // Mutex already owned by another process - find and kill it
        log_stdout("Another instance detected. Terminating it...");

        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);

        HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hProcessSnap != INVALID_HANDLE_VALUE) {
            std::string exeName = "mgvwr.exe";
            DWORD currentPid = GetCurrentProcessId();
            bool terminated = false;

            if (Process32First(hProcessSnap, &pe32)) {
                do {
                    // pe32.szExeFile is already char[], just compare directly
                    std::string processName(pe32.szExeFile);

                    if (exeName == processName && pe32.th32ProcessID != currentPid) {
                        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                        if (hProcess) {
                            TerminateProcess(hProcess, 0);
                            CloseHandle(hProcess);
                            log_stdout("Terminated process ID ", pe32.th32ProcessID);
                            terminated = true;
                            Sleep(500); // Give it time to shut down
                        }
                    }
                } while (Process32Next(hProcessSnap, &pe32));
            }
            CloseHandle(hProcessSnap);
        }

        // Try to acquire the mutex again after killing old instance
        hMutex = CreateMutexW(NULL, FALSE, mutexName);
        if (hMutex) {
            WaitForSingleObject(hMutex, INFINITE);
        }
        return true; // An instance was terminated
    }
    // Keep mutex open for the lifetime of the app - it will be released when process exits
    return false; // No instance was terminated
#else
    return false;
#endif
}

// Configuration structures
struct Filter {
    std::string expression;
    std::string key;
    std::string pattern; // Parsed from expression
};

struct Map {
    std::string name;
    int zoom = 0;
    std::string gui_url_template;
};

class MgVwr {
  private:
    json config;
    std::vector<fs::path> allImagePaths;
    std::vector<fs::path> allDirectories;
    fs::path currentWatchedFolder;
    fs::path currentFolder; // Currently displayed folder
    bool folderModeEligible = false;
    size_t currentIndex = 0;
    std::shared_ptr<sf::RenderWindow> window;
    std::shared_ptr<sf::Sprite> sprite;
    std::shared_ptr<sf::Texture> texture;
    std::shared_ptr<sf::Texture> precachedTexture;
    sf::Font uiFont;
    bool uiFontLoaded = false;
    sf::VideoMode desktopMode;
    sf::Vector2u windowedSize;
    sf::Vector2i windowedPosition;
    std::string windowTitle;
    bool isFullscreen = true;
    bool hasStoredWindowState = false;
    unsigned int fullscreenWidth; // Cached fullscreen width for font calculations
    fs::path appIconPath;
    sf::Image appIconImage;
    bool appIconLoaded = false;
    bool appIconLoadAttempted = false;

    // Image metadata cache: path -> { EXIF strings, Keywords array, GPSLatitude/GPSLongitude }
    ImageMetadataCache imageMetadataCache;

    // Folder cache: folder path -> (image paths, metadata, directory)
    struct FolderCache {
        std::vector<fs::path> images;
        ImageMetadataCache metadata;
        bool sortByName = false;
        bool deferMetadata = false;
        fs::path folderPath;
    };
    std::map<fs::path, FolderCache> folderCaches;

    bool jumpedToOldest = false;
    bool exiftoolAvailable = false;
    bool hasShownFirstImage = false;
    bool wasReloaded = false; // Track if we terminated another instance
    bool metadataCacheReady = false;
    fs::path metadataCacheFilePath;

    // Navigation messages
    std::string navigationMessage;
    float navigationMessageTime = 0.0f;
    bool thumbnailCollectionMessageActive = false;
    bool metadataCollectionMessageActive = false;
    bool mapTileDownloadMessageActive = false;

    // Font configuration
    json fontSizeConfig;

    // Config values
    bool quietMode;
    bool singleInstanceMode;
    bool experimental;
    std::map<fs::path, bool> watchedFolderAutoScan;
    std::vector<fs::path> watchedFolders;
    bool windowModeIsDefault;
    json defaultWindowWidth;
    json defaultWindowHeight;
    std::string homeCountry;
    std::string geoKeywordPrefix;
    std::vector<std::string> regions;

    // Cache configuration
    bool cacheEnabled;
    std::string cacheLocation;
    size_t maxCacheSizeMB;
    json mapWindowWidth;
    json mapWindowHeight;
    int defaultZoom;
    int minZoom;
    int maxZoom;

    // Pre-caching thread and key queue
    std::thread preCacheThread;
    std::queue<sf::Event::KeyPressed> pendingKeyPresses;
    std::mutex keyQueueMutex;
    bool isPreCaching = false;

    // Filter system
    int activeFilterIndex = -1; // -1 means no filter active
    std::vector<Filter> filters;

    // Maps system
    std::vector<std::pair<int, sf::FloatRect>>
        mapLinkAreas; // Store (map_index, clickable_area) for each displayed map link
    std::vector<Map> maps;
    std::unique_ptr<MapViewer> mapViewer;
    bool sortByNameCurrentFolder = false;
    bool deferMetadataCurrentFolder = false;
    bool isHandCursorActive = false; // Track if hand cursor is currently set
    float fitImageScale = 1.0f;
    float currentImageScale = 1.0f;

    // Thumbnail mode
    bool thumbnailMode = false;
    int thumbnailColumns = 8;
    int thumbnailScrollRow = 0;
    bool thumbnailScrollbarDragging = false;
    float thumbnailScrollbarDragOffset = 0.0f;
    std::vector<std::pair<size_t, sf::FloatRect>> thumbnailClickAreas;
    struct FolderModeEntry {
        fs::path folderPath;
        fs::path representativeImage;
        std::string label;
        bool isParentPlaceholder = false;
    };
    bool folderMode = false;
    bool watchedFoldersMode = false;
    std::vector<FolderModeEntry> folderModeEntries;
    std::vector<std::pair<size_t, sf::FloatRect>> folderModeClickAreas;
    size_t folderModeFocusIndex = 0;
    std::vector<fs::path> seenImageFolders;
    std::map<fs::path, std::shared_ptr<sf::Texture>> thumbnailTextureCache;
    std::deque<size_t> thumbnailLoadQueue;
    std::unordered_set<size_t> thumbnailQueuedIndices;
    std::unordered_set<size_t> thumbnailReadyIndices;
    std::deque<size_t> folderThumbnailLoadQueue;
    std::unordered_set<size_t> folderThumbnailQueuedIndices;
    std::unordered_set<size_t> folderThumbnailReadyIndices;
    bool folderThumbQueueSeeded = false;
    std::chrono::steady_clock::time_point lastThumbnailClickTime = std::chrono::steady_clock::time_point::min();
    size_t lastThumbnailClickedIndex = std::numeric_limits<size_t>::max();
    std::chrono::steady_clock::time_point lastFolderModeClickTime = std::chrono::steady_clock::time_point::min();
    size_t lastFolderModeClickedIndex = std::numeric_limits<size_t>::max();
    std::chrono::steady_clock::time_point lastBackspaceEnterThumbnailTime =
        std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point lastSearchSubmitTime = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point lastSearchCursorBlinkTime = std::chrono::steady_clock::now();
    bool searchCursorVisible = true;
    fs::path pendingFolderModeFocusFolder;

    // Navigation arrow system
    enum class NavArrow { Left, Right, Up, Down };
    std::vector<std::pair<NavArrow, sf::FloatRect>>
        navArrowAreas; // Store (arrow_direction, clickable_area) for navigation arrows

    // Help system
    bool showHelp = false;
    std::vector<std::string> helpLines;

    enum class ContextMenuAction {
        None,
        CopyImagePath,
        CopyCoordinates,
        ToggleMap,
        OpenGoogleMaps,
        ShowAllInMap,
        ToggleThumbnails,
        ShowInFolder,
        ToggleFullscreen,
        ShowHelp,
    };

    struct ContextMenuItem {
        std::string label;
        bool enabled = true;
        ContextMenuAction action = ContextMenuAction::None;
        int mapIndex = -1;
    };

    bool contextMenuVisible = false;
    sf::Vector2f contextMenuPos{0.f, 0.f};
    std::vector<ContextMenuItem> contextMenuItems;

    struct SearchSuggestion {
        std::string token;
        std::string display;
        std::int64_t imageCount = 0;
    };

    struct SearchSnapshot {
        bool valid = false;
        bool thumbnailMode = false;
        bool folderMode = false;
        bool watchedFoldersMode = false;
        bool searchResultsActive = false;
        fs::path currentFolder;
        fs::path currentWatchedFolder;
        std::vector<fs::path> allImagePaths;
        std::vector<fs::path> allDirectories;
        size_t currentIndex = 0;
        int thumbnailScrollRow = 0;
        size_t folderModeFocusIndex = 0;
        std::vector<size_t> searchMatchedIndices;
    };

    // Search UI state
    bool searchUiOpen = false;
    bool searchInputFocused = false;
    bool searchResultsActive = false;
    std::vector<size_t> searchMatchedIndices;
    std::vector<std::string> searchTokens;
    std::string searchPrefix;
    size_t searchPrefixCursor = 0;
    std::vector<SearchSuggestion> searchSuggestions;
    int highlightedSearchSuggestion = -1;
    bool searchZeroMatchesHint = false;
    SearchSnapshot searchSnapshot;

    sf::FloatRect hamburgerButtonRect;
    sf::FloatRect searchOpenButtonRect;
    sf::FloatRect searchInputHitRect;
    sf::FloatRect searchSubmitButtonRect;
    sf::FloatRect searchDismissButtonRect;
    std::vector<std::pair<size_t, sf::FloatRect>> searchTokenDismissRects;
    std::vector<std::pair<size_t, sf::FloatRect>> searchSuggestionRects;

    float topLeftInfoBoxWidth = 520.0f;

    std::unordered_map<std::uint32_t, std::string> searchUnidecodeMap;

    std::vector<std::string> supportedSuffixes;
    std::vector<PathClassification> pathClassifications;

    float getInlineMapY(unsigned int windowHeight, unsigned int mapHeight) const {
        if (windowHeight <= mapHeight) {
            return 0.0f;
        }
        return static_cast<float>(windowHeight - mapHeight);
    }

    void drawNoGpsOverlayAt(float mapX, float mapY, float mapWidth, float mapHeight) {
        sf::RectangleShape overlay(sf::Vector2f(mapWidth, mapHeight));
        overlay.setPosition(sf::Vector2f(mapX, mapY));
        overlay.setFillColor(sf::Color::Black);
        window->draw(overlay);

        if (uiFontLoaded) {
            const std::string noMapText = "No map for this image (no GPS coordinates)";
            sf::Text text(uiFont, noMapText, getCalculatedFontSize());
            text.setFillColor(sf::Color::Red);
            sf::FloatRect bounds = text.getLocalBounds();
            float textX = mapX + std::max(8.f, (mapWidth - bounds.size.x) * 0.5f);
            float textY = mapY + (mapHeight - bounds.size.y) * 0.5f;
            text.setPosition(sf::Vector2f(textX, textY));
            window->draw(text);
        }
    }

    void drawNoGpsInlineMapPlaceholder() {
        if (!window) {
            return;
        }

        auto windowSize = window->getSize();
        float mapWidth = static_cast<float>(parseSizeValue(mapWindowWidth, windowSize.x));
        float mapHeight = static_cast<float>(parseSizeValue(mapWindowHeight, windowSize.y));
        if (mapWidth <= 0.0f || mapHeight <= 0.0f) {
            return;
        }

        float mapX = 0.0f;
        float mapY = getInlineMapY(windowSize.y, static_cast<unsigned int>(mapHeight));

        drawNoGpsOverlayAt(mapX, mapY, mapWidth, mapHeight);
    }

    float getBottomLeftOverlayY(float boxHeight, float margin = 15.0f) const {
        float y = static_cast<float>(window->getSize().y) - boxHeight - margin;

        if (experimental && mapViewer && mapViewer->isOpen()) {
            const sf::Texture *mapTexture = mapViewer->getTexture();
            if (mapTexture) {
                float mapTopY = getInlineMapY(window->getSize().y, mapTexture->getSize().y);
                y = std::min(y, mapTopY - boxHeight - margin);
            }
        }

        return std::max(0.0f, y);
    }

    static void latLonToPixelAtZoom(double lat, double lon, int zoom, double &pixX, double &pixY) {
        constexpr double PI = 3.14159265358979323846;
        const double n = std::pow(2.0, zoom);
        pixX = (lon + 180.0) / 360.0 * n * 256.0;
        const double latRad = lat * PI / 180.0;
        pixY = (1.0 - std::asinh(std::tan(latRad)) / PI) / 2.0 * n * 256.0;
    }

    sf::FloatRect getInlineMapRect() const {
        if (!(experimental && mapViewer && mapViewer->isOpen())) {
            return sf::FloatRect();
        }
        const sf::Texture *mapTexture = mapViewer->getTexture();
        if (!mapTexture) {
            return sf::FloatRect();
        }

        auto windowSize = window->getSize();
        auto mapSize = mapTexture->getSize();
        float mapX = 0.f;
        float mapY = getInlineMapY(windowSize.y, mapSize.y);
        return sf::FloatRect(sf::Vector2f(mapX, mapY),
                             sf::Vector2f(static_cast<float>(mapSize.x), static_cast<float>(mapSize.y)));
    }

    bool currentImageHasGps() const {
        return !allImagePaths.empty() && currentIndex < allImagePaths.size() &&
               hasGpsLatitude(allImagePaths[currentIndex]);
    }

    fs::path currentMapSubjectImage() const {
        if (thumbnailMode && folderMode && folderModeFocusIndex < folderModeEntries.size()) {
            const auto &entry = folderModeEntries[folderModeFocusIndex];
            if (!entry.isParentPlaceholder && !entry.representativeImage.empty()) {
                return entry.representativeImage;
            }
            return fs::path();
        }

        if (!allImagePaths.empty() && currentIndex < allImagePaths.size()) {
            return allImagePaths[currentIndex];
        }
        return fs::path();
    }

    bool currentMapSubjectHasGps() const {
        const fs::path subject = currentMapSubjectImage();
        return !subject.empty() && hasGpsLatitude(subject);
    }

    void refreshMapForFolderFocusSelection() {
        if (!mapViewer || !mapViewer->isOpen()) {
            return;
        }
        if (!(thumbnailMode && folderMode) || folderModeFocusIndex >= folderModeEntries.size()) {
            return;
        }

        const auto &entry = folderModeEntries[folderModeFocusIndex];
        if (entry.isParentPlaceholder || entry.representativeImage.empty()) {
            return;
        }

        ensureMetadataForImage(entry.representativeImage);
        updateMapViewerForImage(entry.representativeImage);
    }

    struct MapDotCandidate {
        size_t imageIndex = 0;
        double lat = 0.0;
        double lon = 0.0;
        sf::Vector2f screenPos{0.f, 0.f};
    };

    std::vector<MapDotCandidate> getInlineMapDotCandidates() const {
        std::vector<MapDotCandidate> candidates;
        if (!mapViewer || !mapViewer->isOpen() || allImagePaths.empty() || currentIndex >= allImagePaths.size() ||
            currentIndex >= allDirectories.size()) {
            return candidates;
        }

        const sf::FloatRect mapRect = getInlineMapRect();
        if (mapRect.size.x <= 0.0f || mapRect.size.y <= 0.0f) {
            return candidates;
        }

        const fs::path currentDir = allDirectories[currentIndex];
        const int zoom = mapViewer->getCurrentZoom();
        double centerPixX = 0.0;
        double centerPixY = 0.0;
        latLonToPixelAtZoom(mapViewer->getCenterLat(), mapViewer->getCenterLon(), zoom, centerPixX, centerPixY);

        const float screenCenterX = mapRect.position.x + mapRect.size.x * 0.5f;
        const float screenCenterY = mapRect.position.y + mapRect.size.y * 0.5f;

        for (size_t i = 0; i < allImagePaths.size(); i++) {
            if (i >= allDirectories.size() || allDirectories[i] != currentDir ||
                !passesActiveFilter(allImagePaths[i]) || !hasGpsLatitude(allImagePaths[i])) {
                continue;
            }

            const double lat = getGpsValueOrZero(allImagePaths[i], "GPSLatitude");
            const double lon = getGpsValueOrZero(allImagePaths[i], "GPSLongitude");

            double pointPixX = 0.0;
            double pointPixY = 0.0;
            latLonToPixelAtZoom(lat, lon, zoom, pointPixX, pointPixY);

            const float pointX = screenCenterX + static_cast<float>(pointPixX - centerPixX);
            const float pointY = screenCenterY + static_cast<float>(pointPixY - centerPixY);

            if (pointX >= mapRect.position.x - 10.f && pointX <= mapRect.position.x + mapRect.size.x + 10.f &&
                pointY >= mapRect.position.y - 10.f && pointY <= mapRect.position.y + mapRect.size.y + 10.f) {
                candidates.push_back(MapDotCandidate{i, lat, lon, sf::Vector2f(pointX, pointY)});
            }
        }

        return candidates;
    }

    size_t pickBestMapDotCandidate(const std::vector<MapDotCandidate> &candidates) {
        if (candidates.empty()) {
            return currentIndex;
        }

        const auto sameCoord = [](double aLat, double aLon, double bLat, double bLon) {
            const double eps = 1e-9;
            return std::abs(aLat - bLat) <= eps && std::abs(aLon - bLon) <= eps;
        };

        std::vector<size_t> sameSpot;
        sameSpot.reserve(candidates.size());
        const double refLat = candidates.front().lat;
        const double refLon = candidates.front().lon;
        for (const auto &c : candidates) {
            if (sameCoord(c.lat, c.lon, refLat, refLon)) {
                sameSpot.push_back(c.imageIndex);
            }
        }
        if (sameSpot.empty()) {
            return candidates.front().imageIndex;
        }

        auto hasValidDate = [&](size_t idx, std::string &outDate) {
            if (idx >= allImagePaths.size()) {
                return false;
            }
            ensureMetadataForImage(allImagePaths[idx]);
            outDate = getExifString(allImagePaths[idx], "DateTimeOriginal");
            return !outDate.empty() && outDate != "0000:00:00 00:00:00";
        };

        size_t bestByDate = std::numeric_limits<size_t>::max();
        std::string bestDate;
        for (size_t idx : sameSpot) {
            std::string dt;
            if (hasValidDate(idx, dt)) {
                if (bestByDate == std::numeric_limits<size_t>::max() || dt < bestDate) {
                    bestByDate = idx;
                    bestDate = dt;
                }
            }
        }
        if (bestByDate != std::numeric_limits<size_t>::max()) {
            return bestByDate;
        }

        size_t bestByName = sameSpot.front();
        std::string bestName = allImagePaths[bestByName].filename().string();
        for (size_t idx : sameSpot) {
            const std::string name = allImagePaths[idx].filename().string();
            if (name < bestName) {
                bestByName = idx;
                bestName = name;
            }
        }
        return bestByName;
    }

    bool selectImageFromInlineMapDotsNear(const sf::Vector2f &cursorPos) {
        if (!mapViewer || !mapViewer->isOpen()) {
            return false;
        }

        constexpr float hitRadius = 24.0f; // 3 * current marker radius (8 px)
        const float maxDist2 = hitRadius * hitRadius;

        const auto candidates = getInlineMapDotCandidates();
        std::vector<MapDotCandidate> nearest;
        float bestDist2 = std::numeric_limits<float>::infinity();
        for (const auto &c : candidates) {
            const float dx = c.screenPos.x - cursorPos.x;
            const float dy = c.screenPos.y - cursorPos.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > maxDist2) {
                continue;
            }

            if (d2 + 1e-4f < bestDist2) {
                bestDist2 = d2;
                nearest.clear();
                nearest.push_back(c);
            } else if (std::abs(d2 - bestDist2) <= 1e-4f) {
                nearest.push_back(c);
            }
        }

        if (nearest.empty()) {
            return false;
        }

        const size_t targetIndex = pickBestMapDotCandidate(nearest);
        if (targetIndex >= allImagePaths.size()) {
            return false;
        }

        if (thumbnailMode) {
            currentIndex = targetIndex;
            onThumbnailSelectionChanged();
        } else {
            loadImage(targetIndex);
        }
        return true;
    }

    bool isMouseOverInlineMapDot(const sf::Vector2f &cursorPos) const {
        if (!mapViewer || !mapViewer->isOpen()) {
            return false;
        }

        constexpr float hitRadius = 24.0f; // 3 * current marker radius (8 px)
        const float maxDist2 = hitRadius * hitRadius;
        for (const auto &c : getInlineMapDotCandidates()) {
            const float dx = c.screenPos.x - cursorPos.x;
            const float dy = c.screenPos.y - cursorPos.y;
            if (dx * dx + dy * dy <= maxDist2) {
                return true;
            }
        }
        return false;
    }

    void showAllCurrentFolderInMap() {
        if (!mapViewer || allImagePaths.empty()) {
            return;
        }

        std::vector<size_t> mapIndices;
        if (searchResultsActive && !searchMatchedIndices.empty()) {
            mapIndices.reserve(searchMatchedIndices.size());
            for (size_t idx : searchMatchedIndices) {
                if (idx < allImagePaths.size() && passesActiveFilter(allImagePaths[idx]) &&
                    hasGpsLatitude(allImagePaths[idx])) {
                    mapIndices.push_back(idx);
                }
            }
        } else {
            if (currentIndex >= allImagePaths.size() || currentIndex >= allDirectories.size()) {
                return;
            }

            fs::path currentDir = allDirectories[currentIndex];
            mapIndices.reserve(allImagePaths.size());
            for (size_t i = 0; i < allImagePaths.size(); i++) {
                if (i < allDirectories.size() && allDirectories[i] == currentDir &&
                    passesActiveFilter(allImagePaths[i]) && hasGpsLatitude(allImagePaths[i])) {
                    mapIndices.push_back(i);
                }
            }
        }

        if (mapIndices.empty()) {
            return;
        }

        double minLat = 90.0;
        double maxLat = -90.0;
        double minLon = 180.0;
        double maxLon = -180.0;
        std::vector<std::pair<double, double>> folderGpsPoints;
        folderGpsPoints.reserve(mapIndices.size());
        for (size_t idx : mapIndices) {
            double lat = getGpsValueOrZero(allImagePaths[idx], "GPSLatitude");
            double lon = getGpsValueOrZero(allImagePaths[idx], "GPSLongitude");
            minLat = std::min(minLat, lat);
            maxLat = std::max(maxLat, lat);
            minLon = std::min(minLon, lon);
            maxLon = std::max(maxLon, lon);
            folderGpsPoints.push_back({lat, lon});
        }

        const double centerLat = (minLat + maxLat) * 0.5;
        const double centerLon = (minLon + maxLon) * 0.5;

        auto windowSize = window->getSize();
        int mapW = parseSizeValue(mapWindowWidth, windowSize.x);
        int mapH = parseSizeValue(mapWindowHeight, windowSize.y);
        int fitZoom = minZoom;
        constexpr double marginPx = 24.0;

        for (int z = maxZoom; z >= minZoom; --z) {
            double centerPixX = 0.0;
            double centerPixY = 0.0;
            latLonToPixelAtZoom(centerLat, centerLon, z, centerPixX, centerPixY);

            bool allVisible = true;
            for (size_t idx : mapIndices) {
                double lat = getGpsValueOrZero(allImagePaths[idx], "GPSLatitude");
                double lon = getGpsValueOrZero(allImagePaths[idx], "GPSLongitude");
                double pointPixX = 0.0;
                double pointPixY = 0.0;
                latLonToPixelAtZoom(lat, lon, z, pointPixX, pointPixY);

                double driftX = std::abs(pointPixX - centerPixX);
                double driftY = std::abs(pointPixY - centerPixY);
                if (driftX > (static_cast<double>(mapW) * 0.5 - marginPx) ||
                    driftY > (static_cast<double>(mapH) * 0.5 - marginPx)) {
                    allVisible = false;
                    break;
                }
            }

            if (allVisible) {
                fitZoom = z;
                break;
            }
        }

        fitZoom = std::min(fitZoom, defaultZoom);

        mapViewer->showMap(centerLat, centerLon, fitZoom);
        mapViewer->setGPSPoints(folderGpsPoints);

        if (currentIndex < allImagePaths.size() && hasGpsLatitude(allImagePaths[currentIndex])) {
            double lat = getGpsValueOrZero(allImagePaths[currentIndex], "GPSLatitude");
            double lon = getGpsValueOrZero(allImagePaths[currentIndex], "GPSLongitude");
            mapViewer->updateMarkerOnly(lat, lon);
        }
    }

    void copyImagePathToClipboard() {
        if (!allImagePaths.empty()) {
            std::string fullPath = toUtf8String(allImagePaths[currentIndex]);
            sf::String clip = sf::String::fromUtf8(fullPath.begin(), fullPath.end());
            sf::Clipboard::setString(clip);
        }
    }

    void copyCurrentCoordinatesToClipboard() {
        if (allImagePaths.empty()) {
            return;
        }

        const auto &imagePath = allImagePaths[currentIndex];
        if (!hasGpsLatitude(imagePath)) {
            return;
        }

        double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
        double lon = getGpsValueOrZero(imagePath, "GPSLongitude");

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << lat << ", " << lon;
        sf::String clip = sf::String::fromUtf8(oss.str().begin(), oss.str().end());
        sf::Clipboard::setString(clip);
    }

    void toggleMapForCurrentImage() {
        if (!mapViewer || allImagePaths.empty()) {
            return;
        }

        const auto &imagePath = allImagePaths[currentIndex];
        if (!hasGpsLatitude(imagePath)) {
            return;
        }

        if (mapViewer->isOpen()) {
            mapViewer->close();
            return;
        }

        double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
        double lon = getGpsValueOrZero(imagePath, "GPSLongitude");
        mapViewer->showMap(lat, lon, defaultZoom);
    }

    void updateMapViewerForImage(const fs::path &imagePath) {
        if (!mapViewer || !mapViewer->isOpen()) {
            return;
        }

        if (hasGpsLatitude(imagePath)) {
            double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
            double lon = getGpsValueOrZero(imagePath, "GPSLongitude");

            // Only recenter map if new point is outside the center 50% visible area.
            if (!mapViewer->isPointInStayPutArea(lat, lon)) {
                mapViewer->updateGPS(lat, lon);
            } else {
                // Inside center 50% - keep map centered but update marker position.
                mapViewer->updateMarkerOnly(lat, lon);
            }
        }

        // Keep folder point overlays in sync with the currently selected image's folder.
        std::vector<std::pair<double, double>> folderGpsPoints;
        fs::path currentDir = imagePath.parent_path();
        for (size_t i = 0; i < allImagePaths.size(); i++) {
            if (allDirectories[i] == currentDir && hasGpsLatitude(allImagePaths[i]) &&
                passesActiveFilter(allImagePaths[i])) {
                double ptLat = getGpsValueOrZero(allImagePaths[i], "GPSLatitude");
                double ptLon = getGpsValueOrZero(allImagePaths[i], "GPSLongitude");
                folderGpsPoints.push_back({ptLat, ptLon});
            }
        }
        mapViewer->setGPSPoints(folderGpsPoints);
    }

    void onThumbnailSelectionChanged() {
        if (allImagePaths.empty() || currentIndex >= allImagePaths.size()) {
            return;
        }

        ensureMetadataForImage(allImagePaths[currentIndex]);
        ensureThumbnailSelectionVisible();
        updateMapViewerForImage(allImagePaths[currentIndex]);
    }

    void openCurrentImageInGoogleMaps() {
        if (allImagePaths.empty()) {
            return;
        }

        const auto &imagePath = allImagePaths[currentIndex];
        if (!hasGpsLatitude(imagePath)) {
            return;
        }

        double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
        double lon = getGpsValueOrZero(imagePath, "GPSLongitude");
        std::ostringstream oss;
        oss << "https://www.google.com/maps?q=" << std::fixed << std::setprecision(6) << lat << "," << lon;
        std::string url = oss.str();
        openURL(url);
    }

    void showSearchBoundaryMessage(bool atEnd) {
        navigationMessage = atEnd ? "Reached End of Search Results" : "Reached Start of Search Results";
    }

    bool isSearchResultSetActive() const { return searchResultsActive && !searchMatchedIndices.empty(); }

    std::optional<size_t> currentSearchResultPosition() const {
        if (!isSearchResultSetActive()) {
            return std::nullopt;
        }
        auto it = std::find(searchMatchedIndices.begin(), searchMatchedIndices.end(), currentIndex);
        if (it == searchMatchedIndices.end()) {
            return std::nullopt;
        }
        return static_cast<size_t>(std::distance(searchMatchedIndices.begin(), it));
    }

    std::string getCurrentNavigationIndexLabel() const {
        if (searchResultsActive && !searchMatchedIndices.empty()) {
            const auto pos = currentSearchResultPosition();
            if (pos.has_value()) {
                return std::to_string(pos.value() + 1) + "/" + std::to_string(searchMatchedIndices.size());
            }
            return "0/" + std::to_string(searchMatchedIndices.size());
        }

        if (activeFilterIndex >= 0 && activeFilterIndex < static_cast<int>(filters.size())) {
            size_t filteredCount = 0;
            size_t currentFilteredPosition = 0;
            bool foundCurrent = false;

            for (size_t i = 0; i < allImagePaths.size(); i++) {
                if (passesActiveFilter(allImagePaths[i])) {
                    filteredCount++;
                    if (i < currentIndex || (i == currentIndex && !foundCurrent)) {
                        currentFilteredPosition = filteredCount;
                        if (i == currentIndex) {
                            foundCurrent = true;
                        }
                    }
                }
            }

            if (filteredCount > 0) {
                return std::to_string(currentFilteredPosition) + "/" + std::to_string(filteredCount);
            }
        }

        if (allImagePaths.empty()) {
            return "0/0";
        }
        return std::to_string(currentIndex + 1) + "/" + std::to_string(allImagePaths.size());
    }

    bool navigateWithinSearchResultsByOffset(int delta, bool useThumbnailSelectionFlow) {
        if (!isSearchResultSetActive() || allImagePaths.empty()) {
            return false;
        }

        size_t pos = currentSearchResultPosition().value_or(0);
        int target = static_cast<int>(pos) + delta;
        int clamped = std::clamp(target, 0, static_cast<int>(searchMatchedIndices.size()) - 1);

        if (clamped == static_cast<int>(pos)) {
            showSearchBoundaryMessage(delta > 0);
            return true;
        }

        currentIndex = searchMatchedIndices[static_cast<size_t>(clamped)];
        navigationMessage.clear();
        if (useThumbnailSelectionFlow) {
            onThumbnailSelectionChanged();
        } else {
            loadImage(currentIndex);
        }
        return true;
    }

    bool navigateWithinSearchResultsToBoundary(bool toEnd, bool useThumbnailSelectionFlow) {
        if (!isSearchResultSetActive() || allImagePaths.empty()) {
            return false;
        }

        size_t targetPos = toEnd ? (searchMatchedIndices.size() - 1) : 0;
        auto currentPos = currentSearchResultPosition();
        if (currentPos.has_value() && currentPos.value() == targetPos) {
            showSearchBoundaryMessage(toEnd);
            return true;
        }

        currentIndex = searchMatchedIndices[targetPos];
        navigationMessage.clear();
        if (useThumbnailSelectionFlow) {
            onThumbnailSelectionChanged();
        } else {
            loadImage(currentIndex);
        }
        return true;
    }

    void showCurrentSearchResultInItsFolder() {
        if (!isSearchResultSetActive() || allImagePaths.empty() || currentIndex >= allImagePaths.size()) {
            return;
        }

        const fs::path selectedImage = allImagePaths[currentIndex];
        const fs::path targetFolder = selectedImage.parent_path();
        const bool restoreThumbnailView = thumbnailMode;

        searchResultsActive = false;
        searchMatchedIndices.clear();
        searchUiOpen = false;
        searchSnapshot.valid = false;
        folderMode = false;
        watchedFoldersMode = false;

        buildImageList(targetFolder);
        if (allImagePaths.empty()) {
            return;
        }

        currentIndex = 0;
        for (size_t i = 0; i < allImagePaths.size(); i++) {
            if (allImagePaths[i] == selectedImage) {
                currentIndex = i;
                break;
            }
        }

        if (restoreThumbnailView) {
            thumbnailMode = true;
            onThumbnailSelectionChanged();
        } else {
            thumbnailMode = false;
            loadImage(currentIndex);
        }
        navigationMessage = "Showing folder for selected search result";
    }

    void openContextMenu(sf::Vector2f pos) {
        bool hasGPS = !allImagePaths.empty() && hasGpsLatitude(allImagePaths[currentIndex]);
        bool hasFolderGps = false;
        if (!allImagePaths.empty() && currentIndex < allImagePaths.size() && currentIndex < allDirectories.size()) {
            fs::path currentDir = allDirectories[currentIndex];
            for (size_t i = 0; i < allImagePaths.size(); i++) {
                if (i < allDirectories.size() && allDirectories[i] == currentDir &&
                    passesActiveFilter(allImagePaths[i]) && hasGpsLatitude(allImagePaths[i])) {
                    hasFolderGps = true;
                    break;
                }
            }
        }
        contextMenuItems.clear();
        contextMenuItems.push_back({"Copy image full path (Ctrl-C)", true, ContextMenuAction::CopyImagePath});
        contextMenuItems.push_back({"Copy coordinates", hasGPS, ContextMenuAction::CopyCoordinates});

        if (hasGPS && mapViewer) {
            const char *toggleText = mapViewer->isOpen() ? "Hide map" : "Show on map";
            contextMenuItems.push_back({toggleText, true, ContextMenuAction::ToggleMap});
            contextMenuItems.push_back({"Open in Google Maps", true, ContextMenuAction::OpenGoogleMaps});
        }

        if (mapViewer) {
            bool showSearchResultsGps = false;
            if (searchResultsActive && !searchMatchedIndices.empty()) {
                for (size_t idx : searchMatchedIndices) {
                    if (idx < allImagePaths.size() && passesActiveFilter(allImagePaths[idx]) &&
                        hasGpsLatitude(allImagePaths[idx])) {
                        showSearchResultsGps = true;
                        break;
                    }
                }
            }

            if (showSearchResultsGps || hasFolderGps) {
                const char *showAllMapText = (searchResultsActive && !searchMatchedIndices.empty())
                                                 ? "Show all search results in map"
                                                 : "Show entire folder in map";
                contextMenuItems.push_back({showAllMapText, true, ContextMenuAction::ShowAllInMap});
            }
        }

        contextMenuItems.push_back(
            {"Enter thumbnail mode (Backspace)", !thumbnailMode, ContextMenuAction::ToggleThumbnails});
        contextMenuItems.push_back({"Show in folder", searchResultsActive, ContextMenuAction::ShowInFolder});
        contextMenuItems.push_back({"Toggle full screen (F11)", true, ContextMenuAction::ToggleFullscreen});
        contextMenuItems.push_back({"Help (F1)", true, ContextMenuAction::ShowHelp});

        contextMenuPos = pos;
        contextMenuVisible = true;
    }

    void closeContextMenu() { contextMenuVisible = false; }

    sf::Vector2f getContextMenuSize() const {
        const float paddingX = 12.f;
        const float paddingY = 8.f;
        const float itemHeight = static_cast<float>(getCalculatedFontSize() + 10);

        float maxLabelWidth = 0.f;
        for (const auto &item : contextMenuItems) {
            sf::Text text(uiFont, item.label, getCalculatedFontSize());
            sf::FloatRect bounds = text.getLocalBounds();
            maxLabelWidth = std::max(maxLabelWidth, bounds.size.x);
        }

        const float minMenuWidth = 280.f;
        const float menuWidth = std::max(minMenuWidth, maxLabelWidth + paddingX * 2.f + 2.f);
        const float menuHeight = paddingY * 2.f + itemHeight * static_cast<float>(contextMenuItems.size());
        return sf::Vector2f(menuWidth, menuHeight);
    }

    bool handleContextMenuClick(sf::Vector2f clickPos) {
        if (!contextMenuVisible || contextMenuItems.empty() || !uiFontLoaded) {
            return false;
        }

        const float paddingX = 12.f;
        const float paddingY = 8.f;
        const float itemHeight = static_cast<float>(getCalculatedFontSize() + 10);
        sf::Vector2f menuSize = getContextMenuSize();
        const float menuWidth = menuSize.x;
        const float menuHeight = menuSize.y;

        auto winSize = window->getSize();
        float menuX = std::clamp(contextMenuPos.x, 0.f, static_cast<float>(winSize.x) - menuWidth - 2.f);
        float menuY = std::clamp(contextMenuPos.y, 0.f, static_cast<float>(winSize.y) - menuHeight - 2.f);
        sf::FloatRect menuRect(sf::Vector2f(menuX, menuY), sf::Vector2f(menuWidth, menuHeight));

        if (!menuRect.contains(clickPos)) {
            closeContextMenu();
            return true;
        }

        float currentY = menuY + paddingY;
        for (const auto &item : contextMenuItems) {
            sf::FloatRect rect(sf::Vector2f(menuX, currentY), sf::Vector2f(menuWidth, itemHeight));
            if (rect.contains(clickPos) && item.enabled) {
                switch (item.action) {
                case ContextMenuAction::CopyImagePath:
                    copyImagePathToClipboard();
                    break;
                case ContextMenuAction::CopyCoordinates:
                    copyCurrentCoordinatesToClipboard();
                    break;
                case ContextMenuAction::ToggleMap:
                    toggleMapForCurrentImage();
                    break;
                case ContextMenuAction::OpenGoogleMaps:
                    openCurrentImageInGoogleMaps();
                    break;
                case ContextMenuAction::ShowAllInMap:
                    showAllCurrentFolderInMap();
                    break;
                case ContextMenuAction::ToggleThumbnails:
                    toggleThumbnailMode();
                    break;
                case ContextMenuAction::ShowInFolder:
                    showCurrentSearchResultInItsFolder();
                    break;
                case ContextMenuAction::ToggleFullscreen:
                    toggleWindowMode();
                    break;
                case ContextMenuAction::ShowHelp:
                    showHelp = true;
                    break;
                case ContextMenuAction::None:
                    break;
                }
                closeContextMenu();
                return true;
            }
            currentY += itemHeight;
        }

        closeContextMenu();
        return true;
    }

    void drawContextMenu() {
        if (!contextMenuVisible || contextMenuItems.empty() || !uiFontLoaded) {
            return;
        }

        const float paddingX = 12.f;
        const float paddingY = 8.f;
        const float itemHeight = static_cast<float>(getCalculatedFontSize() + 10);
        sf::Vector2f menuSize = getContextMenuSize();
        const float menuWidth = menuSize.x;
        const float menuHeight = menuSize.y;

        auto winSize = window->getSize();
        float menuX = std::clamp(contextMenuPos.x, 0.f, static_cast<float>(winSize.x) - menuWidth - 2.f);
        float menuY = std::clamp(contextMenuPos.y, 0.f, static_cast<float>(winSize.y) - menuHeight - 2.f);

        sf::RectangleShape bg(sf::Vector2f(menuWidth, menuHeight));
        bg.setPosition({menuX, menuY});
        bg.setFillColor(sf::Color(24, 24, 24, 240));
        bg.setOutlineColor(sf::Color(180, 180, 180));
        bg.setOutlineThickness(1.f);
        window->draw(bg);

        sf::Vector2f mousePos(static_cast<float>(sf::Mouse::getPosition(*window).x),
                              static_cast<float>(sf::Mouse::getPosition(*window).y));
        float currentY = menuY + paddingY;
        for (const auto &item : contextMenuItems) {
            sf::FloatRect rect(sf::Vector2f(menuX, currentY), sf::Vector2f(menuWidth, itemHeight));
            bool hovered = rect.contains(mousePos);

            if (hovered && item.enabled) {
                sf::RectangleShape hoverBg(sf::Vector2f(menuWidth, itemHeight));
                hoverBg.setPosition({menuX, currentY});
                hoverBg.setFillColor(sf::Color(70, 70, 70));
                window->draw(hoverBg);
            }

            sf::Text text(uiFont, item.label, getCalculatedFontSize());
            text.setPosition({menuX + paddingX, currentY + 4.f});
            text.setFillColor(item.enabled ? sf::Color::White : sf::Color(128, 128, 128));
            window->draw(text);

            currentY += itemHeight;
        }
    }

    sf::FloatRect getThumbnailAreaRect() const {
        auto windowSize = window->getSize();
        float startX = 0.0f;
        if (experimental) {
            startX = static_cast<float>(parseSizeValue(mapWindowWidth, windowSize.x));
        }
        float width = std::max(0.0f, static_cast<float>(windowSize.x) - startX);
        float height = static_cast<float>(windowSize.y);
        return sf::FloatRect(sf::Vector2f(startX, 0.0f), sf::Vector2f(width, height));
    }

    bool isWithinCurrentWatchedFolder(const fs::path &dir) const {
        if (currentWatchedFolder.empty()) {
            return false;
        }
        try {
            fs::path absDir = normalizePath(fs::absolute(dir));
            fs::path absWatched = normalizePath(fs::absolute(currentWatchedFolder));
            auto rel = fs::relative(absDir, absWatched);
            std::string relStr = rel.string();
            return !relStr.empty() && relStr.substr(0, 2) != "..";
        } catch (...) {
            return false;
        }
    }

    bool folderHasDirectImages(const fs::path &dir) const {
        try {
            for (const auto &entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file() && isSupportedImage(entry.path())) {
                    return true;
                }
            }
        } catch (...) {
        }
        return false;
    }

    fs::path findRepresentativeImageInTree(const fs::path &dir) {
        std::vector<fs::path> directImages;
        std::vector<fs::path> childFolders;

        try {
            for (const auto &entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file() && isSupportedImage(entry.path())) {
                    directImages.push_back(normalizePath(entry.path()));
                } else if (entry.is_directory()) {
                    childFolders.push_back(normalizePath(entry.path()));
                }
            }
        } catch (...) {
            return fs::path();
        }

        // If this folder has images, choose lexicographically first filename.
        if (!directImages.empty()) {
            std::ranges::sort(directImages, [](const fs::path &a, const fs::path &b) {
                std::string nameA = a.filename().string();
                std::string nameB = b.filename().string();
                std::transform(nameA.begin(), nameA.end(), nameA.begin(), ::tolower);
                std::transform(nameB.begin(), nameB.end(), nameB.begin(), ::tolower);
                return nameA < nameB;
            });
            return directImages.front();
        }

        // No direct images: walk child folders in name order.
        std::ranges::sort(childFolders, [](const fs::path &a, const fs::path &b) {
            std::string nameA = a.filename().string();
            std::string nameB = b.filename().string();
            std::transform(nameA.begin(), nameA.end(), nameA.begin(), ::tolower);
            std::transform(nameB.begin(), nameB.end(), nameB.begin(), ::tolower);
            return nameA < nameB;
        });

        for (const auto &child : childFolders) {
            fs::path rep = findRepresentativeImageInTree(child);
            if (!rep.empty()) {
                return rep;
            }
        }

        return fs::path();
    }

    void rebuildFolderModeEntries() {
        folderModeEntries.clear();
        folderModeClickAreas.clear();

        if (watchedFoldersMode) {
            std::set<fs::path> roots;
            for (const auto &folder : watchedFolders) {
                if (folder.empty()) {
                    continue;
                }
                fs::path normalized = normalizePath(fs::absolute(folder));
                std::error_code ec;
                if (fs::exists(normalized, ec) && fs::is_directory(normalized, ec)) {
                    roots.insert(normalized);
                }
            }

            for (const auto &root : roots) {
                FolderModeEntry item;
                item.folderPath = root;
                item.representativeImage = findRepresentativeImageInTree(root);
                item.label = pathToString(root);
                folderModeEntries.push_back(item);
            }

            if (!pendingFolderModeFocusFolder.empty()) {
                fs::path wanted = normalizePath(fs::absolute(pendingFolderModeFocusFolder));
                size_t matched = std::numeric_limits<size_t>::max();
                for (size_t i = 0; i < folderModeEntries.size(); i++) {
                    if (normalizePath(fs::absolute(folderModeEntries[i].folderPath)) == wanted) {
                        matched = i;
                        break;
                    }
                }
                folderModeFocusIndex = (matched == std::numeric_limits<size_t>::max()) ? 0 : matched;
                pendingFolderModeFocusFolder.clear();
            } else {
                folderModeFocusIndex = 0;
            }

            thumbnailScrollRow = 0;
            return;
        }

        if (currentFolder.empty()) {
            return;
        }

        fs::path absCurrent = normalizePath(fs::absolute(currentFolder));
        fs::path absWatched = normalizePath(fs::absolute(currentWatchedFolder));

        FolderModeEntry parentEntry;
        parentEntry.folderPath = (absCurrent == absWatched) ? absCurrent : absCurrent.parent_path();
        parentEntry.label = "..";
        parentEntry.isParentPlaceholder = true;
        folderModeEntries.push_back(parentEntry);

        std::vector<FolderModeEntry> entries;
        try {
            std::vector<fs::path> children;
            for (const auto &entry : fs::directory_iterator(absCurrent)) {
                if (entry.is_directory()) {
                    children.push_back(normalizePath(entry.path()));
                }
            }
            std::sort(children.begin(), children.end());

            for (const auto &child : children) {
                fs::path rep = findRepresentativeImageInTree(child);
                if (rep.empty()) {
                    continue;
                }
                FolderModeEntry item;
                item.folderPath = child;
                item.representativeImage = rep;
                item.label = child.filename().string();
                entries.push_back(item);
            }
        } catch (...) {
        }

        folderModeEntries.insert(folderModeEntries.end(), entries.begin(), entries.end());

        if (!pendingFolderModeFocusFolder.empty()) {
            fs::path wanted = normalizePath(fs::absolute(pendingFolderModeFocusFolder));
            size_t matched = std::numeric_limits<size_t>::max();
            for (size_t i = 0; i < folderModeEntries.size(); i++) {
                if (normalizePath(fs::absolute(folderModeEntries[i].folderPath)) == wanted) {
                    matched = i;
                    break;
                }
            }
            folderModeFocusIndex = (matched == std::numeric_limits<size_t>::max()) ? 0 : matched;
            pendingFolderModeFocusFolder.clear();
        } else {
            folderModeFocusIndex = 0;
        }

        thumbnailScrollRow = 0;
    }

    void ensureFolderModeFocusVisible() {
        if (!folderMode || folderModeEntries.empty()) {
            return;
        }
        folderModeFocusIndex = std::min(folderModeFocusIndex, folderModeEntries.size() - 1);

        ThumbnailLayout layout = computeThumbnailLayout();
        if (layout.visibleRows <= 0) {
            return;
        }

        int focusedRow = static_cast<int>(folderModeFocusIndex / static_cast<size_t>(thumbnailColumns));
        if (focusedRow < thumbnailScrollRow) {
            thumbnailScrollRow = focusedRow;
        } else if (focusedRow >= thumbnailScrollRow + layout.visibleRows) {
            thumbnailScrollRow = focusedRow - layout.visibleRows + 1;
        }
        thumbnailScrollRow = std::clamp(thumbnailScrollRow, 0, layout.maxScroll);
    }

    fs::path computeSeenImagesCommonFolder() const {
        if (seenImageFolders.empty()) {
            return currentFolder;
        }

        fs::path common = normalizePath(fs::absolute(seenImageFolders.front()));
        for (size_t i = 1; i < seenImageFolders.size(); i++) {
            fs::path other = normalizePath(fs::absolute(seenImageFolders[i]));
            while (!common.empty() && common != other) {
                try {
                    auto rel = fs::relative(other, common);
                    std::string relStr = rel.string();
                    if (!relStr.empty() && relStr.substr(0, 2) != "..") {
                        break;
                    }
                } catch (...) {
                }
                common = common.parent_path();
            }
        }
        return common.empty() ? currentFolder : common;
    }

    void enterFolderMode(const fs::path &folder, const fs::path &focusFolder = fs::path()) {
        watchedFoldersMode = false;
        folderMode = true;
        thumbnailMode = true;
        thumbnailCollectionMessageActive = true;
        currentFolder = normalizePath(fs::absolute(folder));
        pendingFolderModeFocusFolder = focusFolder.empty() ? fs::path() : normalizePath(fs::absolute(focusFolder));
        resetThumbnailLoadingState();
        rebuildFolderModeEntries();
        ensureFolderModeFocusVisible();
    }

    void enterWatchedFoldersMode(const fs::path &focusWatchedFolder = fs::path()) {
        watchedFoldersMode = true;
        folderMode = true;
        thumbnailMode = true;
        thumbnailCollectionMessageActive = true;
        currentFolder.clear();
        pendingFolderModeFocusFolder =
            focusWatchedFolder.empty() ? fs::path() : normalizePath(fs::absolute(focusWatchedFolder));
        resetThumbnailLoadingState();
        rebuildFolderModeEntries();
        ensureFolderModeFocusVisible();
    }

    void openFolderModeEntry(size_t entryIndex) {
        if (entryIndex >= folderModeEntries.size()) {
            return;
        }

        if (watchedFoldersMode) {
            const FolderModeEntry &entry = folderModeEntries[entryIndex];
            fs::path targetFolder = entry.folderPath;
            if (targetFolder.empty()) {
                return;
            }

            currentWatchedFolder = normalizePath(fs::absolute(targetFolder));
            folderModeEligible = true;

            if (folderHasDirectImages(targetFolder)) {
                buildImageList(targetFolder);
                watchedFoldersMode = false;
                folderMode = false;
                thumbnailMode = true;
                currentIndex = 0;
                for (size_t i = 0; i < allImagePaths.size(); i++) {
                    if (passesActiveFilter(allImagePaths[i])) {
                        currentIndex = i;
                        break;
                    }
                }
                onThumbnailSelectionChanged();
                return;
            }

            enterFolderMode(targetFolder);
            return;
        }

        fs::path previousFolder = normalizePath(fs::absolute(currentFolder));
        const FolderModeEntry &entry = folderModeEntries[entryIndex];
        fs::path targetFolder = entry.folderPath;

        if (entry.isParentPlaceholder) {
            if (!currentWatchedFolder.empty() && previousFolder == normalizePath(fs::absolute(currentWatchedFolder))) {
                enterWatchedFoldersMode(previousFolder);
                return;
            }
            if (targetFolder.empty() || !isWithinCurrentWatchedFolder(targetFolder)) {
                return;
            }
            enterFolderMode(targetFolder, previousFolder);
            return;
        }

        if (targetFolder.empty() || !isWithinCurrentWatchedFolder(targetFolder)) {
            return;
        }

        if (folderHasDirectImages(targetFolder)) {
            buildImageList(targetFolder);
            folderMode = false;
            thumbnailMode = true;
            currentIndex = 0;
            for (size_t i = 0; i < allImagePaths.size(); i++) {
                if (passesActiveFilter(allImagePaths[i])) {
                    currentIndex = i;
                    break;
                }
            }
            onThumbnailSelectionChanged();
            return;
        }

        enterFolderMode(targetFolder);
    }

    bool ensureThumbnailCacheFile(const fs::path &imagePath, const fs::path &thumbCacheFile) {
        return ::ensureThumbnailCacheFileOnDisk(imagePath, thumbCacheFile);
    }

    void invalidateThumbnailCache(const fs::path &imagePath) {
        thumbnailTextureCache.erase(imagePath);

        std::error_code ec;
        const fs::path thumbCacheRoot = cacheLocation.empty() ? getDefaultCacheLocation() : fs::path(cacheLocation);
        fs::remove(getThumbnailCacheFilePath(imagePath, thumbCacheRoot), ec);
    }

    std::shared_ptr<sf::Texture> getOrCreateThumbnailTexture(const fs::path &imagePath) {
        if (imagePath.empty()) {
            return nullptr;
        }
        auto it = thumbnailTextureCache.find(imagePath);
        if (it != thumbnailTextureCache.end()) {
            return it->second;
        }

        std::error_code ec;
        const fs::path thumbCacheRoot = cacheLocation.empty() ? getDefaultCacheLocation() : fs::path(cacheLocation);
        fs::path thumbCacheFile = getThumbnailCacheFilePath(imagePath, thumbCacheRoot);
        fs::create_directories(thumbCacheFile.parent_path(), ec);

        if (!ensureThumbnailCacheFile(imagePath, thumbCacheFile)) {
            return nullptr;
        }

        auto tex = std::make_shared<sf::Texture>();
        if (tex->loadFromFile(thumbCacheFile.string())) {
            thumbnailTextureCache[imagePath] = tex;
            return tex;
        }
        return nullptr;
    }

    struct ThumbnailLayout {
        float padding = 0.0f;
        float gap = 0.0f;
        float scrollbarReserve = 0.0f;
        float cellW = 0.0f;
        float cellH = 0.0f;
        int visibleRows = 1;
        int totalRows = 0;
        int maxScroll = 0;
    };

    ThumbnailLayout computeThumbnailLayout() const {
        ThumbnailLayout layout;
        layout.padding = 12.0f;
        layout.gap = 14.0f;
        layout.scrollbarReserve = 16.0f;

        sf::FloatRect area = getThumbnailAreaRect();
        float usableWidth = std::max(1.0f, area.size.x - layout.padding * 2.0f - layout.scrollbarReserve);
        float usableHeight = std::max(1.0f, area.size.y - layout.padding * 2.0f);

        float widthBound = (usableWidth - layout.gap * static_cast<float>(thumbnailColumns - 1)) /
                           static_cast<float>(thumbnailColumns);
        int targetRows = std::max(1, thumbnailColumns);
        float heightBound = (usableHeight - layout.gap * static_cast<float>(targetRows - 1)) /
                            (static_cast<float>(targetRows) * 0.75f);

        layout.cellW = std::max(20.0f, std::min(widthBound, heightBound));
        layout.cellH = layout.cellW * 0.75f;
        layout.visibleRows = targetRows;
        size_t visibleCount = folderMode ? folderModeEntries.size() : getVisibleThumbnailIndices().size();
        layout.totalRows = static_cast<int>((visibleCount + static_cast<size_t>(thumbnailColumns) - 1) /
                                            static_cast<size_t>(thumbnailColumns));
        layout.maxScroll = std::max(0, layout.totalRows - layout.visibleRows);
        return layout;
    }

    sf::FloatRect getThumbnailScrollbarTrackRect() const {
        sf::FloatRect area = getThumbnailAreaRect();
        ThumbnailLayout layout = computeThumbnailLayout();
        if (layout.totalRows <= layout.visibleRows) {
            return sf::FloatRect();
        }

        float barX = area.position.x + area.size.x - 8.0f;
        float barY = area.position.y + layout.padding;
        float barH = std::max(10.0f, area.size.y - layout.padding * 2.0f);
        return sf::FloatRect(sf::Vector2f(barX, barY), sf::Vector2f(4.0f, barH));
    }

    sf::FloatRect getThumbnailScrollbarThumbRect() const {
        ThumbnailLayout layout = computeThumbnailLayout();
        sf::FloatRect track = getThumbnailScrollbarTrackRect();
        if (track.size.x <= 0.0f || track.size.y <= 0.0f) {
            return sf::FloatRect();
        }

        float thumbH = std::max(
            20.0f, track.size.y * (static_cast<float>(layout.visibleRows) / static_cast<float>(layout.totalRows)));
        float t = (layout.maxScroll > 0)
                      ? (static_cast<float>(thumbnailScrollRow) / static_cast<float>(layout.maxScroll))
                      : 0.0f;
        float thumbY = track.position.y + t * (track.size.y - thumbH);
        return sf::FloatRect(sf::Vector2f(track.position.x, thumbY), sf::Vector2f(track.size.x, thumbH));
    }

    void setThumbnailScrollFromCursorY(float cursorY) {
        ThumbnailLayout layout = computeThumbnailLayout();
        if (layout.maxScroll <= 0) {
            thumbnailScrollRow = 0;
            return;
        }

        sf::FloatRect track = getThumbnailScrollbarTrackRect();
        sf::FloatRect thumb = getThumbnailScrollbarThumbRect();
        if (track.size.y <= 0.0f || thumb.size.y <= 0.0f) {
            return;
        }

        float newThumbY = std::clamp(cursorY - thumbnailScrollbarDragOffset, track.position.y,
                                     track.position.y + track.size.y - thumb.size.y);
        float t = (track.size.y - thumb.size.y) > 0.0f ? (newThumbY - track.position.y) / (track.size.y - thumb.size.y)
                                                       : 0.0f;
        thumbnailScrollRow = static_cast<int>(std::round(t * static_cast<float>(layout.maxScroll)));
        thumbnailScrollRow = std::clamp(thumbnailScrollRow, 0, layout.maxScroll);
    }

    static bool isSearchSpecialPunctuation(char c) { return c == '.' || c == '/' || c == '+' || c == '-'; }

    static std::string encodeUtf8(std::uint32_t cp) {
        std::string out;
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
            return out;
        }
        if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return out;
        }
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return out;
        }
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return out;
    }

    static bool decodeUtf8At(const std::string &input, size_t offset, std::uint32_t &codepoint, size_t &width) {
        if (offset >= input.size()) {
            return false;
        }

        const unsigned char c0 = static_cast<unsigned char>(input[offset]);
        if ((c0 & 0x80u) == 0u) {
            codepoint = c0;
            width = 1;
            return true;
        }
        if ((c0 & 0xE0u) == 0xC0u && offset + 1 < input.size()) {
            const unsigned char c1 = static_cast<unsigned char>(input[offset + 1]);
            if ((c1 & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = ((c0 & 0x1Fu) << 6) | (c1 & 0x3Fu);
            width = 2;
            return true;
        }
        if ((c0 & 0xF0u) == 0xE0u && offset + 2 < input.size()) {
            const unsigned char c1 = static_cast<unsigned char>(input[offset + 1]);
            const unsigned char c2 = static_cast<unsigned char>(input[offset + 2]);
            if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = ((c0 & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
            width = 3;
            return true;
        }
        if ((c0 & 0xF8u) == 0xF0u && offset + 3 < input.size()) {
            const unsigned char c1 = static_cast<unsigned char>(input[offset + 1]);
            const unsigned char c2 = static_cast<unsigned char>(input[offset + 2]);
            const unsigned char c3 = static_cast<unsigned char>(input[offset + 3]);
            if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = ((c0 & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
            width = 4;
            return true;
        }
        return false;
    }

    static size_t prevUtf8Offset(const std::string &input, size_t offset) {
        if (offset == 0) {
            return 0;
        }
        size_t p = offset - 1;
        while (p > 0 && (static_cast<unsigned char>(input[p]) & 0xC0u) == 0x80u) {
            --p;
        }
        return p;
    }

    static size_t nextUtf8Offset(const std::string &input, size_t offset) {
        if (offset >= input.size()) {
            return input.size();
        }
        std::uint32_t cp = 0;
        size_t width = 0;
        if (!decodeUtf8At(input, offset, cp, width) || width == 0) {
            return std::min(input.size(), offset + 1);
        }
        return std::min(input.size(), offset + width);
    }

    static std::string joinQuestionMarks(size_t count) {
        std::string out;
        for (size_t i = 0; i < count; i++) {
            if (!out.empty()) {
                out += ", ";
            }
            out += "?";
        }
        return out;
    }

    static std::string repairMojibakeIfNeeded(const std::string &text) {
        try {
            return fixStringEncoding(text);
        } catch (const std::exception &) {
            return text;
        }
    }

    static sf::String sfStringFromUtf8(const std::string &text) {
        return sf::String::fromUtf8(text.begin(), text.end());
    }

    static std::string normalizePathForSearch(const fs::path &p) {
        std::string s = p.lexically_normal().generic_string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    void ensureSearchUnidecodeMapLoaded() {
        if (!searchUnidecodeMap.empty()) {
            return;
        }
        std::vector<fs::path> candidates = {
            fs::path(__FILE__).parent_path().parent_path() / "shared" / "data" / "i18n" / "unidecode_mapping.tsv",
            fs::current_path() / "shared" / "data" / "i18n" / "unidecode_mapping.tsv",
            fs::path("shared") / "data" / "i18n" / "unidecode_mapping.tsv",
        };

        fs::path mappingPath;
        for (const auto &c : candidates) {
            std::error_code ec;
            if (fs::exists(c, ec) && !ec) {
                mappingPath = c;
                break;
            }
        }
        if (mappingPath.empty()) {
            return;
        }

        std::ifstream in(mappingPath);
        if (!in.is_open()) {
            return;
        }

        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (first) {
                first = false;
                continue;
            }
            if (line.empty()) {
                continue;
            }
            size_t tab = line.find('\t');
            if (tab == std::string::npos || tab == 0) {
                continue;
            }
            std::string keyUtf8 = line.substr(0, tab);
            std::string val = line.substr(tab + 1);
            std::uint32_t cp = 0;
            size_t width = 0;
            if (decodeUtf8At(keyUtf8, 0, cp, width)) {
                searchUnidecodeMap[cp] = val;
            }
        }
    }

    struct SearchNormalizedPrefix {
        std::string namePrefix;
        std::string simplePrefix;
        bool hasUnicodeLetters = false;
    };

    static void appendNormalizedPrefixSeparator(std::string &value) {
        if (value.empty() || value.back() == ' ') {
            return;
        }
        value.push_back(' ');
    }

    static std::vector<std::string> splitNormalizedPrefixes(const std::string &value) {
        std::vector<std::string> parts;
        std::string current;
        for (unsigned char ch : value) {
            if (ch == ' ') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(static_cast<char>(ch));
            }
        }
        if (!current.empty()) {
            parts.push_back(current);
        }
        return parts;
    }

    SearchNormalizedPrefix normalizeSearchPrefix(const std::string &raw) {
        ensureSearchUnidecodeMapLoaded();
        SearchNormalizedPrefix out;

        size_t i = 0;
        while (i < raw.size()) {
            std::uint32_t cp = 0;
            size_t width = 0;
            if (!decodeUtf8At(raw, i, cp, width) || width == 0) {
                i++;
                continue;
            }

            if (cp < 128) {
                char c = static_cast<char>(cp);
                if (std::isspace(static_cast<unsigned char>(c))) {
                    appendNormalizedPrefixSeparator(out.namePrefix);
                    appendNormalizedPrefixSeparator(out.simplePrefix);
                    i += width;
                    continue;
                }
                bool allowed = std::isalnum(static_cast<unsigned char>(c)) || isSearchSpecialPunctuation(c);
                if (allowed) {
                    char lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    out.namePrefix.push_back(lowered);
                    out.simplePrefix.push_back(lowered);
                }
                i += width;
                continue;
            }

            auto it = searchUnidecodeMap.find(cp);
            std::string mapped = (it == searchUnidecodeMap.end()) ? std::string() : it->second;
            bool mappedHasAsciiLetters =
                !mapped.empty() &&
                std::any_of(mapped.begin(), mapped.end(), [](unsigned char ch) { return std::isalpha(ch) != 0; });
            bool treatAsUnicodeLetter = mappedHasAsciiLetters || mapped.empty();

            if (treatAsUnicodeLetter) {
                out.hasUnicodeLetters = true;
                out.namePrefix.append(raw, i, width);
            }

            for (char &ch : mapped) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (std::isspace(static_cast<unsigned char>(ch))) {
                    appendNormalizedPrefixSeparator(out.namePrefix);
                    appendNormalizedPrefixSeparator(out.simplePrefix);
                    continue;
                }
                if (std::isalnum(static_cast<unsigned char>(ch)) || isSearchSpecialPunctuation(ch)) {
                    out.simplePrefix.push_back(ch);
                    if (!treatAsUnicodeLetter) {
                        out.namePrefix.push_back(ch);
                    }
                }
            }

            i += width;
        }

        return out;
    }

    void clearActiveSearchResults() {
        searchResultsActive = false;
        searchMatchedIndices.clear();
    }

    void openSearchUi() {
        if (!searchSnapshot.valid) {
            searchSnapshot.valid = true;
            searchSnapshot.thumbnailMode = thumbnailMode;
            searchSnapshot.folderMode = folderMode;
            searchSnapshot.watchedFoldersMode = watchedFoldersMode;
            searchSnapshot.searchResultsActive = searchResultsActive;
            searchSnapshot.currentFolder = currentFolder;
            searchSnapshot.currentWatchedFolder = currentWatchedFolder;
            searchSnapshot.allImagePaths = allImagePaths;
            searchSnapshot.allDirectories = allDirectories;
            searchSnapshot.currentIndex = currentIndex;
            searchSnapshot.thumbnailScrollRow = thumbnailScrollRow;
            searchSnapshot.folderModeFocusIndex = folderModeFocusIndex;
            searchSnapshot.searchMatchedIndices = searchMatchedIndices;
        }
        searchUiOpen = true;
        searchInputFocused = true;
        searchPrefixCursor = std::min(searchPrefixCursor, searchPrefix.size());
        refreshSearchSuggestions();
        lastSearchCursorBlinkTime = std::chrono::steady_clock::now();
        searchCursorVisible = true;
        searchZeroMatchesHint = false;
    }

    void restoreSearchSnapshotState() {
        if (!searchSnapshot.valid) {
            return;
        }

        const bool restoreThumbnailView = searchSnapshot.thumbnailMode;
        const bool restoreFolderMode = searchSnapshot.folderMode;
        const bool restoreWatchedFoldersMode = searchSnapshot.watchedFoldersMode;
        const bool restoreSearchResultsActive = searchSnapshot.searchResultsActive;
        const fs::path restoreCurrentFolder = searchSnapshot.currentFolder;
        const fs::path restoreCurrentWatchedFolder = searchSnapshot.currentWatchedFolder;
        const std::vector<fs::path> restoreAllImagePaths = searchSnapshot.allImagePaths;
        const std::vector<fs::path> restoreAllDirectories = searchSnapshot.allDirectories;
        const size_t restoreCurrentIndex = searchSnapshot.currentIndex;

        allImagePaths = restoreAllImagePaths;
        allDirectories = restoreAllDirectories;
        currentFolder = restoreCurrentFolder;
        currentWatchedFolder = restoreCurrentWatchedFolder;
        thumbnailMode = restoreThumbnailView;
        folderMode = restoreFolderMode;
        watchedFoldersMode = restoreWatchedFoldersMode;
        searchResultsActive = restoreSearchResultsActive;
        if (restoreSearchResultsActive) {
            searchMatchedIndices = searchSnapshot.searchMatchedIndices;
        } else {
            searchMatchedIndices.clear();
        }
        thumbnailScrollRow = searchSnapshot.thumbnailScrollRow;
        folderModeFocusIndex = searchSnapshot.folderModeFocusIndex;
        currentIndex = restoreCurrentIndex;
        if (!allImagePaths.empty()) {
            currentIndex = std::min(currentIndex, allImagePaths.size() - 1);
        } else {
            currentIndex = 0;
        }

        if (searchSnapshot.thumbnailMode) {
            onThumbnailSelectionChanged();
        } else if (!allImagePaths.empty()) {
            loadImage(currentIndex);
        }

        searchSnapshot.valid = false;
    }

    void closeSearchUiAndRestore() {
        if (searchSnapshot.valid) {
            restoreSearchSnapshotState();
        } else {
            searchResultsActive = false;
            searchMatchedIndices.clear();
        }
        navigationMessage.clear();
        searchUiOpen = false;
        searchInputFocused = false;
        searchTokens.clear();
        searchPrefix.clear();
        searchPrefixCursor = 0;
        searchSuggestions.clear();
        highlightedSearchSuggestion = -1;
        searchZeroMatchesHint = false;
        searchTokenDismissRects.clear();
        searchSuggestionRects.clear();
    }

    void closeSearchUiAfterSubmit() {
        searchInputFocused = false;
        searchCursorVisible = false;
        searchSuggestions.clear();
        highlightedSearchSuggestion = -1;
        searchZeroMatchesHint = false;
        searchTokenDismissRects.clear();
        searchSuggestionRects.clear();
    }

    std::vector<SearchSuggestion> querySearchSuggestions(const std::vector<std::string> &prefixes, bool useWordName) {
        std::vector<SearchSuggestion> out;
        if (!metadataCacheReady || metadataCacheFilePath.empty() || prefixes.empty()) {
            return out;
        }

        int suggestionLimit = 200;
        if (window) {
            const unsigned int fontSize = getCalculatedFontSize();
            const float lineSpacing = static_cast<float>(fontSize + 4);
            int visibleRows = static_cast<int>(std::max(1.0f, static_cast<float>(window->getSize().y) / lineSpacing));
            suggestionLimit = std::clamp(visibleRows + 8, 30, 500);
        }

        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(metadataCacheFilePath.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (db) {
                sqlite3_close(db);
            }
            return out;
        }

        const std::string wordColumn = useWordName ? "w.name" : "w.simple";
        std::ostringstream sql;
        sql << "WITH matched_token_prefix AS (";

        for (size_t i = 0; i < prefixes.size(); i++) {
            if (i > 0) {
                sql << " UNION ALL ";
            }
            sql << "SELECT tw.token_id AS token_id, " << (i + 1) << " AS prefix_idx "
                << "FROM token_word tw "
                << "JOIN word w ON w.id = tw.word_id "
                << "WHERE " << wordColumn << " LIKE ? || '%' ";
        }

        sql << "), candidate_tokens AS ("
            << " SELECT token_id"
            << " FROM matched_token_prefix"
            << " GROUP BY token_id"
            << " HAVING COUNT(DISTINCT prefix_idx) = " << prefixes.size() << ") "
            << "SELECT t.name, COUNT(DISTINCT ct.content_id) AS cnt "
            << "FROM candidate_tokens cand "
            << "JOIN token t ON t.id = cand.token_id "
            << "JOIN content_token ct ON ct.token_id = t.id ";

        if (!searchTokens.empty()) {
            sql << "JOIN ("
                << " SELECT ct2.content_id"
                << " FROM content_token ct2"
                << " JOIN token st ON st.id = ct2.token_id"
                << " WHERE st.name IN (" << joinQuestionMarks(searchTokens.size()) << ")"
                << " GROUP BY ct2.content_id"
                << " HAVING COUNT(DISTINCT st.name) = " << searchTokens.size()
                << ") base ON base.content_id = ct.content_id ";
        }

        sql << "WHERE 1 = 1 ";
        if (!searchTokens.empty()) {
            sql << "AND t.name NOT IN (" << joinQuestionMarks(searchTokens.size()) << ") ";
        }

        sql << "GROUP BY t.id, t.name ORDER BY cnt DESC, t.name ASC LIMIT " << suggestionLimit << ";";

        sqlite3_stmt *stmt = nullptr;
        const std::string primarySql = sql.str();
        if (sql::prepare(db, primarySql.c_str(), &stmt) == SQLITE_OK) {
            int bindIndex = 1;
            for (const auto &prefix : prefixes) {
                sqlite3_bind_text(stmt, bindIndex++, prefix.c_str(), -1, SQLITE_TRANSIENT);
            }
            for (const auto &token : searchTokens) {
                sqlite3_bind_text(stmt, bindIndex++, token.c_str(), -1, SQLITE_TRANSIENT);
            }
            for (const auto &token : searchTokens) {
                sqlite3_bind_text(stmt, bindIndex++, token.c_str(), -1, SQLITE_TRANSIENT);
            }

            int rowCount = 0;
            std::string firstRow = "null";
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                ++rowCount;
                const unsigned char *tokenText = sqlite3_column_text(stmt, 0);
                std::int64_t cnt = sqlite3_column_int64(stmt, 1);
                if (rowCount == 1) {
                    std::string tokenName = tokenText ? reinterpret_cast<const char *>(tokenText) : "";
                    std::ostringstream firstRowBuilder;
                    firstRowBuilder << "{name: \"" << tokenName << "\", image_count: " << cnt << "}";
                    firstRow = firstRowBuilder.str();
                }
                if (tokenText) {
                    std::string rawToken(reinterpret_cast<const char *>(tokenText));
                    out.push_back({rawToken, repairMojibakeIfNeeded(rawToken), cnt});
                }
            }
            log_stdout("Results: ", rowCount, " First: ", firstRow);

            sqlite3_finalize(stmt);
        } else {
            log_stdout("DEBUG search: primary suggestion query prepare failed, using fallback: ", sqlite3_errmsg(db));
        }

        if (out.empty()) {
            // Fallback path: keep the same word-prefix semantics as the primary query.
            // We must not fall back to generic token-name substring matches, since that
            // can reintroduce unrelated tokens when a later prefix matches inside a longer word.
            std::ostringstream fallbackSql;
            fallbackSql << "WITH matched_token_prefix AS (";

            for (size_t i = 0; i < prefixes.size(); i++) {
                if (i > 0) {
                    fallbackSql << " UNION ALL ";
                }
                fallbackSql << "SELECT tw.token_id AS token_id, " << (i + 1) << " AS prefix_idx "
                            << "FROM token_word tw "
                            << "JOIN word w ON w.id = tw.word_id "
                            << "WHERE " << wordColumn << " LIKE ? || '%' ";
            }

            fallbackSql << "), candidate_tokens AS ("
                        << " SELECT token_id"
                        << " FROM matched_token_prefix"
                        << " GROUP BY token_id"
                        << " HAVING COUNT(DISTINCT prefix_idx) = " << prefixes.size() << ") "
                        << "SELECT t.name, COUNT(DISTINCT ct.content_id) AS cnt "
                        << "FROM candidate_tokens cand "
                        << "JOIN token t ON t.id = cand.token_id "
                        << "JOIN content_token ct ON ct.token_id = t.id ";

            if (!searchTokens.empty()) {
                fallbackSql << "JOIN ("
                            << " SELECT ct2.content_id"
                            << " FROM content_token ct2"
                            << " JOIN token st ON st.id = ct2.token_id"
                            << " WHERE st.name IN (" << joinQuestionMarks(searchTokens.size()) << ")"
                            << " GROUP BY ct2.content_id"
                            << " HAVING COUNT(DISTINCT st.name) = " << searchTokens.size()
                            << ") base ON base.content_id = ct.content_id ";
            }

            fallbackSql << "WHERE 1 = 1 ";
            if (!searchTokens.empty()) {
                fallbackSql << "AND t.name NOT IN (" << joinQuestionMarks(searchTokens.size()) << ") ";
            }

            fallbackSql << "GROUP BY t.id, t.name ORDER BY cnt DESC, t.name ASC LIMIT " << suggestionLimit << ";";

            sqlite3_stmt *fallbackStmt = nullptr;
            const std::string fallbackSqlText = fallbackSql.str();
            if (sql::prepare(db, fallbackSqlText.c_str(), &fallbackStmt) == SQLITE_OK) {
                int fallbackBind = 1;
                for (const auto &prefix : prefixes) {
                    sqlite3_bind_text(fallbackStmt, fallbackBind++, prefix.c_str(), -1, SQLITE_TRANSIENT);
                }
                for (const auto &token : searchTokens) {
                    sqlite3_bind_text(fallbackStmt, fallbackBind++, token.c_str(), -1, SQLITE_TRANSIENT);
                }
                for (const auto &token : searchTokens) {
                    sqlite3_bind_text(fallbackStmt, fallbackBind++, token.c_str(), -1, SQLITE_TRANSIENT);
                }

                int rowCount = 0;
                std::string firstRow = "null";
                while (sqlite3_step(fallbackStmt) == SQLITE_ROW) {
                    ++rowCount;
                    const unsigned char *tokenText = sqlite3_column_text(fallbackStmt, 0);
                    std::int64_t cnt = sqlite3_column_int64(fallbackStmt, 1);
                    if (rowCount == 1) {
                        std::string tokenName = tokenText ? reinterpret_cast<const char *>(tokenText) : "";
                        std::ostringstream firstRowBuilder;
                        firstRowBuilder << "{name: \"" << tokenName << "\", image_count: " << cnt << "}";
                        firstRow = firstRowBuilder.str();
                    }
                    if (tokenText) {
                        std::string rawToken(reinterpret_cast<const char *>(tokenText));
                        out.push_back({rawToken, repairMojibakeIfNeeded(rawToken), cnt});
                    }
                }
                log_stdout("Results: ", rowCount, " First: ", firstRow);

                sqlite3_finalize(fallbackStmt);
            }
        }

        sqlite3_close(db);
        return out;
    }

    void refreshSearchSuggestions() {
        searchSuggestions.clear();
        highlightedSearchSuggestion = -1;
        searchZeroMatchesHint = false;

        SearchNormalizedPrefix norm = normalizeSearchPrefix(searchPrefix);
        const bool useWordName = norm.hasUnicodeLetters;
        const std::string normalized = useWordName ? norm.namePrefix : norm.simplePrefix;
        std::vector<std::string> prefixes = splitNormalizedPrefixes(normalized);
        if (prefixes.empty()) {
            return;
        }

        searchSuggestions = querySearchSuggestions(prefixes, useWordName);
        if (!searchSuggestions.empty()) {
            highlightedSearchSuggestion = 0;
        } else {
            searchZeroMatchesHint = true;
        }
    }

    bool addSearchToken(const std::string &token) {
        if (token.empty()) {
            return false;
        }
        if (std::find(searchTokens.begin(), searchTokens.end(), token) != searchTokens.end()) {
            searchPrefix.clear();
            searchPrefixCursor = 0;
            refreshSearchSuggestions();
            return false;
        }

        // A new token starts a new composed query. Any previously live result set must be reset
        // before the user edits the search terms or submits again, otherwise stale counts/indexes
        // remain in the UI while the new query is being built.
        searchResultsActive = false;
        searchMatchedIndices.clear();
        navigationMessage.clear();

        searchTokens.push_back(token);
        searchPrefix.clear();
        searchPrefixCursor = 0;
        refreshSearchSuggestions();
        return true;
    }

    std::string detectFileNameColumn(sqlite3 *db) {
        std::string column = "basename";
        sqlite3_stmt *stmt = nullptr;
        if (sql::prepare(db, "PRAGMA table_info(file);", &stmt) != SQLITE_OK) {
            return column;
        }

        bool hasBasename = false;
        bool hasName = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *nameRaw = sqlite3_column_text(stmt, 1);
            if (!nameRaw) {
                continue;
            }
            const std::string c(reinterpret_cast<const char *>(nameRaw));
            if (c == "basename") {
                hasBasename = true;
            } else if (c == "name") {
                hasName = true;
            }
        }
        sqlite3_finalize(stmt);
        if (hasBasename) {
            return "basename";
        }
        if (hasName) {
            return "name";
        }
        return column;
    }

    std::vector<size_t> querySearchMatchedIndices(const std::vector<std::string> &tokens) {
        std::vector<size_t> out;
        if (!metadataCacheReady || metadataCacheFilePath.empty() || tokens.empty() || allImagePaths.empty()) {
            log_stdout("DEBUG search: querySearchMatchedIndices early exit (cacheReady=", metadataCacheReady,
                       ", cachePath='", metadataCacheFilePath.string(), "', tokens=", tokens.size(),
                       ", images=", allImagePaths.size(), ")");
            return out;
        }

        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(metadataCacheFilePath.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            log_stdout("DEBUG search: failed to open metadata DB at ", metadataCacheFilePath.string());
            if (db) {
                sqlite3_close(db);
            }
            return out;
        }

        const std::string fileNameColumn = detectFileNameColumn(db);

        std::ostringstream sql;
        sql << "SELECT d.name, f." << fileNameColumn << " "
            << "FROM content_token ct "
            << "JOIN token t ON t.id = ct.token_id "
            << "JOIN content c ON c.id = ct.content_id "
            << "JOIN file f ON f.id = c.file_id "
            << "JOIN dir d ON d.id = f.dir_id "
            << "WHERE t.name IN (" << joinQuestionMarks(tokens.size()) << ") "
            << "GROUP BY c.id, d.name, f." << fileNameColumn << " "
            << "HAVING COUNT(DISTINCT t.name) = " << tokens.size() << ";";

        sqlite3_stmt *stmt = nullptr;
        const std::string matchedSql = sql.str();
        if (sql::prepare(db, matchedSql.c_str(), &stmt) != SQLITE_OK) {
            log_stdout("DEBUG search: prepare failed for matched query: ", sqlite3_errmsg(db));
            sqlite3_close(db);
            return out;
        }
        for (size_t i = 0; i < tokens.size(); i++) {
            sqlite3_bind_text(stmt, static_cast<int>(i + 1), tokens[i].c_str(), -1, SQLITE_TRANSIENT);
        }

        int rowCount = 0;
        std::string firstRow = "null";
        std::vector<fs::path> matchedImagePaths;
        std::vector<fs::path> matchedDirectories;
        std::unordered_map<std::string, size_t> seenMatches;

        size_t dbRows = 0;
        size_t mappedRows = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            dbRows++;
            if (dbRows == 1) {
                const unsigned char *dirNameRaw = sqlite3_column_text(stmt, 0);
                const unsigned char *baseNameRaw = sqlite3_column_text(stmt, 1);
                std::string dirName = dirNameRaw ? reinterpret_cast<const char *>(dirNameRaw) : "";
                std::string baseName = baseNameRaw ? reinterpret_cast<const char *>(baseNameRaw) : "";
                std::ostringstream firstBuilder;
                firstBuilder << "{dir_name: \"" << dirName << "\", basename: \"" << baseName << "\"}";
                firstRow = firstBuilder.str();
            }
            const unsigned char *dirNameRaw = sqlite3_column_text(stmt, 0);
            const unsigned char *baseNameRaw = sqlite3_column_text(stmt, 1);
            if (!dirNameRaw || !baseNameRaw) {
                continue;
            }

            std::string dirName(reinterpret_cast<const char *>(dirNameRaw));
            std::string baseName(reinterpret_cast<const char *>(baseNameRaw));
            fs::path candidate = fs::path(dirName) / baseName;
            std::error_code absEc;
            fs::path candidateAbs = fs::absolute(candidate, absEc);
            if (absEc) {
                candidateAbs = candidate;
            }
            candidateAbs = candidateAbs.lexically_normal();

            const std::string key = normalizePathForSearch(candidateAbs);
            if (seenMatches.find(key) != seenMatches.end()) {
                continue;
            }

            if (!passesActiveFilter(candidateAbs)) {
                continue;
            }

            seenMatches[key] = matchedImagePaths.size();
            matchedImagePaths.push_back(candidateAbs);
            matchedDirectories.push_back(candidateAbs.parent_path());
            mappedRows++;
        }

        log_stdout("Results: ", dbRows, " First: ", firstRow);

        sqlite3_finalize(stmt);
        sqlite3_close(db);

        if (!matchedImagePaths.empty()) {
            allImagePaths = matchedImagePaths;
            allDirectories = matchedDirectories;
            out.resize(matchedImagePaths.size());
            std::iota(out.begin(), out.end(), 0);
        }

        log_stdout("DEBUG search: matched rows from DB=", dbRows, ", mapped rows=", mappedRows,
                   ", unique hits=", out.size(), ", file column=", fileNameColumn);
        return out;
    }

    bool submitSearchQuery() {
        log_stdout("DEBUG search: submit start tokens=", searchTokens.size(), ", prefix bytes=", searchPrefix.size(),
                   ", suggestions=", searchSuggestions.size(), ", highlighted=", highlightedSearchSuggestion);

        std::vector<std::string> submitTokens = searchTokens;
        if (!searchPrefix.empty()) {
            std::string chosenToken = searchPrefix;
            if (!searchSuggestions.empty() && highlightedSearchSuggestion >= 0 &&
                highlightedSearchSuggestion < static_cast<int>(searchSuggestions.size())) {
                chosenToken = searchSuggestions[static_cast<size_t>(highlightedSearchSuggestion)].token;
            }
            if (!chosenToken.empty() &&
                std::find(submitTokens.begin(), submitTokens.end(), chosenToken) == submitTokens.end()) {
                submitTokens.push_back(chosenToken);
            }
            searchPrefix.clear();
            searchPrefixCursor = 0;
            refreshSearchSuggestions();
        }

        if (submitTokens.empty()) {
            searchTokens.clear();
            navigationMessage = "Search: no token";
            log_stdout("DEBUG search: submit aborted, no tokens selected");
            return false;
        }

        searchTokens = submitTokens;
        clearActiveSearchResults();

        std::ostringstream tok;
        for (size_t i = 0; i < searchTokens.size(); i++) {
            if (i > 0) {
                tok << " | ";
            }
            tok << searchTokens[i];
        }
        log_stdout("DEBUG search: query tokens: ", tok.str());

        std::vector<size_t> hits = querySearchMatchedIndices(searchTokens);
        if (hits.empty()) {
            navigationMessage = "Search: (0)";
            log_stdout("DEBUG search: submit produced 0 hits");
            return false;
        }

        searchMatchedIndices = std::move(hits);
        searchResultsActive = true;
        thumbnailMode = true;
        folderMode = false;
        watchedFoldersMode = false;
        // Search submit swaps the image/index set; clear stale ready/queued thumbnail indices.
        resetThumbnailLoadingState();
        thumbnailCollectionMessageActive = true;
        currentIndex = searchMatchedIndices.front();
        onThumbnailSelectionChanged();
        navigationMessage = "Search hits: " + std::to_string(searchMatchedIndices.size());
        log_stdout("DEBUG search: submit success, hits=", searchMatchedIndices.size(), ", first index=", currentIndex);
        lastSearchSubmitTime = std::chrono::steady_clock::now();
        lastSearchCursorBlinkTime = std::chrono::steady_clock::now();
        searchCursorVisible = true;
        searchSuggestions.clear();
        highlightedSearchSuggestion = -1;
        searchZeroMatchesHint = false;
        return true;
    }

    std::vector<size_t> getVisibleThumbnailIndices() const {
        std::vector<size_t> visible;

        if (searchResultsActive) {
            visible.reserve(searchMatchedIndices.size());
            for (size_t idx : searchMatchedIndices) {
                if (idx < allImagePaths.size() && passesActiveFilter(allImagePaths[idx])) {
                    visible.push_back(idx);
                }
            }
            return visible;
        }

        visible.reserve(allImagePaths.size());

        for (size_t i = 0; i < allImagePaths.size(); i++) {
            if (passesActiveFilter(allImagePaths[i])) {
                visible.push_back(i);
            }
        }

        return visible;
    }

    void clampThumbnailScroll() {
        if (!folderMode && allImagePaths.empty()) {
            thumbnailScrollRow = 0;
            return;
        }

        ThumbnailLayout layout = computeThumbnailLayout();
        thumbnailScrollRow = std::clamp(thumbnailScrollRow, 0, layout.maxScroll);
    }

    void ensureThumbnailSelectionVisible() {
        if (!thumbnailMode) {
            return;
        }
        if (folderMode) {
            clampThumbnailScroll();
            return;
        }
        if (allImagePaths.empty()) {
            return;
        }

        ThumbnailLayout layout = computeThumbnailLayout();
        auto visibleIndices = getVisibleThumbnailIndices();
        auto selectedIt = std::find(visibleIndices.begin(), visibleIndices.end(), currentIndex);
        int selectedRow = 0;
        if (selectedIt != visibleIndices.end()) {
            size_t visibleIndex = static_cast<size_t>(std::distance(visibleIndices.begin(), selectedIt));
            selectedRow = static_cast<int>(visibleIndex / static_cast<size_t>(thumbnailColumns));
        }

        if (selectedRow < thumbnailScrollRow) {
            thumbnailScrollRow = selectedRow;
        } else if (selectedRow >= thumbnailScrollRow + layout.visibleRows) {
            thumbnailScrollRow = selectedRow - layout.visibleRows + 1;
        }

        clampThumbnailScroll();
    }

    void moveThumbnailPageDownOrNextFolder() {
        if (!thumbnailMode || allImagePaths.empty() || currentIndex >= allImagePaths.size() ||
            currentIndex >= allDirectories.size()) {
            return;
        }

        auto visibleIndices = getVisibleThumbnailIndices();
        if (visibleIndices.empty()) {
            return;
        }

        const fs::path currentDir = allDirectories[currentIndex];
        std::vector<size_t> folderVisibleIndices;
        folderVisibleIndices.reserve(visibleIndices.size());
        for (size_t idx : visibleIndices) {
            if (idx < allDirectories.size() && allDirectories[idx] == currentDir) {
                folderVisibleIndices.push_back(idx);
            }
        }

        if (folderVisibleIndices.empty()) {
            return;
        }

        ThumbnailLayout layout = computeThumbnailLayout();
        size_t pageSize = std::max<size_t>(1, static_cast<size_t>(layout.visibleRows) *
                                                  static_cast<size_t>(std::max(1, thumbnailColumns)));

        auto it = std::find(folderVisibleIndices.begin(), folderVisibleIndices.end(), currentIndex);
        size_t currentPos = (it != folderVisibleIndices.end())
                                ? static_cast<size_t>(std::distance(folderVisibleIndices.begin(), it))
                                : static_cast<size_t>(0);
        size_t targetPos = currentPos + pageSize;

        if (targetPos < folderVisibleIndices.size()) {
            currentIndex = folderVisibleIndices[targetPos];
        } else {
            currentIndex = getFirstInNextFolder();
        }

        onThumbnailSelectionChanged();
    }

    void moveThumbnailPageUpOrPrevFolder() {
        if (!thumbnailMode || allImagePaths.empty() || currentIndex >= allImagePaths.size() ||
            currentIndex >= allDirectories.size()) {
            return;
        }

        auto visibleIndices = getVisibleThumbnailIndices();
        if (visibleIndices.empty()) {
            return;
        }

        const fs::path currentDir = allDirectories[currentIndex];
        std::vector<size_t> folderVisibleIndices;
        folderVisibleIndices.reserve(visibleIndices.size());
        for (size_t idx : visibleIndices) {
            if (idx < allDirectories.size() && allDirectories[idx] == currentDir) {
                folderVisibleIndices.push_back(idx);
            }
        }

        if (folderVisibleIndices.empty()) {
            return;
        }

        ThumbnailLayout layout = computeThumbnailLayout();
        size_t pageSize = std::max<size_t>(1, static_cast<size_t>(layout.visibleRows) *
                                                  static_cast<size_t>(std::max(1, thumbnailColumns)));

        auto it = std::find(folderVisibleIndices.begin(), folderVisibleIndices.end(), currentIndex);
        size_t currentPos = (it != folderVisibleIndices.end())
                                ? static_cast<size_t>(std::distance(folderVisibleIndices.begin(), it))
                                : static_cast<size_t>(0);

        if (currentPos >= pageSize) {
            currentIndex = folderVisibleIndices[currentPos - pageSize];
        } else {
            currentIndex = getFirstInPrevFolder();
        }

        onThumbnailSelectionChanged();
    }

    void setThumbnailColumns(int cols) {
        thumbnailColumns = std::clamp(cols, 4, 12);
        clampThumbnailScroll();
        ensureThumbnailSelectionVisible();
    }

    void resetThumbnailLoadingState() {
        thumbnailLoadQueue.clear();
        thumbnailQueuedIndices.clear();
        thumbnailReadyIndices.clear();
        folderThumbnailLoadQueue.clear();
        folderThumbnailQueuedIndices.clear();
        folderThumbnailReadyIndices.clear();
        folderThumbQueueSeeded = false;
        thumbnailScrollbarDragging = false;
    }

    void enqueueFolderThumbnailLoad(size_t index, bool front = false) {
        if (index >= folderModeEntries.size() || folderThumbnailReadyIndices.contains(index) ||
            folderThumbnailQueuedIndices.contains(index)) {
            return;
        }

        const auto &entry = folderModeEntries[index];
        if (entry.isParentPlaceholder || entry.representativeImage.empty()) {
            return;
        }

        folderThumbnailQueuedIndices.insert(index);
        if (front) {
            folderThumbnailLoadQueue.push_front(index);
        } else {
            folderThumbnailLoadQueue.push_back(index);
        }
    }

    void enqueueVisibleFolderThumbnailLoads(int startRow, int endRow) {
        if (folderModeEntries.empty()) {
            return;
        }

        if (folderModeFocusIndex < folderModeEntries.size()) {
            enqueueFolderThumbnailLoad(folderModeFocusIndex, true);
        }

        for (int row = startRow; row < endRow; row++) {
            for (int col = 0; col < thumbnailColumns; col++) {
                size_t idx =
                    static_cast<size_t>(row) * static_cast<size_t>(thumbnailColumns) + static_cast<size_t>(col);
                if (idx >= folderModeEntries.size()) {
                    break;
                }
                enqueueFolderThumbnailLoad(idx);
            }
        }
    }

    void enqueueThumbnailLoad(size_t index, bool front = false) {
        if (index >= allImagePaths.size() || thumbnailReadyIndices.contains(index) ||
            thumbnailQueuedIndices.contains(index)) {
            return;
        }

        thumbnailQueuedIndices.insert(index);
        if (front) {
            thumbnailLoadQueue.push_front(index);
        } else {
            thumbnailLoadQueue.push_back(index);
        }
    }

    void enqueueVisibleThumbnailLoads(int startRow, int endRow) {
        if (allImagePaths.empty()) {
            return;
        }

        auto visibleIndices = getVisibleThumbnailIndices();
        enqueueThumbnailLoad(currentIndex, true);

        for (int row = startRow; row < endRow; row++) {
            for (int col = 0; col < thumbnailColumns; col++) {
                size_t visibleIndex =
                    static_cast<size_t>(row) * static_cast<size_t>(thumbnailColumns) + static_cast<size_t>(col);
                if (visibleIndex >= visibleIndices.size()) {
                    break;
                }
                enqueueThumbnailLoad(visibleIndices[visibleIndex]);
            }
        }
    }

    void processThumbnailLoading() {
        if (!thumbnailMode) {
            return;
        }

        if (folderMode) {
            if (!folderThumbnailLoadQueue.empty()) {
                size_t idx = folderThumbnailLoadQueue.front();
                folderThumbnailLoadQueue.pop_front();
                folderThumbnailQueuedIndices.erase(idx);

                if (idx < folderModeEntries.size()) {
                    const auto &entry = folderModeEntries[idx];
                    if (!entry.isParentPlaceholder && !entry.representativeImage.empty()) {
                        ensureMetadataForImage(entry.representativeImage);
                        if (getOrCreateThumbnailTexture(entry.representativeImage)) {
                            folderThumbnailReadyIndices.insert(idx);
                        }
                    }
                }
            }

            if (thumbnailCollectionMessageActive) {
                bool allReady = true;
                for (size_t i = 0; i < folderModeEntries.size(); i++) {
                    const auto &entry = folderModeEntries[i];
                    if (entry.isParentPlaceholder || entry.representativeImage.empty()) {
                        continue;
                    }
                    if (!folderThumbnailReadyIndices.contains(i)) {
                        allReady = false;
                        break;
                    }
                }

                if (allReady && folderThumbnailLoadQueue.empty() && folderThumbnailQueuedIndices.empty()) {
                    thumbnailCollectionMessageActive = false;
                }
            }
            return;
        }

        if (thumbnailLoadQueue.empty()) {
            return;
        }

        size_t idx = thumbnailLoadQueue.front();
        thumbnailLoadQueue.pop_front();
        thumbnailQueuedIndices.erase(idx);

        if (idx >= allImagePaths.size()) {
            return;
        }

        const fs::path &imagePath = allImagePaths[idx];
        // Orientation comes from metadata; load it before drawing ready thumbnails.
        ensureMetadataForImage(imagePath);
        if (getOrCreateThumbnailTexture(imagePath)) {
            thumbnailReadyIndices.insert(idx);
        }

        if (thumbnailCollectionMessageActive && thumbnailMode && thumbnailLoadQueue.empty() &&
            thumbnailQueuedIndices.empty()) {
            auto visibleIndices = getVisibleThumbnailIndices();
            ThumbnailLayout layout = computeThumbnailLayout();
            int startRow = thumbnailScrollRow;
            int endRow = std::min(layout.totalRows, startRow + layout.visibleRows);
            bool visibleReady = true;

            for (int row = startRow; row < endRow && visibleReady; row++) {
                for (int col = 0; col < thumbnailColumns; col++) {
                    size_t visibleIndex =
                        static_cast<size_t>(row) * static_cast<size_t>(thumbnailColumns) + static_cast<size_t>(col);
                    if (visibleIndex >= visibleIndices.size()) {
                        break;
                    }
                    size_t imageIndex = visibleIndices[visibleIndex];
                    if (!thumbnailReadyIndices.contains(imageIndex)) {
                        visibleReady = false;
                        break;
                    }
                }
            }

            if (visibleReady && thumbnailReadyIndices.contains(currentIndex)) {
                thumbnailCollectionMessageActive = false;
            }
        }
    }

    void toggleThumbnailMode() {
        thumbnailMode = !thumbnailMode;
        folderMode = false;
        closeContextMenu();
        if (thumbnailMode) {
            thumbnailCollectionMessageActive = true;
            resetThumbnailLoadingState();
            ensureThumbnailSelectionVisible();
            thumbnailReadyIndices.erase(currentIndex);
        } else if (!allImagePaths.empty()) {
            thumbnailCollectionMessageActive = false;
            loadImage(currentIndex);
        } else {
            thumbnailCollectionMessageActive = false;
        }
    }

    void refreshThumbnailsAfterImageSetChange() {
        if (!thumbnailMode) {
            return;
        }

        resetThumbnailLoadingState();
    }

    std::shared_ptr<sf::Texture> getCachedThumbnailTexture(const fs::path &imagePath) {
        auto it = thumbnailTextureCache.find(imagePath);
        if (it == thumbnailTextureCache.end()) {
            return nullptr;
        }
        return it->second;
    }

    sf::String fitTextToWidthWithEllipsis(const std::string &text, unsigned int charSize, float maxWidth) const {
        if (text.empty() || maxWidth <= 0.0f) {
            return sf::String();
        }

        sf::Text probe(uiFont, sf::String(), charSize);
        sf::String full = sf::String::fromUtf8(text.begin(), text.end());
        probe.setString(full);
        if (probe.getLocalBounds().size.x <= maxWidth) {
            return full;
        }

        const sf::String dots("...");
        probe.setString(dots);
        if (probe.getLocalBounds().size.x > maxWidth) {
            return sf::String();
        }

        std::size_t lo = 0;
        std::size_t hi = full.getSize();
        while (lo < hi) {
            std::size_t mid = (lo + hi + 1) / 2;
            sf::String candidate = full.substring(0, mid) + dots;
            probe.setString(candidate);
            if (probe.getLocalBounds().size.x <= maxWidth) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }

        if (lo == 0) {
            return dots;
        }
        return full.substring(0, lo) + dots;
    }

    void drawThumbnailGrid() {
        thumbnailClickAreas.clear();
        folderModeClickAreas.clear();
        if (!thumbnailMode) {
            return;
        }
        if (!folderMode && allImagePaths.empty()) {
            return;
        }

        sf::FloatRect area = getThumbnailAreaRect();
        ThumbnailLayout layout = computeThumbnailLayout();
        auto visibleIndices = folderMode ? std::vector<size_t>() : getVisibleThumbnailIndices();
        thumbnailScrollRow = std::clamp(thumbnailScrollRow, 0, layout.maxScroll);

        int startRow = thumbnailScrollRow;
        int endRow = std::min(layout.totalRows, startRow + layout.visibleRows);

        if (folderMode) {
            enqueueVisibleFolderThumbnailLoads(startRow, endRow);

            if (!folderThumbQueueSeeded) {
                for (size_t i = 0; i < folderModeEntries.size(); i++) {
                    enqueueFolderThumbnailLoad(i);
                }
                folderThumbQueueSeeded = true;
            }
        } else {
            enqueueVisibleThumbnailLoads(startRow, endRow);
        }

        sf::RectangleShape areaBg(area.size);
        areaBg.setPosition(area.position);
        areaBg.setFillColor(sf::Color(12, 12, 12));
        window->draw(areaBg);

        for (int row = startRow; row < endRow; row++) {
            for (int col = 0; col < thumbnailColumns; col++) {
                size_t visibleIndex =
                    static_cast<size_t>(row) * static_cast<size_t>(thumbnailColumns) + static_cast<size_t>(col);
                size_t totalCount = folderMode ? folderModeEntries.size() : visibleIndices.size();
                if (visibleIndex >= totalCount) {
                    break;
                }

                size_t idx = folderMode ? visibleIndex : visibleIndices[visibleIndex];

                float x = area.position.x + layout.padding + static_cast<float>(col) * (layout.cellW + layout.gap);
                float y = area.position.y + layout.padding +
                          static_cast<float>(row - startRow) * (layout.cellH + layout.gap);
                sf::FloatRect cellRect(sf::Vector2f(x, y), sf::Vector2f(layout.cellW, layout.cellH));

                float captionHeight =
                    folderMode ? std::max(18.0f, static_cast<float>(getCalculatedFontSize() + 6)) : 0.0f;
                sf::FloatRect imageRect =
                    folderMode
                        ? sf::FloatRect(sf::Vector2f(x, y),
                                        sf::Vector2f(layout.cellW, std::max(10.0f, layout.cellH - captionHeight)))
                        : cellRect;

                sf::RectangleShape cellBg(cellRect.size);
                cellBg.setPosition(cellRect.position);
                cellBg.setFillColor(sf::Color(18, 18, 18));
                cellBg.setOutlineThickness(1.0f);
                cellBg.setOutlineColor(sf::Color(70, 70, 70));
                window->draw(cellBg);

                if (folderMode && folderModeEntries[idx].isParentPlaceholder) {
                    sf::RectangleShape placeholder(imageRect.size);
                    placeholder.setPosition(imageRect.position);
                    placeholder.setFillColor(sf::Color::Transparent);
                    placeholder.setOutlineThickness(2.0f);
                    placeholder.setOutlineColor(sf::Color(140, 140, 140));
                    window->draw(placeholder);
                } else {
                    std::shared_ptr<sf::Texture> tex;
                    if (folderMode) {
                        if (folderThumbnailReadyIndices.contains(idx)) {
                            tex = getCachedThumbnailTexture(folderModeEntries[idx].representativeImage);
                        }
                    } else if (thumbnailReadyIndices.contains(idx)) {
                        tex = getCachedThumbnailTexture(allImagePaths[idx]);
                    }

                    if (tex) {
                        sf::Sprite thumb(*tex);
                        auto texSize = tex->getSize();
                        int orientation = 1;
                        if (folderMode) {
                            const fs::path &repImage = folderModeEntries[idx].representativeImage;
                            orientation = getOrientationOrDefault(repImage);
                        } else {
                            orientation = getOrientationOrDefault(allImagePaths[idx]);
                        }
                        float thumbW = static_cast<float>(texSize.x);
                        float thumbH = static_cast<float>(texSize.y);
                        float rotation = 0.0f;
                        bool flipH = false;
                        bool flipV = false;

                        switch (orientation) {
                        case 2:
                            flipH = true;
                            break;
                        case 3:
                            rotation = 180.0f;
                            break;
                        case 4:
                            flipV = true;
                            break;
                        case 5:
                            rotation = 90.0f;
                            flipH = true;
                            break;
                        case 6:
                            rotation = 90.0f;
                            break;
                        case 7:
                            rotation = 270.0f;
                            flipH = true;
                            break;
                        case 8:
                            rotation = 270.0f;
                            break;
                        default:
                            break;
                        }

                        if (rotation == 90.0f || rotation == 270.0f) {
                            std::swap(thumbW, thumbH);
                        }

                        float sx = imageRect.size.x / thumbW;
                        float sy = imageRect.size.y / thumbH;
                        float s = std::min(sx, sy);
                        thumb.setRotation(sf::degrees(rotation));
                        thumb.setScale({flipH ? -s : s, flipV ? -s : s});
                        thumb.setOrigin({static_cast<float>(texSize.x) / 2.0f, static_cast<float>(texSize.y) / 2.0f});

                        thumb.setPosition({imageRect.position.x + imageRect.size.x / 2.0f,
                                           imageRect.position.y + imageRect.size.y / 2.0f});
                        window->draw(thumb);
                    }
                }

                if (!folderMode && idx == currentIndex) {
                    sf::RectangleShape selected(cellRect.size);
                    selected.setPosition(cellRect.position);
                    selected.setFillColor(sf::Color::Transparent);
                    selected.setOutlineThickness(3.0f);
                    selected.setOutlineColor(sf::Color::Cyan);
                    window->draw(selected);
                } else if (folderMode && idx == folderModeFocusIndex) {
                    sf::RectangleShape focused(cellRect.size);
                    focused.setPosition(cellRect.position);
                    focused.setFillColor(sf::Color::Transparent);
                    focused.setOutlineThickness(3.0f);
                    focused.setOutlineColor(sf::Color::Cyan);
                    window->draw(focused);
                }

                if (folderMode) {
                    const std::string caption = folderModeEntries[idx].label;
                    if (!caption.empty()) {
                        unsigned int captionSize = getCalculatedFontSize();
                        float maxCaptionWidth = std::max(0.0f, imageRect.size.x - 8.0f);
                        sf::String fittedCaption = fitTextToWidthWithEllipsis(caption, captionSize, maxCaptionWidth);
                        if (fittedCaption.isEmpty()) {
                            folderModeClickAreas.push_back({idx, cellRect});
                            continue;
                        }

                        sf::Text captionText(uiFont, fittedCaption, captionSize);
                        captionText.setFillColor(sf::Color::White);
                        sf::FloatRect cb = captionText.getLocalBounds();
                        float captionX = x + (layout.cellW - cb.size.x) / 2.0f - cb.position.x;
                        float captionY =
                            y + layout.cellH - captionHeight + (captionHeight - cb.size.y) / 2.0f - cb.position.y;
                        captionText.setPosition({captionX, captionY});
                        window->draw(captionText);
                    }
                    folderModeClickAreas.push_back({idx, cellRect});
                } else {
                    thumbnailClickAreas.push_back({idx, cellRect});
                }
            }
        }

        if (layout.totalRows > layout.visibleRows) {
            sf::FloatRect track = getThumbnailScrollbarTrackRect();

            sf::RectangleShape barBg(track.size);
            barBg.setPosition(track.position);
            barBg.setFillColor(sf::Color(40, 40, 40));
            window->draw(barBg);

            sf::FloatRect thumb = getThumbnailScrollbarThumbRect();
            sf::RectangleShape barThumb(thumb.size);
            barThumb.setPosition(thumb.position);
            barThumb.setFillColor(sf::Color::Cyan);
            window->draw(barThumb);
        }
    }

    void parseFilterExpression(Filter &filter) {
        // Extract pattern from expression like "Keywords % 'NOMINUS'"
        std::regex expr_regex(R"(Keywords\s*%\s*'([^']+)')");
        std::smatch match;
        if (std::regex_search(filter.expression, match, expr_regex) && match.size() > 1) {
            filter.pattern = match[1];
        }
    }

    const std::string &getExifString(const fs::path &imagePath, const std::string &key) const {
        static const std::string empty;
        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt == imageMetadataCache.end()) {
            return empty;
        }
        const json &meta = imageIt->second;
        if (!meta.contains(key) || !meta[key].is_string()) {
            return empty;
        }
        return meta[key].get_ref<const std::string &>();
    }

    std::optional<std::int64_t> getTakenEpoch(const fs::path &imagePath) const {
        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt == imageMetadataCache.end()) {
            return std::nullopt;
        }
        return datetime_utils::getTakenEpochFromMetadata(imageIt->second);
    }

    bool hasKeywords(const fs::path &imagePath) const {
        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt == imageMetadataCache.end()) {
            return false;
        }
        const json &meta = imageIt->second;
        return meta.contains("Keywords") && meta["Keywords"].is_array();
    }

    std::vector<std::string> getKeywords(const fs::path &imagePath) const {
        std::vector<std::string> result;
        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt == imageMetadataCache.end()) {
            return result;
        }
        const json &meta = imageIt->second;
        if (!meta.contains("Keywords") || !meta["Keywords"].is_array()) {
            return result;
        }
        for (const auto &entry : meta["Keywords"]) {
            std::string trimmed = trimWhitespace(entry.get<std::string>());
            if (!trimmed.empty()) {
                result.push_back(trimmed);
            }
        }
        return result;
    }

    bool hasGpsLatitude(const fs::path &imagePath) const {
        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt == imageMetadataCache.end()) {
            return false;
        }
        const json &meta = imageIt->second;
        return meta.contains("GPSLatitude") && !meta["GPSLatitude"].is_null();
    }

    static double getGpsValueOrZero(const json &meta, const char *key) {
        if (!meta.contains(key)) {
            return 0.0;
        }
        const auto &field = meta[key];
        if (field.is_number()) {
            return field.get<double>();
        }
        return 0.0;
    }

    double getGpsValueOrZero(const fs::path &imagePath, const char *key) const {
        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt == imageMetadataCache.end()) {
            return 0.0;
        }
        return getGpsValueOrZero(imageIt->second, key);
    }

    int getOrientationOrDefault(const fs::path &imagePath) const {
        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt == imageMetadataCache.end()) {
            return 1;
        }
        const json &meta = imageIt->second;
        if (!meta.contains("Orientation")) {
            return 1;
        }
        const auto &field = meta["Orientation"];
        if (field.is_number_integer()) {
            return field.get<int>();
        }
        if (field.is_number()) {
            return static_cast<int>(field.get<double>());
        }
        return 1;
    }

    // Check if image passes the currently active filter (using pre-computed filter results)
    bool passesActiveFilter(const fs::path &imagePath) const {
        if (activeFilterIndex < 0 || activeFilterIndex >= static_cast<int>(filters.size())) {
            return true; // No filter active, all images pass
        }

        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt == imageMetadataCache.end()) {
            return true; // No metadata, assume it passes
        }

        const json &meta = imageIt->second;
        const std::string &filterKey = filters[activeFilterIndex].key;

        return meta["filters"][filterKey]; // Implicit conversion: null/false->false, true->true
    }

    bool matchesFilter(const std::vector<std::string> &keywords) {
        if (activeFilterIndex < 0 || activeFilterIndex >= static_cast<int>(filters.size())) {
            return true; // No filter active
        }

        const std::string &pattern = filters[activeFilterIndex].pattern;
        if (pattern.empty())
            return true;

        std::string trimmedPattern = trimWhitespace(pattern);
        for (const auto &keyword : keywords) {
            if (keyword == trimmedPattern) {
                return true;
            }
        }
        return false;
    }

    // Evaluate a filter expression for given keywords and return true/false
    bool evaluateFilterExpression(const std::string &expression, const std::string &pattern,
                                  const std::vector<std::string> &keywords) {
        if (pattern.empty())
            return true;

        std::string trimmedPattern = trimWhitespace(pattern);
        for (const auto &keyword : keywords) {
            if (keyword == trimmedPattern) {
                return true;
            }
        }
        return false;
    }

    // Populate "filters" field in metadata for an image
    void populateFilterResults(const fs::path &imagePath) {
        if (filters.empty())
            return;

        const auto keywords = getKeywords(imagePath);
        json &meta = imageMetadataCache[imagePath];
        if (!meta.is_object()) {
            meta = json::object();
        }

        json filterResults = json::object();
        for (const auto &filter : filters) {
            bool matches = evaluateFilterExpression(filter.expression, filter.pattern, keywords);
            filterResults[filter.key] = matches;
        }
        meta["filters"] = filterResults;
    }

    // Helper to show current image on map with configured zoom
    void showCurrentImageOnMap() {
        if (!mapViewer || allImagePaths.empty())
            return;

        const auto &imagePath = allImagePaths[currentIndex];
        if (hasGpsLatitude(imagePath)) {
            double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
            double lon = getGpsValueOrZero(imagePath, "GPSLongitude");
            mapViewer->showMap(lat, lon, defaultZoom);
        }
    }

    bool isSupportedImage(const fs::path &path) const {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        for (const auto &fmt : supportedSuffixes) {
            if (ext == fmt)
                return true;
        }
        return false;
    }

    // Find first folder with images in subtree (depth-first)
    fs::path findFirstFolderWithImages(const fs::path &dir) {
        // First check if this folder has images
        try {
            for (const auto &entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file() && isSupportedImage(entry.path())) {
                    return dir; // Found images here
                }
            }
        } catch (...) {
            return fs::path();
        }

        // No images in this folder, check children
        try {
            std::vector<fs::path> children;
            for (const auto &entry : fs::directory_iterator(dir)) {
                if (entry.is_directory()) {
                    children.push_back(normalizePath(entry.path()));
                }
            }
            std::sort(children.begin(), children.end());

            for (const auto &child : children) {
                fs::path result = findFirstFolderWithImages(child);
                if (!result.empty()) {
                    return result;
                }
            }
        } catch (...) {
            return fs::path();
        }

        return fs::path();
    }

    void loadUIFont() {
        std::string os = getOs();

        for (const auto &fontPath : config["font"]["by_os"][os]["main"]) {
            std::string path = fontPath.get<std::string>();
            if (uiFont.openFromFile(path)) {
                uiFontLoaded = true;
                break;
            }
        }
    }

    bool loadConfig(const fs::path &searchDir = "", const std::string &configFileName = "") {
        try {
            // Load and validate config using schema-driven system
            try {
                if (!configFileName.empty()) {
                    // Load specific config file directly
                    fs::path fullPath = searchDir / configFileName;
                    config = loadAndValidateConfigFile(fullPath);
                } else {
                    // Use the default config search (looks for mgvwr.yaml in searchDir or current dir)
                    config = loadAndValidateConfig(searchDir);
                }
            } catch (const std::exception &e) {
                throw std::runtime_error(std::string("Failed to load and enrich config: ") + e.what());
            }

            // Extract values into member variables (schema has already validated and applied defaults)
            try {
                singleInstanceMode = config["single_instance_mode"].get<bool>();
                quietMode = config["quiet_mode"].get<bool>();
                experimental = config["map"]["viewer"]["window"]["inline"].get<bool>();
                homeCountry = config["home_country"].get<std::string>();
                geoKeywordPrefix = config["geo_keyword_prefix"].get<std::string>();

                // Load regions list
                regions.clear();
                for (const auto &region : config["regions"]) {
                    regions.push_back(region.get<std::string>());
                }
            } catch (const nlohmann::json::exception &je) {
                throw std::runtime_error("Error reading config: " + std::string(je.what()));
            }

            // Image file configuration
            supportedSuffixes.clear();
            for (const auto &suffix : config["image_file"]["supported_suffixes"]) {
                supportedSuffixes.push_back(suffix.get<std::string>());
            }

            // Font configuration
            fontSizeConfig = config["font"]["size"];

            // Watched folders: ordered array of {folder, auto_scan} objects; auto_scan defaults to false.
            watchedFolderAutoScan.clear();
            watchedFolders.clear();
            for (const auto &entry : config["watched_folders"]) {
                fs::path folderPath = normalizePath(fs::path(entry["folder"].get<std::string>()));
                bool autoScan = entry.value("auto_scan", false);
                watchedFolderAutoScan[folderPath] = autoScan;
                watchedFolders.push_back(folderPath);
            }

            // Path classifications
            pathClassifications.clear();
            for (const auto &classificationObj : config["path_classifications"]) {
                PathClassification pc;
                pc.pattern = classificationObj["pattern"];
                for (const auto &name : classificationObj["names"]) {
                    pc.names.push_back(name.get<std::string>());
                }
                pathClassifications.push_back(pc);
            }

            // Filters
            filters.clear();
            for (const auto &filterObj : config["filters"]) {
                Filter f;
                f.expression = filterObj["expression"];
                f.key = filterObj["key"];
                parseFilterExpression(f);
                filters.push_back(f);
            }

            // Map configuration
            // Map window size (always read for layout calculation, store as JSON)
            const auto &sizeArray = config["map"]["viewer"]["window"]["size"];
            mapWindowWidth = sizeArray[0];
            mapWindowHeight = sizeArray[1];

            // Map zoom levels
            defaultZoom = config["map"]["viewer"]["zoom"]["default"];
            minZoom = config["map"]["viewer"]["zoom"]["minimum"];
            maxZoom = config["map"]["viewer"]["zoom"]["maximum"];

            // Map links
            maps.clear();
            for (const auto &mapObj : config["map"]["links"]) {
                Map m;
                m.name = mapObj["name"];
                m.zoom = mapObj["zoom"];
                m.gui_url_template = mapObj["gui_url_template"];
                if (mapObj["enabled"]) {
                    maps.push_back(m);
                }
            }

            // Cache configuration
            const auto &cacheConfig = config["map"]["cache"];
            cacheEnabled = cacheConfig["enabled"];
            maxCacheSizeMB = cacheConfig["max_size_mb"];
            cacheLocation = cacheConfig["location"];

            // Window mode
            const auto &wmConfig = config["window_mode"];
            windowModeIsDefault = wmConfig["is_default"];
            defaultWindowWidth = wmConfig["default_size"][0];
            defaultWindowHeight = wmConfig["default_size"][1];

            return true;
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Config loading error: ") + e.what());
        }
    }

    fs::path findWatchedFolder(const fs::path &imagePath) {
        fs::path absImagePath = fs::absolute(imagePath);
        log_stdout("DEBUG", "findWatchedFolder: Looking for watched folder containing: ", absImagePath.string());

        for (const auto &watchedFolder : watchedFolders) {
            fs::path absWatched = fs::absolute(watchedFolder);
            log_stdout("DEBUG", "findWatchedFolder: Checking against: ", absWatched.string());

            // Check if imagePath is within this watched folder
            try {
                auto rel = fs::relative(absImagePath, absWatched);
                // If relative path doesn't start with .., it's within the watched folder
                std::string relStr = rel.string();
                log_stdout("DEBUG", "findWatchedFolder: Relative path: '", relStr, "'");
                // Empty means different drives on Windows, ".." means outside watched folder
                if (!relStr.empty() && relStr.substr(0, 2) != "..") {
                    log_stdout("DEBUG", "findWatchedFolder: MATCH! Returning: ", absWatched.string());
                    return absWatched;
                }
            } catch (const std::exception &e) {
                log_stdout("DEBUG", "findWatchedFolder: Exception: ", e.what());
            } catch (...) {
                log_stdout("DEBUG", "findWatchedFolder: Unknown exception (likely different drives)");
            }
        }

        log_stdout("DEBUG", "findWatchedFolder: No watched folder found");
        return fs::path(); // Not in any watched folder
    }

    // Helper to extract keywords from JSON
    std::vector<std::string> extractKeywordsFromJson(const json &obj) {
        if (!obj.contains("Keywords")) {
            return {};
        }

        // Schema guarantees Keywords is an array of strings
        const auto &keywordsField = obj["Keywords"];
        std::vector<std::string> result;
        for (const auto &entry : keywordsField) {
            std::string trimmed = trimWhitespace(entry.get<std::string>());
            if (!trimmed.empty()) {
                result.push_back(trimmed);
            }
        }
        return result;
    }

    // Helper to build URL from template with GPS coordinates using inja
    std::string buildMapURL(const std::string &template_url, double latitude, double longitude, int zoom_level) {
        inja::Environment env;
        inja::json data;
        data["GPSLatitude"] = latitude;
        data["GPSLongitude"] = longitude;
        data["zoom"] = zoom_level;

        return env.render(template_url, data);
    }

    // Helper to open URL in default browser
    void openURL(const std::string &url) {
#ifdef _WIN32
        // Windows: use ShellExecuteA
        ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOW);
#elif __APPLE__
        // macOS: use open command
        std::string cmd = "open \"" + url + "\"";
        system(cmd.c_str());
#else
        // Linux: use xdg-open
        std::string cmd = "xdg-open \"" + url + "\" &";
        system(cmd.c_str());
#endif
    }

    // Calculate actual font size in pixels based on fullscreen width
    // Font size stays consistent regardless of windowed/fullscreen mode
    unsigned int getCalculatedFontSize() const { return parseSizeValue(fontSizeConfig, fullscreenWidth); }

    fs::path resolveAppIconPath(const fs::path &exePath) const {
        std::vector<fs::path> candidates;
        candidates.push_back(fs::current_path() / "icons" / "MgVwr.ico");
        candidates.push_back(fs::current_path().parent_path() / "icons" / "MgVwr.ico");

        if (!exePath.empty()) {
            fs::path exeDir = fs::absolute(exePath).parent_path();
            candidates.push_back(exeDir / "icons" / "MgVwr.ico");
            candidates.push_back(exeDir.parent_path() / "icons" / "MgVwr.ico");
        }

        for (const auto &candidate : candidates) {
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                return candidate;
            }
        }

        return fs::path();
    }

    void applyWindowIcon() {
        if (!window || appIconPath.empty()) {
            return;
        }

        if (!appIconLoadAttempted) {
            appIconLoadAttempted = true;
            appIconLoaded = appIconImage.loadFromFile(appIconPath.string());
            if (!appIconLoaded) {
                log_stdout("DEBUG", "Failed to load window icon: ", appIconPath.string());
            }
        }

        if (appIconLoaded) {
            window->setIcon(appIconImage);
        }
    }

    void createWindow(bool fullscreen) {
        isFullscreen = fullscreen;
        sf::VideoMode mode = fullscreen ? desktopMode : sf::VideoMode(windowedSize);
        sf::State state = fullscreen ? sf::State::Fullscreen : sf::State::Windowed;
        std::uint32_t style = fullscreen ? sf::Style::None : sf::Style::Default;
        std::string title = fullscreen ? "MgVwr" : windowTitle;

        window = std::make_shared<sf::RenderWindow>(mode, title, style, state);
        window->setFramerateLimit(60);
        applyWindowIcon();

        // Restore window position if we have a stored state and we're in windowed mode
        if (!fullscreen && hasStoredWindowState) {
            window->setPosition(windowedPosition);
        }
    }

    void toggleWindowMode() {
        if (isFullscreen) {
            // Going from fullscreen to windowed - use stored state if available
            createWindow(false);
            // Update map viewer with configured map dimensions (not full window)
            if (mapViewer) {
                auto windowSize = window->getSize();
                int newMapWidth = parseSizeValue(mapWindowWidth, windowSize.x);
                int newMapHeight = parseSizeValue(mapWindowHeight, windowSize.y);
                mapViewer->onWindowResize(newMapWidth, newMapHeight);
            }
        } else {
            // Going from windowed to fullscreen - store current window state
            windowedSize = window->getSize();
            windowedPosition = window->getPosition();
            hasStoredWindowState = true;
            createWindow(true);
            // Update map viewer with configured map dimensions (not full window)
            if (mapViewer) {
                int newMapWidth = parseSizeValue(mapWindowWidth, desktopMode.size.x);
                int newMapHeight = parseSizeValue(mapWindowHeight, desktopMode.size.y);
                mapViewer->onWindowResize(newMapWidth, newMapHeight);
            }
        }
        isHandCursorActive = false; // Reset cursor state when toggling modes
        // Don't reload image - just update sprite positioning for new window size
        if (!allImagePaths.empty() && sprite && texture) {
            updateSpritePositioning();
        }
    }

    // Recalculate sprite positioning for current window size (called when resizing window)
    void updateSpritePositioning() {
        if (!sprite || !texture)
            return;

        auto windowSize = window->getSize();
        float windowWidth = static_cast<float>(windowSize.x);
        float windowHeight = static_cast<float>(windowSize.y);

        auto textureSize = texture->getSize();
        float textureWidth = static_cast<float>(textureSize.x);
        float textureHeight = static_cast<float>(textureSize.y);

        // Get current rotation from sprite
        float rotationDegrees = sprite->getRotation().asDegrees();
        float rotation = rotationDegrees;

        // Account for rotation when calculating display size
        if ((rotation >= 45.0f && rotation <= 135.0f) || (rotation >= 225.0f && rotation <= 315.0f)) {
            std::swap(textureWidth, textureHeight);
        }

        float scale;

        if (experimental) {
            // Experimental layout: reserve map width pixels on LEFT, no right space
            float mapReserved = static_cast<float>(parseSizeValue(mapWindowWidth, windowSize.x));
            float availableWidth = windowWidth - mapReserved;

            // Calculate scale based on available width and full window height
            float scaleX = availableWidth / textureWidth;
            float scaleY = windowHeight / textureHeight;
            scale = std::min(scaleX, scaleY);
            fitImageScale = scale;
        } else {
            // Original layout: center image in full window
            float scaleX = windowWidth / textureWidth;
            float scaleY = windowHeight / textureHeight;
            scale = std::min(scaleX, scaleY);
            fitImageScale = scale;
        }

        applyScaleWithCanonicalPosition(scale);
    }

    void applyScaleWithCanonicalPosition(float scale) {
        if (!sprite || !texture) {
            return;
        }

        auto windowSize = window->getSize();
        float windowWidth = static_cast<float>(windowSize.x);
        float windowHeight = static_cast<float>(windowSize.y);

        auto textureSize = texture->getSize();
        float textureWidth = static_cast<float>(textureSize.x);
        float textureHeight = static_cast<float>(textureSize.y);

        // Account for rotation when calculating display size.
        float rotation = sprite->getRotation().asDegrees();
        if ((rotation >= 45.0f && rotation <= 135.0f) || (rotation >= 225.0f && rotation <= 315.0f)) {
            std::swap(textureWidth, textureHeight);
        }

        sprite->setScale({scale, scale});

        float scaledWidth = textureWidth * scale;
        float scaledHeight = textureHeight * scale;
        float posX = (windowWidth - scaledWidth) / 2.0f + scaledWidth / 2.0f;
        float posY = (windowHeight - scaledHeight) / 2.0f + scaledHeight / 2.0f;

        if (experimental) {
            float mapReserved = static_cast<float>(parseSizeValue(mapWindowWidth, windowSize.x));
            float leftEdgeX = posX - scaledWidth / 2.0f;
            if (leftEdgeX < mapReserved) {
                posX += (mapReserved - leftEdgeX);
            }
        }

        sprite->setPosition({posX, posY});
        currentImageScale = scale;
    }

    bool zoomImageAtCursor(const sf::Vector2f &cursorPos, float wheelDelta) {
        if (!sprite || !texture || wheelDelta == 0.0f) {
            return false;
        }

        if (!sprite->getGlobalBounds().contains(cursorPos)) {
            return false;
        }

        const float minScale = fitImageScale;
        const float maxScale = std::max(minScale, 2.0f);
        const float step = 1.10f;
        const float oldScale = sprite->getScale().x;

        float targetScale = oldScale;
        if (wheelDelta > 0.0f) {
            targetScale = oldScale * step;
        } else if (wheelDelta < 0.0f) {
            targetScale = oldScale / step;
        }

        targetScale = std::clamp(targetScale, minScale, maxScale);

        // Consume wheel over image area even when already clamped at a limit.
        if (targetScale == oldScale) {
            jumpedToOldest = false;
            if (wheelDelta > 0.0f && oldScale >= maxScale) {
                navigationMessage = "Reached maximum zoom";
            } else if (wheelDelta < 0.0f && oldScale <= minScale) {
                navigationMessage = "Reached minimum zoom";
            }
            return true;
        }

        jumpedToOldest = false;
        if (wheelDelta > 0.0f && targetScale >= maxScale) {
            navigationMessage = "Reached maximum zoom";
        } else if (wheelDelta < 0.0f && targetScale <= minScale) {
            navigationMessage = "Reached minimum zoom";
        } else {
            navigationMessage.clear();
        }

        // Zoom-out should target canonical image placement, not cursor anchoring.
        if (wheelDelta < 0.0f) {
            applyScaleWithCanonicalPosition(targetScale);
            return true;
        }

        // Keep the same image pixel under the cursor after scaling.
        sf::Vector2f localPoint = sprite->getInverseTransform().transformPoint(cursorPos);
        sprite->setScale({targetScale, targetScale});
        sf::Vector2f after = sprite->getTransform().transformPoint(localPoint);
        sprite->setPosition(sprite->getPosition() + (cursorPos - after));

        currentImageScale = targetScale;
        return true;
    }

    void ensureMetadataForImage(const fs::path &imagePath) {
        metadata::ProviderOptions providerOptions;
        providerOptions.cacheEnabled = metadataCacheReady;
        providerOptions.cacheFilePath = metadataCacheFilePath;
        providerOptions.exiftoolAvailable = exiftoolAvailable;
        providerOptions.exiftoolPath = g_exiftoolPath;

        metadata::ensureMetadataForImage(imagePath, imageMetadataCache, providerOptions,
                                         [this](const fs::path &p) { invalidateThumbnailCache(p); });
        populateFilterResults(imagePath);
    }

    void buildImageList(const fs::path &startDir) {
        refreshThumbnailsAfterImageSetChange();

        allImagePaths.clear();
        allDirectories.clear();
        currentFolder = normalizePath(startDir);

        // Update current watched folder when changing directories
        fs::path newWatchedFolder = findWatchedFolder(currentFolder);
        if (!newWatchedFolder.empty()) {
            folderModeEligible = true;
            if (newWatchedFolder != currentWatchedFolder) {
                log_stdout("DEBUG", "buildImageList: Updating currentWatchedFolder from '",
                           currentWatchedFolder.string(), "' to '", newWatchedFolder.string(), "'");
            }
            currentWatchedFolder = newWatchedFolder;
        } else {
            folderModeEligible = false;
            log_stdout("DEBUG", "buildImageList: Warning - no watched folder found for: ", currentFolder.string());
        }

        // Check if folder is already cached
        if (folderCaches.find(startDir) != folderCaches.end()) {
            log_stdout("Using cached folder: ", startDir.string());
            FolderCache &cache = folderCaches[startDir];
            allImagePaths = cache.images;
            imageMetadataCache = cache.metadata;
            sortByNameCurrentFolder = cache.sortByName;
            deferMetadataCurrentFolder = cache.deferMetadata;

            for (size_t i = 0; i < allImagePaths.size(); i++) {
                allDirectories.push_back(startDir);
            }
            return;
        }

        // Scan the folder (not cached yet)
        std::vector<fs::path> dirImages;
        try {
            for (const auto &entry : fs::directory_iterator(startDir)) {
                if (entry.is_regular_file() && isSupportedImage(entry.path())) {
                    dirImages.push_back(entry.path());
                }
            }
        } catch (...) {
            // Skip unreadable directories
        }

        metadata::ProviderOptions providerOptions;
        providerOptions.cacheEnabled = metadataCacheReady;
        providerOptions.cacheFilePath = metadataCacheFilePath;
        providerOptions.exiftoolAvailable = exiftoolAvailable;
        providerOptions.exiftoolPath = g_exiftoolPath;

        metadata::fillMetadataForFolder(dirImages, imageMetadataCache, providerOptions, deferMetadataCurrentFolder,
                                        sortByNameCurrentFolder,
                                        [this](const fs::path &p) { invalidateThumbnailCache(p); });

        for (const auto &imagePath : dirImages) {
            populateFilterResults(imagePath);
        }

        // Sort images by shooting date/time if allowed, else/then by filename if datetimes are equal
        std::sort(dirImages.begin(), dirImages.end(), [this](const fs::path &a, const fs::path &b) {
            std::string aName = a.filename().string();
            std::string bName = b.filename().string();
            std::transform(aName.begin(), aName.end(), aName.begin(), ::tolower);
            std::transform(bName.begin(), bName.end(), bName.begin(), ::tolower);

            if (sortByNameCurrentFolder) {
                return aName < bName;
            }

            auto aTaken = getTakenEpoch(a);
            auto bTaken = getTakenEpoch(b);
            if (aTaken.has_value() != bTaken.has_value()) {
                return aTaken.has_value();
            }
            if (aTaken.has_value() && bTaken.has_value() && *aTaken != *bTaken) {
                return *aTaken < *bTaken;
            }
            return aName < bName;
        });
        // std::sort(dirImages.begin(), dirImages.end(),
        //     [this](const fs::path& a, const fs::path& b) {
        //         if (!sortByNameCurrentFolder) {
        //             std::string dateA = getExifString(a, "DateTimeOriginal");
        //             std::string dateB = getExifString(b, "DateTimeOriginal");
        //             if (dateA != dateB) {
        //                 return dateA < dateB;
        //             }
        //         }
        //         // Same datetime or sort by datetime not allowed: sort by filename
        //         std::string nameA = a.filename().string();
        //         std::string nameB = b.filename().string();
        //         std::transform(nameA.begin(), nameA.end(), nameA.begin(), ::tolower);
        //         std::transform(nameB.begin(), nameB.end(), nameB.begin(), ::tolower);
        //         return nameA < nameB;
        //     }
        // );

        for (const auto &img : dirImages) {
            allImagePaths.push_back(img);
            allDirectories.push_back(startDir);
        }

        // Cache this folder for future use
        FolderCache cache;
        cache.images = allImagePaths;
        cache.metadata = imageMetadataCache;
        cache.sortByName = sortByNameCurrentFolder;
        cache.deferMetadata = deferMetadataCurrentFolder;
        cache.folderPath = startDir;
        folderCaches[startDir] = cache;

        log_stdout("DEBUG", "Indexed ", allImagePaths.size(), " images in folder:", "\n  ", startDir.string(),
                   " (cached)");
        for (size_t i = 0; i < allImagePaths.size() && i < 3; i++) {
            log_stdout("DEBUG", "[", (i + 1), "]", allImagePaths[i].string());
        }

        // Start pre-caching thread if not already running
        if (!isPreCaching) {
            if (preCacheThread.joinable()) {
                preCacheThread.join();
            }
            preCacheThread = std::thread(&MgVwr::preCacheNextAndPrevFolders, this);
        }
    }

    void loadImage(size_t index) {
        if (index >= allImagePaths.size()) {
            log_stderr("Invalid image index");
            return;
        }

        currentIndex = index;
        const auto &imagePath = allImagePaths[currentIndex];

        fs::path shownFolder = normalizePath(imagePath.parent_path());
        if (std::find(seenImageFolders.begin(), seenImageFolders.end(), shownFolder) == seenImageFolders.end()) {
            seenImageFolders.push_back(shownFolder);
        }

        ensureMetadataForImage(imagePath);

        texture = std::make_shared<sf::Texture>();
        if (!texture->loadFromFile(imagePath.string())) {
            log_stderr("Failed to load image: ", imagePath);
            return;
        }

        sprite = std::make_shared<sf::Sprite>(*texture);

        // Get EXIF orientation and rotate sprite accordingly
        int orientation = getOrientationOrDefault(imagePath);
        float rotation = 0.0f;
        bool flipH = false, flipV = false;

        switch (orientation) {
        case 2:
            flipH = true;
            break;
        case 3:
            rotation = 180.0f;
            break;
        case 4:
            flipV = true;
            break;
        case 5:
            rotation = 90.0f;
            flipH = true;
            break;
        case 6:
            rotation = 90.0f;
            break;
        case 7:
            rotation = 270.0f;
            flipH = true;
            break;
        case 8:
            rotation = 270.0f;
            break;
        default:
            break;
        }

        auto textureSize = texture->getSize();
        float textureWidth = static_cast<float>(textureSize.x);
        float textureHeight = static_cast<float>(textureSize.y);

        // Account for rotation when calculating display size
        if (rotation == 90.0f || rotation == 270.0f) {
            std::swap(textureWidth, textureHeight);
        }

        auto windowSize = window->getSize();
        float windowWidth = static_cast<float>(windowSize.x);
        float windowHeight = static_cast<float>(windowSize.y);

        // Set rotation origin to center for proper rotation and then apply fit layout.
        auto origSize = texture->getSize();
        sprite->setOrigin({origSize.x / 2.0f, origSize.y / 2.0f});
        sprite->setRotation(sf::degrees(rotation));
        updateSpritePositioning();

        // Format datetime output: skip time if 00:00:00, skip entirely if keywords start with the literal "+/-" string
        std::string dateTimeStr = getExifString(imagePath, "DateTimeOriginal");
        const auto &keywords = getKeywords(imagePath);
        bool hasSpecialKeyword = false;
        for (const auto &kw : keywords) {
            if (kw.length() >= 3 && kw.substr(0, 3) == "+/-") {
                hasSpecialKeyword = true;
                break;
            }
        }

        std::string displayDateTime;
        if (!hasSpecialKeyword && dateTimeStr != "0000:00:00 00:00:00") {
            // Check if time is 00:00:00
            if (dateTimeStr.length() >= 19 && dateTimeStr.substr(9) == " 00:00:00") {
                // Only show date
                displayDateTime = dateTimeStr.substr(0, 10);
            } else {
                displayDateTime = dateTimeStr;
            }
        }

        log_stdout("DEBUG", "Loaded [", (currentIndex + 1), "/", allImagePaths.size(), "]: ", "\n  ",
                   allImagePaths[currentIndex].string());
        if (!displayDateTime.empty()) {
            log_stdout("DEBUG", "DateTime: ", displayDateTime);
        }

        // Precache next image
        precacheNextImage();

        // Update map viewer state for the newly loaded image.
        if (mapViewer) {
            if (hasGpsLatitude(imagePath)) {
                double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
                double lon = getGpsValueOrZero(imagePath, "GPSLongitude");

                if (!mapViewer->isOpen()) {
                    mapViewer->showMap(lat, lon, defaultZoom);
                }

                // Only recenter map if new point is outside the center 50% visible area
                if (!mapViewer->isPointInStayPutArea(lat, lon)) {
                    mapViewer->updateGPS(lat, lon);
                } else {
                    // Inside center 50% - keep map centered but update marker position
                    mapViewer->updateMarkerOnly(lat, lon);
                }
            }

            if (mapViewer->isOpen()) {
                // Collect all GPS points from current folder to display on map (only for images passing filter)
                std::vector<std::pair<double, double>> folderGpsPoints;
                fs::path currentDir = imagePath.parent_path();
                for (size_t i = 0; i < allImagePaths.size(); i++) {
                    if (allDirectories[i] == currentDir && hasGpsLatitude(allImagePaths[i]) &&
                        passesActiveFilter(allImagePaths[i])) {
                        double ptLat = getGpsValueOrZero(allImagePaths[i], "GPSLatitude");
                        double ptLon = getGpsValueOrZero(allImagePaths[i], "GPSLongitude");
                        folderGpsPoints.push_back({ptLat, ptLon});
                    }
                }
                mapViewer->setGPSPoints(folderGpsPoints);
            }
        }

        if (thumbnailMode) {
            ensureThumbnailSelectionVisible();
        }
    }

    void precacheNextImage() {
        size_t nextIdx = currentIndex + 1;

        // Skip to next directory's first image if at end of current directory
        if (nextIdx < allImagePaths.size() && allDirectories[nextIdx] != allDirectories[currentIndex]) {
            return; // At folder boundary
        }

        if (nextIdx < allImagePaths.size()) {
            precachedTexture = std::make_shared<sf::Texture>();
            if (!precachedTexture->loadFromFile(allImagePaths[nextIdx].string())) {
                precachedTexture.reset();
            } else {
                log_stdout("DEBUG", "Precached: ", allImagePaths[nextIdx].filename().string());
            }
        }
    }

    fs::path getNextFolder() {
        // Navigate to next folder by checking sibling directories
        if (currentWatchedFolder.empty()) {
            return fs::path();
        }

        log_stdout("DEBUG", "getNextFolder: Starting from currentFolder: ", currentFolder.string());

        // First, check if current directory has subdirectories with images
        std::vector<fs::path> children;
        try {
            for (const auto &entry : fs::directory_iterator(currentFolder)) {
                if (entry.is_directory()) {
                    children.push_back(normalizePath(entry.path()));
                }
            }
        } catch (const std::exception &e) {
            log_stdout("DEBUG", "getNextFolder: Exception reading current folder: ", e.what());
        }

        std::sort(children.begin(), children.end());
        log_stdout("DEBUG", "getNextFolder: Found ", children.size(), " subdirectories");

        // Look for first child with images
        for (const auto &child : children) {
            log_stdout("DEBUG", "getNextFolder: Checking subdir: ", child.string());
            fs::path candidate = findFirstFolderWithImages(child);
            if (!candidate.empty()) {
                // Verify candidate is within the same watched folder
                fs::path candidateWatchedFolder = findWatchedFolder(candidate);
                log_stdout("DEBUG", "getNextFolder: Candidate watched folder: '", candidateWatchedFolder.string(),
                           "' vs current: '", currentWatchedFolder.string(), "'");
                if (candidateWatchedFolder == currentWatchedFolder) {
                    log_stdout("DEBUG", "getNextFolder: Found next folder in subdirs: ", candidate.string());
                    return candidate;
                }
            }
        }

        // No subdirectories with images, now look for siblings
        fs::path checkDir = currentFolder;

        // Walk up the tree until we find a next sibling with images
        while (checkDir >= currentWatchedFolder) {
            // Stop if we've reached the watched folder boundary
            if (checkDir == currentWatchedFolder) {
                break;
            }

            fs::path parent = checkDir.parent_path();

            // Get all immediate children of parent
            std::vector<fs::path> siblings;
            try {
                for (const auto &entry : fs::directory_iterator(parent)) {
                    if (entry.is_directory()) {
                        siblings.push_back(normalizePath(entry.path()));
                    }
                }
            } catch (...) {
                // Can't read parent, go up one level
                if (parent == checkDir)
                    break; // Avoid infinite loop
                checkDir = parent;
                continue;
            }

            std::sort(siblings.begin(), siblings.end());

            // Find current directory in siblings
            auto it = std::find(siblings.begin(), siblings.end(), checkDir);

            if (it != siblings.end()) {
                // Look for next sibling
                for (auto next_it = std::next(it); next_it != siblings.end(); ++next_it) {
                    fs::path candidate = findFirstFolderWithImages(*next_it);
                    if (!candidate.empty()) {
                        // Verify candidate is within the same watched folder
                        fs::path candidateWatchedFolder = findWatchedFolder(candidate);
                        log_stdout("DEBUG", "getNextFolder: Candidate watched folder: '",
                                   candidateWatchedFolder.string(), "' vs current: '", currentWatchedFolder.string(),
                                   "'");
                        if (candidateWatchedFolder == currentWatchedFolder) {
                            log_stdout("DEBUG", "getNextFolder: Found next folder: ", candidate.string());
                            return candidate;
                        } else {
                            log_stdout("DEBUG", "getNextFolder: Skipping candidate (different watched folder): ",
                                       candidate.string());
                        }
                    }
                }
            }

            // No next sibling at this level, go up one level
            if (parent == checkDir)
                break; // Avoid infinite loop at root
            checkDir = parent;
        }

        log_stdout("DEBUG", "getNextFolder: No next folder found");
        return fs::path();
    }

    fs::path getPrevFolder() {
        // Navigate to previous folder by checking sibling directories
        if (currentWatchedFolder.empty()) {
            return fs::path();
        }

        // First look for previous siblings (and their subdirectories)
        fs::path checkDir = currentFolder;

        // Walk up the tree until we find a previous sibling with images
        while (checkDir >= currentWatchedFolder) {
            // Stop if we've reached the watched folder boundary
            if (checkDir == currentWatchedFolder) {
                break;
            }

            fs::path parent = checkDir.parent_path();

            // Get all immediate children of parent
            std::vector<fs::path> siblings;
            try {
                for (const auto &entry : fs::directory_iterator(parent)) {
                    if (entry.is_directory()) {
                        siblings.push_back(normalizePath(entry.path()));
                    }
                }
            } catch (...) {
                // Can't read parent, go up one level
                if (parent == checkDir)
                    break; // Avoid infinite loop
                checkDir = parent;
                continue;
            }

            std::sort(siblings.begin(), siblings.end());

            // Find current directory in siblings
            auto it = std::find(siblings.begin(), siblings.end(), checkDir);

            if (it != siblings.end()) {
                // Look for previous sibling
                for (auto prev_it = std::make_reverse_iterator(it); prev_it != siblings.rend(); ++prev_it) {
                    fs::path candidate = findLastFolderWithImages(*prev_it);
                    if (!candidate.empty()) {
                        // Verify candidate is within the same watched folder
                        fs::path candidateWatchedFolder = findWatchedFolder(candidate);
                        log_stdout("DEBUG", "getPrevFolder: Candidate watched folder: '",
                                   candidateWatchedFolder.string(), "' vs current: '", currentWatchedFolder.string(),
                                   "'");
                        if (candidateWatchedFolder == currentWatchedFolder) {
                            log_stdout("DEBUG", "getPrevFolder: Found prev folder: ", candidate.string());
                            return candidate;
                        } else {
                            log_stdout("DEBUG", "getPrevFolder: Skipping candidate (different watched folder): ",
                                       candidate.string());
                        }
                    }
                }
            }

            // No previous sibling at this level, go up one level
            if (parent == checkDir)
                break; // Avoid infinite loop at root
            checkDir = parent;
        }

        // No siblings found, now check parent directories for images
        fs::path parent = currentFolder.parent_path();
        while (parent >= currentWatchedFolder && parent != currentFolder) {
            // Check if this parent level has images
            bool hasImages = false;
            try {
                for (const auto &entry : fs::directory_iterator(parent)) {
                    if (entry.is_regular_file() && isSupportedImage(entry.path())) {
                        hasImages = true;
                        break;
                    }
                }
            } catch (...) {
                // Can't read parent, try next level up
            }

            if (hasImages) {
                // Verify parent is within the same watched folder
                fs::path parentWatchedFolder = findWatchedFolder(parent);
                log_stdout("DEBUG", "getPrevFolder: Parent watched folder: '", parentWatchedFolder.string(),
                           "' vs current: '", currentWatchedFolder.string(), "'");
                if (parentWatchedFolder == currentWatchedFolder) {
                    log_stdout("DEBUG", "getPrevFolder: Found prev folder in parent: ", parent.string());
                    return parent;
                } else {
                    log_stdout("DEBUG",
                               "getPrevFolder: Skipping parent (different watched folder): ", parent.string());
                }
            }

            // No images at this level, go up one more
            if (parent == currentWatchedFolder)
                break; // Don't go above watched folder
            parent = parent.parent_path();
        }

        log_stdout("DEBUG", "getPrevFolder: No prev folder found");
        return fs::path();
    }

    // Find last folder with images in subtree (depth-first, rightmost branch)
    fs::path findLastFolderWithImages(const fs::path &dir) {
        // Get all children and sort them
        std::vector<fs::path> children;
        try {
            for (const auto &entry : fs::directory_iterator(dir)) {
                if (entry.is_directory()) {
                    children.push_back(normalizePath(entry.path()));
                }
            }
        } catch (...) {
            // Can't read directory
        }
        std::sort(children.begin(), children.end());

        // Search children in reverse order (rightmost first)
        for (auto child_it = children.rbegin(); child_it != children.rend(); ++child_it) {
            fs::path result = findLastFolderWithImages(*child_it);
            if (!result.empty()) {
                return result;
            }
        }

        // No images in any children, check this folder
        try {
            for (const auto &entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file() && isSupportedImage(entry.path())) {
                    return dir; // Found images here
                }
            }
        } catch (...) {
        }

        return fs::path();
    }

    void preCacheFolder(const fs::path &folderPath) {
        // Cache a folder without changing current display state
        if (folderPath.empty() || folderCaches.find(folderPath) != folderCaches.end()) {
            return; // Already cached
        }

        std::vector<fs::path> dirImages;
        try {
            for (const auto &entry : fs::directory_iterator(folderPath)) {
                if (entry.is_regular_file() && isSupportedImage(entry.path())) {
                    dirImages.push_back(entry.path());
                }
            }
        } catch (...) {
            return;
        }

        bool deferMetadata = false;

        ImageMetadataCache imageMetadata;

        auto getLocalExif = [&imageMetadata](const fs::path &path, const std::string &key) -> const std::string & {
            static const std::string empty;
            auto imageIt = imageMetadata.find(path);
            if (imageIt == imageMetadata.end()) {
                return empty;
            }
            const json &meta = imageIt->second;
            if (!meta.contains(key) || !meta[key].is_string()) {
                return empty;
            }
            return meta[key].get_ref<const std::string &>();
        };

        auto getLocalKeywords = [&imageMetadata](const fs::path &path) -> std::vector<std::string> {
            std::vector<std::string> result;
            auto imageIt = imageMetadata.find(path);
            if (imageIt == imageMetadata.end()) {
                return result;
            }
            const json &meta = imageIt->second;
            if (!meta.contains("Keywords") || !meta["Keywords"].is_array()) {
                return result;
            }
            for (const auto &entry : meta["Keywords"]) {
                std::string trimmed = trimWhitespace(entry.get<std::string>());
                if (!trimmed.empty()) {
                    result.push_back(trimmed);
                }
            }
            return result;
        };

        auto populateLocalFilterResults = [&](const fs::path &imagePath) {
            if (filters.empty())
                return;

            const auto keywords = getLocalKeywords(imagePath);
            json &meta = imageMetadata[imagePath];
            if (!meta.is_object()) {
                meta = json::object();
            }

            json filterResults = json::object();
            for (const auto &filter : filters) {
                bool matches = evaluateFilterExpression(filter.expression, filter.pattern, keywords);
                filterResults[filter.key] = matches;
            }
            meta["filters"] = filterResults;
        };

        bool sortByName = false;
        metadata::ProviderOptions providerOptions;
        providerOptions.cacheEnabled = metadataCacheReady;
        providerOptions.cacheFilePath = metadataCacheFilePath;
        providerOptions.exiftoolAvailable = exiftoolAvailable;
        providerOptions.exiftoolPath = g_exiftoolPath;

        metadata::fillMetadataForFolder(dirImages, imageMetadata, providerOptions, deferMetadata, sortByName,
                                        [this](const fs::path &p) { invalidateThumbnailCache(p); });

        for (const auto &imagePath : dirImages) {
            populateLocalFilterResults(imagePath);
        }

        if (deferMetadata) {
            std::sort(dirImages.begin(), dirImages.end(), [](const fs::path &a, const fs::path &b) {
                std::string aName = a.filename().string();
                std::string bName = b.filename().string();
                std::transform(aName.begin(), aName.end(), aName.begin(), ::tolower);
                std::transform(bName.begin(), bName.end(), bName.begin(), ::tolower);
                return aName < bName;
            });
        } else {
            // Sort images by shooting date/time
            std::sort(dirImages.begin(), dirImages.end(), [&getLocalExif](const fs::path &a, const fs::path &b) {
                std::string aName = a.filename().string();
                std::string bName = b.filename().string();
                std::transform(aName.begin(), aName.end(), aName.begin(), ::tolower);
                std::transform(bName.begin(), bName.end(), bName.begin(), ::tolower);

                auto takenEpoch = [&](const fs::path &path) -> std::optional<std::int64_t> {
                    std::string dateTimeOriginal = getLocalExif(path, "DateTimeOriginal");
                    std::string offsetTimeOriginal = getLocalExif(path, "OffsetTimeOriginal");
                    if (offsetTimeOriginal.empty()) {
                        offsetTimeOriginal = "+00:00";
                    }
                    return datetime_utils::exifTakenEpoch(dateTimeOriginal, offsetTimeOriginal);
                };

                auto aTaken = takenEpoch(a);
                auto bTaken = takenEpoch(b);
                if (aTaken.has_value() != bTaken.has_value()) {
                    return aTaken.has_value();
                }
                if (aTaken.has_value() && bTaken.has_value() && *aTaken != *bTaken) {
                    return *aTaken < *bTaken;
                }
                return aName < bName;
            });
        }

        // Cache this folder
        FolderCache cache;
        cache.images = dirImages;
        cache.metadata = imageMetadata;
        cache.sortByName = deferMetadata;
        cache.deferMetadata = deferMetadata;
        cache.folderPath = folderPath;
        folderCaches[folderPath] = cache;

        log_stdout("DEBUG", "Pre-cached ", dirImages.size(), " images in folder: ", folderPath.string());
    }

    void preCacheNextAndPrevFolders() {
        isPreCaching = true;
        log_stdout("DEBUG", "Starting pre-cache of next and prev folders...");

        // Pre-cache next folder
        fs::path nextFolder = getNextFolder();
        if (!nextFolder.empty()) {
            preCacheFolder(nextFolder);
        }

        // Pre-cache prev folder
        fs::path prevFolder = getPrevFolder();
        if (!prevFolder.empty()) {
            preCacheFolder(prevFolder);
        }

        log_stdout("DEBUG", "Pre-caching complete");
        isPreCaching = false;
    }

    // Get next image in current folder only (for Right arrow)
    size_t getNextInFolder() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false;
        navigationMessage = "";

        // Find next image that passes the active filter
        for (size_t i = currentIndex + 1; i < allImagePaths.size(); i++) {
            if (passesActiveFilter(allImagePaths[i])) {
                return i;
            }
        }

        // No more images that pass filter in current folder - try to jump to next folder's first image
        return getFirstInNextFolder();
    }

    // Navigate to next folder's first image (for PageDown)
    size_t getFirstInNextFolder() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false;
        navigationMessage = "";

        fs::path nextFolder = getNextFolder();
        while (!nextFolder.empty()) {
            std::string navigationType = classifyNavigation(currentFolder, nextFolder, pathClassifications);
            navigationMessage = "Jumped to next " + navigationType;
            log_stdout(navigationMessage, ": ", nextFolder.string());
            buildImageList(nextFolder);
            if (!allImagePaths.empty()) {
                // Find first image that passes the active filter
                for (size_t i = 0; i < allImagePaths.size(); i++) {
                    if (passesActiveFilter(allImagePaths[i])) {
                        return i;
                    }
                }
                // No images pass filter, try next folder
                nextFolder = getNextFolder();
                continue;
            }
            nextFolder = getNextFolder();
        }

        // No next folder
        if (currentWatchedFolder.empty()) {
            navigationMessage = "Reached last of folder";
        } else {
            navigationMessage = "Reached last of " + currentWatchedFolder.filename().string();
        }
        log_stderr(navigationMessage);
        return currentIndex;
    }

    // Get previous image in current folder only (for Left arrow)
    size_t getPrevInFolder() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false;
        navigationMessage = "";

        // Find previous image that passes the active filter
        if (currentIndex > 0) {
            for (size_t i = currentIndex; i > 0; i--) {
                if (passesActiveFilter(allImagePaths[i - 1])) {
                    return i - 1;
                }
            }
        }

        // No more images that pass filter before current - try to jump to previous folder's last image
        return getLastInPrevFolder();
    }

    // Navigate to previous folder's last image (for Left arrow at start)
    size_t getLastInPrevFolder() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false;
        navigationMessage = "";

        fs::path prevFolder = getPrevFolder();
        while (!prevFolder.empty()) {
            std::string navigationType = classifyNavigation(currentFolder, prevFolder, pathClassifications);
            navigationMessage = "Jumped to prev " + navigationType + " (last of folder)";
            log_stdout(navigationMessage, ": ", prevFolder.string());
            buildImageList(prevFolder);
            if (!allImagePaths.empty()) {
                // Find last image that passes the active filter
                for (size_t i = allImagePaths.size(); i > 0; i--) {
                    if (passesActiveFilter(allImagePaths[i - 1])) {
                        return i - 1;
                    }
                }
                // No images pass filter, try previous folder
                prevFolder = getPrevFolder();
                continue;
            }
            prevFolder = getPrevFolder();
        }

        // No previous folder
        if (currentWatchedFolder.empty()) {
            navigationMessage = "Reached first of folder";
        } else {
            navigationMessage = "Reached first of " + currentWatchedFolder.filename().string();
        }
        log_stderr(navigationMessage);
        return currentIndex;
    }

    // Navigate to previous folder's first image (for PageUp)
    size_t getFirstInPrevFolder() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false;
        navigationMessage = "";

        fs::path prevFolder = getPrevFolder();
        while (!prevFolder.empty()) {
            std::string navigationType = classifyNavigation(currentFolder, prevFolder, pathClassifications);
            navigationMessage = "Jumped to prev " + navigationType + " (1st of folder)";
            log_stdout(navigationMessage, ": ", prevFolder.string());
            buildImageList(prevFolder);
            if (!allImagePaths.empty()) {
                // Find first image that passes the active filter
                for (size_t i = 0; i < allImagePaths.size(); i++) {
                    if (passesActiveFilter(allImagePaths[i])) {
                        return i;
                    }
                }
                // No images pass filter, try previous folder
                prevFolder = getPrevFolder();
                continue;
            }
            prevFolder = getPrevFolder();
        }

        // No previous folder
        if (currentWatchedFolder.empty()) {
            navigationMessage = "Reached first of folder";
            log_stderr(navigationMessage);
        } else {
            navigationMessage = "Reached first of " + currentWatchedFolder.filename().string();
            log_stderr(navigationMessage);
        }
        return currentIndex;
    }

    // Get first image in current folder (for Home)
    size_t getFirstInFolder() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false;
        navigationMessage = "Jumped to first in folder";
        log_stdout(navigationMessage);

        // Find first image that passes the active filter
        for (size_t i = 0; i < allImagePaths.size(); i++) {
            if (passesActiveFilter(allImagePaths[i])) {
                return i;
            }
        }
        return 0; // Fallback if no images pass filter
    }

    // Get last image in current folder (for End)
    size_t getLastInFolder() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false;
        navigationMessage = "Jumped to last in folder";
        log_stdout(navigationMessage);

        // Find last image that passes the active filter
        for (size_t i = allImagePaths.size(); i > 0; i--) {
            if (passesActiveFilter(allImagePaths[i - 1])) {
                return i - 1;
            }
        }
        return allImagePaths.size() - 1; // Fallback if no images pass filter
    }

    size_t nextImage() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false; // Clear message when navigating

        size_t nextIdx = currentIndex + 1;

        if (nextIdx >= allImagePaths.size()) {
            // Try to jump to next folder
            fs::path nextFolder = getNextFolder();
            if (!nextFolder.empty()) {
                log_stdout("Moving to next folder: ", nextFolder.string());
                buildImageList(nextFolder);
                currentIndex = 0;
                return 0;
            }
            log_stdout("End of watched folder reached.");
            return currentIndex;
        }

        return nextIdx;
    }

    size_t prevImage() {
        if (allImagePaths.empty())
            return 0;

        jumpedToOldest = false; // Clear message when navigating

        if (currentIndex == 0) {
            // Try to jump to previous folder
            fs::path prevFolder = getPrevFolder();
            if (!prevFolder.empty()) {
                log_stdout("Moving to previous folder: ", prevFolder.string());
                buildImageList(prevFolder);
                currentIndex = allImagePaths.size() - 1;
                return currentIndex;
            }
            log_stdout("Beginning of watched folder reached.");
            return 0;
        }

        return currentIndex - 1;
    }

  public:
    MgVwr(const fs::path &imagePath, const fs::path &exePath = "", const std::string &configPath = "") {
        // Store executable path for config loading
        fs::path executableDir = exePath.empty() ? fs::current_path() : fs::absolute(exePath).parent_path();

        // Load configuration from specified path, or executable directory, then current directory
        if (!configPath.empty()) {
            loadConfig(fs::path(configPath).parent_path(), fs::path(configPath).filename().string());
        } else {
            loadConfig(executableDir);
        }

        // Enforce single instance mode if enabled
        if (singleInstanceMode) {
            wasReloaded = enforceSingleInstance();
        }

        // Create window (fullscreen or windowed based on config)
        desktopMode = sf::VideoMode::getDesktopMode();
        fullscreenWidth = desktopMode.size.x; // Cache fullscreen width for font calculations

        // Calculate initial windowed size based on config
        unsigned int winWidth = parseSizeValue(defaultWindowWidth, desktopMode.size.x);
        unsigned int winHeight = parseSizeValue(defaultWindowHeight, desktopMode.size.y);
        windowedSize = sf::Vector2u(winWidth, winHeight);

        // Center the window on screen
        windowedPosition = sf::Vector2i((desktopMode.size.x - winWidth) / 2, (desktopMode.size.y - winHeight) / 2);

        windowTitle = exePath.empty() ? "mgvwr.exe" : fs::path(exePath).filename().string();
        appIconPath = resolveAppIconPath(exePath);

        // Start in windowed or fullscreen mode based on config
        bool startFullscreen = !windowModeIsDefault;
        createWindow(startFullscreen);

        // Show loading screen
        window->clear(sf::Color::Black);

        // Load font for loading text from config by_os
        sf::Font loadingFont;
        std::string os = getOs();

        bool fontLoaded = false;
        for (const auto &fontPath : config["font"]["by_os"][os]["main"]) {
            std::string path = fontPath.get<std::string>();
            if (loadingFont.openFromFile(path)) {
                fontLoaded = true;
                break;
            }
        }

        if (fontLoaded && !quietMode) {
            std::string loadingStr = wasReloaded ? "Reloading" : "Loading";
            unsigned int loadingFontSize = getCalculatedFontSize() * 3; // 3x the normal size
            sf::Text loadingText(loadingFont, loadingStr, loadingFontSize);
            loadingText.setFillColor(sf::Color::White);

            // Center the text
            sf::FloatRect textBounds = loadingText.getLocalBounds();
            float windowWidth = window->getSize().x;
            float windowHeight = window->getSize().y;
            loadingText.setPosition(
                sf::Vector2f((windowWidth - textBounds.size.x) / 2.0f, (windowHeight - textBounds.size.y) / 2.0f));

            window->draw(loadingText);
        }
        window->display();

        loadUIFont();

        // Check if exiftool is available
        std::string resolvedExiftoolPath;
        bool found = metadata::findExiftool(resolvedExiftoolPath);
        g_exiftoolPath = resolvedExiftoolPath;
        exiftoolAvailable = found && !g_exiftoolPath.empty();

        // Initialize map viewer with cache configuration
        if (cacheEnabled) {
            // Use default cache location if not specified
            if (cacheLocation.empty()) {
                cacheLocation = getDefaultCacheLocation().string();
            }

            std::string metadataCacheError;
            fs::path metadataCacheFile = metadata_cache::defaultMetadataCacheFile(fs::path(cacheLocation));
            if (metadata_cache::initializeMetadataCache(metadataCacheFile, metadataCacheError)) {
                metadataCacheReady = true;
                metadataCacheFilePath = metadataCacheFile;
                log_stdout("Metadata cache initialized at: ", pathToString(metadataCacheFile.make_preferred()));
            } else {
                metadataCacheReady = false;
                log_stderr("Metadata cache initialization failed: ", metadataCacheError);
            }

            // Append "osm" subdirectory
            fs::path osmCachePath = fs::path(cacheLocation) / "osm";
            std::string osmCacheDir = osmCachePath.string();
            // Parse window size (supports both percentages and absolute pixels)
            int mapWinWidth = parseSizeValue(mapWindowWidth, desktopMode.size.x);
            int mapWinHeight = parseSizeValue(mapWindowHeight, desktopMode.size.y);
            mapViewer = std::make_unique<MapViewer>(osmCacheDir, maxCacheSizeMB, mapWinWidth, mapWinHeight,
                                                    experimental, minZoom, maxZoom);
            log_stdout("Map cache initialized at: ", pathToString(osmCachePath.make_preferred()),
                       " (window size: ", mapWinWidth, "x", mapWinHeight, " from config: ", mapWindowWidth, "x",
                       mapWindowHeight, ", inline mode: ", experimental ? "true" : "false", ")");
        }

        // Load help content after filters are loaded
        helpLines = loadHelpContent(config);

        if (imagePath.empty()) {
            if (watchedFolders.empty()) {
                throw std::runtime_error("No watched folders configured");
            }
            enterWatchedFoldersMode();
            return;
        }

        // Validate input and normalize paths to uppercase drive letters
        fs::path absPath = fs::absolute(imagePath);
        absPath = normalizePath(absPath);

        if (!fs::exists(absPath)) {
            throw std::runtime_error("File not found: " + absPath.string());
        }

        if (!isSupportedImage(absPath)) {
            throw std::runtime_error("Unsupported image format: " + absPath.string());
        }

        // Check if image is in a watched folder
        currentWatchedFolder = findWatchedFolder(absPath);

        if (currentWatchedFolder.empty()) {
            // Not in a watched folder - use just that folder
            log_stdout("Image not in watched folder. Using folder only: ", absPath.parent_path().string());
            currentWatchedFolder = normalizePath(absPath.parent_path());
        } else {
            log_stdout("Image is in watched folder: ", currentWatchedFolder.string());
        }

        // IMPORTANT: Always use the image's parent directory for caching, not the watched folder root
        fs::path imageDir = normalizePath(absPath.parent_path());
        log_stdout("Scanning image directory: ", imageDir.string());

        // Build image list for the image's directory
        buildImageList(imageDir);

        if (allImagePaths.empty()) {
            throw std::runtime_error("No supported images found in folder");
        }

        currentIndex = 0;
        for (size_t i = 0; i < allImagePaths.size(); i++) {
            if (allImagePaths[i] == absPath) {
                currentIndex = i;
                break;
            }
        }

        jumpedToOldest = false;

        // Load initial image
        loadImage(currentIndex);

        // Show map on startup if image has GPS data
        if (mapViewer && hasGpsLatitude(allImagePaths[currentIndex])) {
            showCurrentImageOnMap();
        }
    }

    void run() {
        while (window->isOpen()) {
            while (const auto event = window->pollEvent()) {
                // Forward events to inline map if it's open and mouse is over map area
                bool eventHandledByMap = false;
                if (experimental && mapViewer && mapViewer->isOpen()) {
                    const sf::Texture *mapTexture = mapViewer->getTexture();
                    if (mapTexture) {
                        if (!currentMapSubjectHasGps()) {
                            mapTexture = nullptr;
                        }
                        bool skipMapForwarding = contextMenuVisible;
                        if (const auto *mouseButtonEvent = event->getIf<sf::Event::MouseButtonPressed>()) {
                            skipMapForwarding =
                                skipMapForwarding || (mouseButtonEvent->button == sf::Mouse::Button::Right);
                        }
                        if (skipMapForwarding) {
                            // Context menu interaction (and right click opening it) should not be swallowed by map.
                            mapTexture = nullptr;
                        }
                    }
                    if (mapTexture) {
                        auto windowSize = window->getSize();
                        auto mapSize = mapTexture->getSize();
                        float mapX = 0.f;
                        float mapY = getInlineMapY(windowSize.y, mapSize.y);

                        // Extract mouse position from event (default to off-screen if not a mouse event)
                        sf::Vector2i mousePos(-1000, -1000); // Default off-screen

                        if (const auto *mouseButtonEvent = event->getIf<sf::Event::MouseButtonPressed>()) {
                            mousePos = mouseButtonEvent->position;
                        } else if (const auto *mouseButtonEvent = event->getIf<sf::Event::MouseButtonReleased>()) {
                            mousePos = mouseButtonEvent->position;
                        } else if (const auto *mouseMoveEvent = event->getIf<sf::Event::MouseMoved>()) {
                            mousePos = mouseMoveEvent->position;
                        } else if (const auto *wheelEvent = event->getIf<sf::Event::MouseWheelScrolled>()) {
                            mousePos = wheelEvent->position;
                        }

                        // Check if mouse is over map area (only for actual mouse events)
                        if (mousePos.x >= mapX && mousePos.x < mapX + mapSize.x && mousePos.y >= mapY &&
                            mousePos.y < mapY + mapSize.y) {
                            if (const auto *mouseButtonEvent = event->getIf<sf::Event::MouseButtonPressed>()) {
                                if (mouseButtonEvent->button == sf::Mouse::Button::Left) {
                                    if (selectImageFromInlineMapDotsNear(sf::Vector2f(
                                            static_cast<float>(mousePos.x), static_cast<float>(mousePos.y)))) {
                                        eventHandledByMap = true;
                                        continue;
                                    }
                                }
                            }

                            // Forward to map viewer with offset
                            mapViewer->handleEvent(*event,
                                                   sf::Vector2i(static_cast<int>(mapX), static_cast<int>(mapY)));
                            eventHandledByMap = true;
                        }
                    }
                }

                if (eventHandledByMap) {
                    // Event was handled by map, don't process further
                    continue;
                }

                if (event->is<sf::Event::Closed>()) {
                    window->close();
                } else if (const auto *keyEvent = event->getIf<sf::Event::KeyPressed>()) {
                    if (contextMenuVisible && keyEvent->code == sf::Keyboard::Key::Escape) {
                        closeContextMenu();
                        continue;
                    }
                    handleKeyPress(*keyEvent);
                } else if (const auto *textEvent = event->getIf<sf::Event::TextEntered>()) {
                    if (searchUiOpen && searchInputFocused) {
                        std::uint32_t uni = textEvent->unicode;
                        if (uni >= 32 && uni != 127) {
                            std::string utf8 = encodeUtf8(uni);
                            searchPrefix.insert(searchPrefixCursor, utf8);
                            searchPrefixCursor += utf8.size();
                            refreshSearchSuggestions();
                        }
                        continue;
                    }
                } else if (const auto *mouseEvent = event->getIf<sf::Event::MouseButtonPressed>()) {
                    sf::Vector2f clickPos(mouseEvent->position.x, mouseEvent->position.y);

                    if (mouseEvent->button == sf::Mouse::Button::Right) {
                        openContextMenu(clickPos);
                        continue;
                    }

                    if (contextMenuVisible) {
                        if (mouseEvent->button == sf::Mouse::Button::Left) {
                            handleContextMenuClick(clickPos);
                        } else {
                            closeContextMenu();
                        }
                        continue;
                    }

                    if (mouseEvent->button == sf::Mouse::Button::Left) {
                        if (hamburgerButtonRect.contains(clickPos)) {
                            openContextMenu(clickPos);
                            continue;
                        }

                        if (!searchUiOpen && searchOpenButtonRect.contains(clickPos)) {
                            openSearchUi();
                            continue;
                        }

                        if (searchUiOpen) {
                            if (searchDismissButtonRect.contains(clickPos)) {
                                closeSearchUiAndRestore();
                                continue;
                            }

                            if (searchSubmitButtonRect.contains(clickPos)) {
                                log_stdout("DEBUG search: submit button clicked, tokens=", searchTokens.size(),
                                           " prefix='", searchPrefix, "'");
                                submitSearchQuery();
                                continue;
                            }

                            bool searchHandled = false;
                            for (const auto &[tokenIndex, rect] : searchTokenDismissRects) {
                                if (rect.contains(clickPos) && tokenIndex < searchTokens.size()) {
                                    searchTokens.erase(searchTokens.begin() + static_cast<std::ptrdiff_t>(tokenIndex));
                                    refreshSearchSuggestions();
                                    searchHandled = true;
                                    break;
                                }
                            }
                            if (searchHandled) {
                                continue;
                            }

                            for (const auto &[suggestionIndex, rect] : searchSuggestionRects) {
                                if (rect.contains(clickPos) && suggestionIndex < searchSuggestions.size()) {
                                    addSearchToken(searchSuggestions[suggestionIndex].token);
                                    searchHandled = true;
                                    break;
                                }
                            }
                            if (searchHandled) {
                                continue;
                            }

                            if (searchInputHitRect.contains(clickPos)) {
                                searchInputFocused = true;
                                searchCursorVisible = true;
                                lastSearchCursorBlinkTime = std::chrono::steady_clock::now();
                                continue;
                            }
                        }
                    }

                    if (mouseEvent->button == sf::Mouse::Button::Left) {
                        if (thumbnailMode) {
                            sf::FloatRect thumbRect = getThumbnailScrollbarThumbRect();
                            sf::FloatRect trackRect = getThumbnailScrollbarTrackRect();
                            if (trackRect.contains(clickPos) && trackRect.size.x > 0.0f && trackRect.size.y > 0.0f) {
                                if (thumbRect.contains(clickPos)) {
                                    thumbnailScrollbarDragging = true;
                                    thumbnailScrollbarDragOffset = clickPos.y - thumbRect.position.y;
                                } else {
                                    thumbnailScrollbarDragOffset = thumbRect.size.y * 0.5f;
                                    setThumbnailScrollFromCursorY(clickPos.y);
                                    thumbnailScrollbarDragging = true;
                                }
                                continue;
                            }

                            if (folderMode) {
                                bool folderHit = false;
                                for (const auto &[entryIdx, rect] : folderModeClickAreas) {
                                    if (rect.contains(clickPos)) {
                                        folderHit = true;
                                        folderModeFocusIndex = entryIdx;
                                        auto now = std::chrono::steady_clock::now();
                                        auto elapsed = now - lastFolderModeClickTime;
                                        refreshMapForFolderFocusSelection();
                                        if (lastFolderModeClickedIndex == entryIdx &&
                                            elapsed <= std::chrono::milliseconds(350)) {
                                            openFolderModeEntry(entryIdx);
                                        }
                                        lastFolderModeClickedIndex = entryIdx;
                                        lastFolderModeClickTime = now;
                                        break;
                                    }
                                }
                                if (!folderHit) {
                                    closeContextMenu();
                                }
                                continue;
                            }

                            bool thumbHit = false;
                            for (const auto &[thumbIdx, rect] : thumbnailClickAreas) {
                                if (rect.contains(clickPos)) {
                                    thumbHit = true;
                                    currentIndex = thumbIdx;
                                    onThumbnailSelectionChanged();

                                    auto now = std::chrono::steady_clock::now();
                                    auto elapsed = now - lastThumbnailClickTime;
                                    if (lastThumbnailClickedIndex == thumbIdx &&
                                        elapsed <= std::chrono::milliseconds(350)) {
                                        thumbnailMode = false;
                                        loadImage(currentIndex);
                                    }
                                    lastThumbnailClickedIndex = thumbIdx;
                                    lastThumbnailClickTime = now;
                                    break;
                                }
                            }

                            if (!thumbHit) {
                                closeContextMenu();
                            }
                            continue;
                        }

                        bool clickHandled = false;

                        // Check if click is on any navigation arrow
                        for (const auto &[arrow, clickArea] : navArrowAreas) {
                            if (clickArea.contains(clickPos)) {
                                switch (arrow) {
                                case NavArrow::Left:
                                    loadImage(getPrevInFolder());
                                    break;
                                case NavArrow::Right:
                                    loadImage(getNextInFolder());
                                    break;
                                case NavArrow::Up:
                                    loadImage(getFirstInPrevFolder());
                                    break;
                                case NavArrow::Down:
                                    loadImage(getFirstInNextFolder());
                                    break;
                                }
                                clickHandled = true;
                                break;
                            }
                        }

                        // Check if click is on any map link (if not already handled)
                        if (!clickHandled) {
                            for (const auto &[mapIdx, clickArea] : mapLinkAreas) {
                                if (clickArea.contains(clickPos)) {
                                    if (!allImagePaths.empty()) {
                                        const auto &imagePath = allImagePaths[currentIndex];
                                        if (hasGpsLatitude(imagePath)) {
                                            double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
                                            double lon = getGpsValueOrZero(imagePath, "GPSLongitude");
                                            if (mapIdx >= 0 && mapIdx < static_cast<int>(maps.size())) {
                                                const Map &m = maps[mapIdx];
                                                std::string url = buildMapURL(m.gui_url_template, lat, lon, m.zoom);
                                                openURL(url);
                                            }
                                        }
                                    }
                                    break; // Only open first clicked map
                                }
                            }
                        }
                    } else if (mouseEvent->button == sf::Mouse::Button::Middle) {
                        // Middle mouse button to toggle embedded map viewer
                        if (mapViewer && mapViewer->isOpen()) {
                            mapViewer->close(); // Close the map viewer
                        } else if (mapViewer && !allImagePaths.empty()) {
                            const auto &imagePath = allImagePaths[currentIndex];
                            if (hasGpsLatitude(imagePath)) {
                                double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
                                double lon = getGpsValueOrZero(imagePath, "GPSLongitude");
                                mapViewer->showMap(lat, lon, defaultZoom);
                                log_stdout("Opening map for ", lat, ", ", lon);
                            } else {
                                log_stdout("No GPS data for current image");
                            }
                        }
                    }
                } else if (const auto *wheelEvent = event->getIf<sf::Event::MouseWheelScrolled>()) {
                    // Mouse wheel for navigation (disabled only when map viewer is open and mouse is over it)
                    // Note: if map is open but mouse is NOT over it, wheel still controls images
                    bool shouldProcessWheel = true;

                    if (experimental && mapViewer && mapViewer->isOpen()) {
                        const sf::Texture *mapTexture = mapViewer->getTexture();
                        if (mapTexture) {
                            auto windowSize = window->getSize();
                            auto mapSize = mapTexture->getSize();
                            float mapX = 0.f;
                            float mapY = getInlineMapY(windowSize.y, mapSize.y);

                            // Check if mouse is over map area - if it is, don't process wheel for images
                            sf::Vector2i mousePos = wheelEvent->position;
                            if (mousePos.x >= mapX && mousePos.x < mapX + mapSize.x && mousePos.y >= mapY &&
                                mousePos.y < mapY + mapSize.y) {
                                shouldProcessWheel = false;
                            }
                        }
                    }

                    if (shouldProcessWheel) {
                        sf::Vector2f wheelPos(static_cast<float>(wheelEvent->position.x),
                                              static_cast<float>(wheelEvent->position.y));
                        bool overThumbnailArea = thumbnailMode && getThumbnailAreaRect().contains(wheelPos);

                        bool ctrlHeld = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LControl) ||
                                        sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RControl);

                        if (thumbnailMode && ctrlHeld && overThumbnailArea) {
                            int deltaColumns = (wheelEvent->delta > 0.0f) ? -1 : (wheelEvent->delta < 0.0f ? 1 : 0);
                            if (deltaColumns != 0) {
                                setThumbnailColumns(thumbnailColumns + deltaColumns);
                            }
                            continue;
                        }

                        if (thumbnailMode && !ctrlHeld && overThumbnailArea) {
                            thumbnailScrollRow += (wheelEvent->delta > 0.0f) ? -1 : (wheelEvent->delta < 0.0f ? 1 : 0);
                            clampThumbnailScroll();
                            continue;
                        }

                        if (ctrlHeld) {
                            if (zoomImageAtCursor(wheelPos, wheelEvent->delta)) {
                                continue;
                            }
                        }

                        bool shiftHeld = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift) ||
                                         sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RShift);

                        if (shiftHeld) {
                            // Shift + Scroll for folder navigation
                            if (wheelEvent->delta > 0) {
                                // Shift + Scroll up = previous folder
                                loadImage(getFirstInPrevFolder());
                            } else if (wheelEvent->delta < 0) {
                                // Shift + Scroll down = next folder
                                loadImage(getFirstInNextFolder());
                            }
                        } else {
                            // Regular scroll for image navigation
                            if (wheelEvent->delta > 0) {
                                // Scroll up = previous image
                                loadImage(getPrevInFolder());
                            } else if (wheelEvent->delta < 0) {
                                // Scroll down = next image
                                loadImage(getNextInFolder());
                            }
                        }
                    }
                } else if (const auto *mouseMoveEvent = event->getIf<sf::Event::MouseMoved>()) {
                    if (thumbnailMode && thumbnailScrollbarDragging) {
                        setThumbnailScrollFromCursorY(static_cast<float>(mouseMoveEvent->position.y));
                        continue;
                    }
                } else if (const auto *mouseReleaseEvent = event->getIf<sf::Event::MouseButtonReleased>()) {
                    if (thumbnailMode && mouseReleaseEvent->button == sf::Mouse::Button::Left) {
                        thumbnailScrollbarDragging = false;
                    }
                } else if (const auto *resizeEvent = event->getIf<sf::Event::Resized>()) {
                    // Reset the view to match the new window size to prevent distortion
                    window->setView(sf::View(sf::FloatRect({0.f, 0.f}, sf::Vector2f(resizeEvent->size))));

                    if (!isFullscreen) {
                        windowedSize = resizeEvent->size;
                        windowedPosition = window->getPosition();
                        hasStoredWindowState = true;
                    }

                    // Resize map proportionally to new window size and reposition image
                    if (mapViewer) {
                        int newMapWidth = parseSizeValue(mapWindowWidth, resizeEvent->size.x);
                        int newMapHeight = parseSizeValue(mapWindowHeight, resizeEvent->size.y);
                        mapViewer->onWindowResize(newMapWidth, newMapHeight);
                    }

                    // Recalculate sprite position for new window size without reloading from disk
                    if (!allImagePaths.empty() && sprite && texture) {
                        updateSpritePositioning();
                    }
                    if (thumbnailMode) {
                        ensureThumbnailSelectionVisible();
                    }
                }
            }

            // Process any queued key presses after pre-caching completes
            processPendingKeyPresses();

            // Update cursor based on hover over any map link or navigation arrow
            auto mousePos = sf::Vector2f(sf::Mouse::getPosition(*window));
            bool mouseOverLink = false;
            if (hamburgerButtonRect.contains(mousePos) || searchOpenButtonRect.contains(mousePos) ||
                searchSubmitButtonRect.contains(mousePos) || searchDismissButtonRect.contains(mousePos)) {
                mouseOverLink = true;
            }
            if (!mouseOverLink) {
                for (const auto &[tokenIndex, rect] : searchTokenDismissRects) {
                    if (rect.contains(mousePos)) {
                        mouseOverLink = true;
                        break;
                    }
                }
            }
            if (!mouseOverLink) {
                for (const auto &[suggestionIndex, rect] : searchSuggestionRects) {
                    if (rect.contains(mousePos)) {
                        mouseOverLink = true;
                        break;
                    }
                }
            }
            for (const auto &[mapIdx, clickArea] : mapLinkAreas) {
                if (clickArea.contains(mousePos)) {
                    mouseOverLink = true;
                    break;
                }
            }
            if (!mouseOverLink && thumbnailMode) {
                if (folderMode) {
                    for (const auto &[entryIdx, clickArea] : folderModeClickAreas) {
                        if (clickArea.contains(mousePos)) {
                            mouseOverLink = true;
                            break;
                        }
                    }
                } else {
                    for (const auto &[thumbIdx, clickArea] : thumbnailClickAreas) {
                        if (clickArea.contains(mousePos)) {
                            mouseOverLink = true;
                            break;
                        }
                    }
                }

                if (!mouseOverLink) {
                    sf::FloatRect scrollTrack = getThumbnailScrollbarTrackRect();
                    if (scrollTrack.size.x > 0.0f && scrollTrack.size.y > 0.0f && scrollTrack.contains(mousePos)) {
                        mouseOverLink = true;
                    }
                }
            }
            // Also check navigation arrows
            if (!mouseOverLink) {
                for (const auto &[arrow, clickArea] : navArrowAreas) {
                    if (clickArea.contains(mousePos)) {
                        mouseOverLink = true;
                        break;
                    }
                }
            }

            if (!mouseOverLink && mapViewer && mapViewer->isOpen()) {
                mouseOverLink = isMouseOverInlineMapDot(mousePos);
            }

            if (quietMode) {
                mapLinkAreas.clear();
                navArrowAreas.clear();
                hamburgerButtonRect = sf::FloatRect();
                searchOpenButtonRect = sf::FloatRect();
                searchSubmitButtonRect = sf::FloatRect();
                searchDismissButtonRect = sf::FloatRect();
                searchTokenDismissRects.clear();
                searchSuggestionRects.clear();
                mouseOverLink = false;
            }

            // Only manage cursor when needed to allow OS to show resize cursors in windowed mode
            if (mouseOverLink && !isHandCursorActive) {
                // Switch to hand cursor
                auto handCursor = sf::Cursor::createFromSystem(sf::Cursor::Type::Hand);
                if (handCursor.has_value()) {
                    window->setMouseCursor(handCursor.value());
                    isHandCursorActive = true;
                }
            } else if (!mouseOverLink && isHandCursorActive) {
                // Reset to arrow cursor only when leaving a link
                auto arrowCursor = sf::Cursor::createFromSystem(sf::Cursor::Type::Arrow);
                if (arrowCursor.has_value()) {
                    window->setMouseCursor(arrowCursor.value());
                    isHandCursorActive = false;
                }
            }
            // In windowed mode, when not over a link, let OS handle cursor (including resize cursors)

            // Render
            window->clear(sf::Color::Black);
            if (!thumbnailMode && sprite) {
                window->draw(*sprite);
            }
            if (thumbnailMode) {
                drawThumbnailGrid();
            }

            // Draw inline map in experimental mode; if map isn't open and image has no GPS,
            // still draw the explicit no-map placeholder.
            if (experimental && mapViewer) {
                bool drewMapSurface = false;
                if (mapViewer->isOpen()) {
                    const sf::Texture *mapTexture = mapViewer->getTexture();
                    if (mapTexture) {
                        sf::Sprite mapSprite(*mapTexture);
                        auto windowSize = window->getSize();
                        auto mapSize = mapTexture->getSize();
                        // Position on left side, anchored to bottom
                        float mapX = 0.f;
                        float mapY = getInlineMapY(windowSize.y, mapSize.y);
                        mapSprite.setPosition(sf::Vector2f(mapX, mapY));
                        window->draw(mapSprite);
                        drewMapSurface = true;

                        if (!currentMapSubjectHasGps()) {
                            drawNoGpsOverlayAt(mapX, mapY, static_cast<float>(mapSize.x),
                                               static_cast<float>(mapSize.y));
                        }
                    }
                }

                if (!drewMapSurface && !currentMapSubjectHasGps()) {
                    drawNoGpsInlineMapPlaceholder();
                }
            }

            mapTileDownloadMessageActive =
                mapViewer && mapViewer->isOpen() && currentMapSubjectHasGps() && mapViewer->isLoadingTiles();

            if (!quietMode) {
                drawTopLeftInfo();
                drawFilterInfo();
                drawMapInfo();

                // Draw navigation message row (including transient status text)
                if (!navigationMessage.empty() || thumbnailCollectionMessageActive ||
                    metadataCollectionMessageActive || mapTileDownloadMessageActive) {
                    drawNavigationMessage();
                }
                // Otherwise draw "Jumped to oldest" message if applicable
                else if (jumpedToOldest) {
                    drawJumpedMessage();
                }
            }

            // Draw help overlay last (on top of everything)
            if (showHelp) {
                drawHelp(window, helpLines, uiFont, getCalculatedFontSize(), config);
            }

            drawContextMenu();

            window->display();

            processThumbnailLoading();

            // Update map viewer if open
            if (mapViewer && mapViewer->isOpen()) {
                mapViewer->update();

                // Handle close request from map viewer
                if (mapViewer->isCloseRequested()) {
                    mapViewer->close(); // Close the map viewer
                } else {
                    // Handle navigation requests from map viewer (prev/next image and folders)
                    int navRequest = mapViewer->getNavigationRequest();
                    if (navRequest != 0) {
                        if (navRequest > 0) {
                            // Request to go to next image
                            loadImage(getNextInFolder());
                        } else {
                            // Request to go to previous image
                            loadImage(getPrevInFolder());
                        }
                    }

                    // Handle folder navigation requests
                    int folderNavRequest = mapViewer->getFolderNavigationRequest();
                    if (folderNavRequest != 0) {
                        if (folderNavRequest > 0) {
                            // Request to go to next folder
                            loadImage(getFirstInNextFolder());
                        } else {
                            // Request to go to previous folder
                            loadImage(getFirstInPrevFolder());
                        }
                    }
                }
            }
        }

        // Wait for pre-cache thread to finish before exiting
        if (preCacheThread.joinable()) {
            preCacheThread.join();
        }
    }

    void drawJumpedMessage() {
        const std::string message = "Jumped to 1st image in folder";

        if (uiFontLoaded) {
            sf::Text text(uiFont, message, getCalculatedFontSize());
            text.setFillColor(sf::Color::Yellow);

            sf::FloatRect bounds = text.getLocalBounds();
            const float paddingX = 12.0f;
            const float paddingY = 8.0f;
            sf::Vector2f boxSize(bounds.size.x + paddingX * 2.0f, bounds.size.y + paddingY * 2.0f);
            sf::Vector2f boxPos(15.0f, getBottomLeftOverlayY(boxSize.y));

            sf::RectangleShape background(boxSize);
            background.setPosition(boxPos);
            background.setFillColor(sf::Color::Black);
            window->draw(background);

            text.setPosition({boxPos.x + paddingX - bounds.position.x, boxPos.y + paddingY - bounds.position.y});
            window->draw(text);
            return;
        }
    }

    void drawFilterInfo() {
        if (activeFilterIndex < 0 || activeFilterIndex >= static_cast<int>(filters.size())) {
            return;
        }

        const Filter &f = filters[activeFilterIndex];
        std::vector<std::string> lines;

        // Count filtered images
        size_t filteredCount = 0;
        for (size_t i = 0; i < allImagePaths.size(); i++) {
            if (passesActiveFilter(allImagePaths[i])) {
                filteredCount++;
            }
        }

        lines.push_back("Filter key: " + f.key + "  " + std::to_string(filteredCount) + "/" +
                        std::to_string(allImagePaths.size()));
        lines.push_back(f.expression);

        if (uiFontLoaded) {
            // Filters are now always active when an activeFilterIndex is set
            bool strike = false;
            unsigned int fontSize = getCalculatedFontSize();
            float lineSpacing = static_cast<float>(fontSize + 4);
            float maxWidth = 0.0f;
            std::vector<sf::Text> texts;
            for (const auto &lineStr : lines) {
                sf::Text text(uiFont, lineStr, fontSize);
                text.setFillColor(sf::Color::Cyan);
                maxWidth = std::max(maxWidth, text.getLocalBounds().size.x);
                texts.push_back(text);
            }

            float padding = 12.0f;
            sf::Vector2f boxSize(maxWidth + padding * 2, lines.size() * lineSpacing + padding * 2);

            // Place filter box directly above active nav/jump message when present.
            float boxY = getBottomLeftOverlayY(boxSize.y);
            const float stackGap = 8.0f;
            const float navPaddingY = 8.0f;

            auto computeBottomOverlayTop = [&](const std::string &message) -> float {
                sf::Text navText(uiFont, message, getCalculatedFontSize());
                sf::FloatRect navBounds = navText.getLocalBounds();
                sf::Vector2f navSize(navBounds.size.x + 12.0f * 2.0f, navBounds.size.y + navPaddingY * 2.0f);
                return getBottomLeftOverlayY(navSize.y);
            };

            std::string rowMessage;
            if (thumbnailCollectionMessageActive) {
                rowMessage = folderMode ? "Collecting folder thumbs..." : "Collecting thumbs...";
            } else if (metadataCollectionMessageActive) {
                rowMessage = "Collecting image metadata...";
            } else if (mapTileDownloadMessageActive) {
                rowMessage = "Downloading map tiles...";
            } else {
                rowMessage = navigationMessage;
            }

            if (!rowMessage.empty()) {
                float navTop = computeBottomOverlayTop(rowMessage);
                boxY = navTop - boxSize.y - stackGap;
            } else if (jumpedToOldest) {
                float navTop = computeBottomOverlayTop("Jumped to 1st image in folder");
                boxY = navTop - boxSize.y - stackGap;
            }

            boxY = std::max(0.0f, boxY);
            sf::Vector2f boxPos(15.0f, boxY);

            sf::RectangleShape bg(boxSize);
            bg.setPosition(boxPos);
            bg.setFillColor(sf::Color::Black);
            window->draw(bg);

            for (size_t i = 0; i < texts.size(); i++) {
                texts[i].setPosition({boxPos.x + padding, boxPos.y + padding + i * lineSpacing});
                window->draw(texts[i]);
                if (strike) {
                    auto bounds = texts[i].getLocalBounds();
                    float lineY = boxPos.y + padding + i * lineSpacing + bounds.size.y * 0.5f;
                    sf::RectangleShape strikeLine(sf::Vector2f(bounds.size.x, 2.0f));
                    strikeLine.setFillColor(sf::Color::Cyan);
                    strikeLine.setPosition({boxPos.x + padding, lineY});
                    window->draw(strikeLine);
                }
            }
        }
    }

    void drawNavigationMessage() {
        std::string rowMessage;
        if (thumbnailCollectionMessageActive) {
            rowMessage = folderMode ? "Collecting folder thumbs..." : "Collecting thumbs...";
        } else if (metadataCollectionMessageActive) {
            rowMessage = "Collecting image metadata...";
        } else if (mapTileDownloadMessageActive) {
            rowMessage = "Downloading map tiles...";
        } else {
            rowMessage = navigationMessage;
        }

        if (rowMessage.empty())
            return;

        if (uiFontLoaded) {
            sf::Text text(uiFont, rowMessage, getCalculatedFontSize());

            bool isZoomBoundary = rowMessage == "Reached maximum zoom" || rowMessage == "Reached minimum zoom";
            bool isEscHint = rowMessage == "To exit, use Ctrl-F4";
            // Keep folder boundary warnings red; keep zoom limit messages yellow.
            text.setFillColor(((rowMessage.find("Reached") == 0 && !isZoomBoundary) || isEscHint) ? sf::Color::Red
                                                                                                  : sf::Color::Yellow);

            sf::FloatRect bounds = text.getLocalBounds();
            const float paddingX = 12.0f;
            const float paddingY = 8.0f;
            sf::Vector2f boxSize(bounds.size.x + paddingX * 2.0f, bounds.size.y + paddingY * 2.0f);
            sf::Vector2f boxPos(15.0f, getBottomLeftOverlayY(boxSize.y));

            sf::RectangleShape background(boxSize);
            background.setPosition(boxPos);
            background.setFillColor(sf::Color::Black);
            window->draw(background);

            text.setPosition({boxPos.x + paddingX - bounds.position.x, boxPos.y + paddingY - bounds.position.y});
            window->draw(text);
            return;
        }
    }

    void presentFrameNow() {
        if (!window) {
            return;
        }

        window->clear(sf::Color::Black);
        if (!thumbnailMode && sprite) {
            window->draw(*sprite);
        }
        if (thumbnailMode) {
            drawThumbnailGrid();
        }

        if (experimental && mapViewer) {
            bool drewMapSurface = false;
            if (mapViewer->isOpen()) {
                const sf::Texture *mapTexture = mapViewer->getTexture();
                if (mapTexture) {
                    sf::Sprite mapSprite(*mapTexture);
                    auto windowSize = window->getSize();
                    auto mapSize = mapTexture->getSize();
                    float mapX = 0.f;
                    float mapY = getInlineMapY(windowSize.y, mapSize.y);
                    mapSprite.setPosition(sf::Vector2f(mapX, mapY));
                    window->draw(mapSprite);
                    drewMapSurface = true;

                    if (!currentMapSubjectHasGps()) {
                        drawNoGpsOverlayAt(mapX, mapY, static_cast<float>(mapSize.x), static_cast<float>(mapSize.y));
                    }
                }
            }

            if (!drewMapSurface && !currentMapSubjectHasGps()) {
                drawNoGpsInlineMapPlaceholder();
            }
        }

        mapTileDownloadMessageActive =
            mapViewer && mapViewer->isOpen() && currentMapSubjectHasGps() && mapViewer->isLoadingTiles();

        if (!quietMode) {
            drawTopLeftInfo();
            drawFilterInfo();
            drawMapInfo();
            if (!navigationMessage.empty() || thumbnailCollectionMessageActive || metadataCollectionMessageActive ||
                mapTileDownloadMessageActive) {
                drawNavigationMessage();
            } else if (jumpedToOldest) {
                drawJumpedMessage();
            }
        }

        if (showHelp) {
            drawHelp(window, helpLines, uiFont, getCalculatedFontSize(), config);
        }
        drawContextMenu();
        window->display();
    }

    static std::string formatDate(const std::string &dateTime) {
        if (dateTime.size() < 10)
            return "";
        std::string datePart = dateTime.substr(0, 10);
        std::replace(datePart.begin(), datePart.end(), ':', '-');

        // Parse date and add abbreviated day name
        try {
            int year = std::stoi(dateTime.substr(0, 4));
            int month = std::stoi(dateTime.substr(5, 2));
            int day = std::stoi(dateTime.substr(8, 2));

            // Use system calendar to get day of week
            struct tm timeinfo = {};
            timeinfo.tm_year = year - 1900;
            timeinfo.tm_mon = month - 1;
            timeinfo.tm_mday = day;
            mktime(&timeinfo);

            char dayBuf[4];
            strftime(dayBuf, sizeof(dayBuf), "%a", &timeinfo);

            datePart += " ";
            datePart += dayBuf;
        } catch (...) {
            // If parsing fails, just return the date part
        }

        return datePart;
    }

    static std::string formatTime(const std::string &dateTime) {
        if (dateTime.size() < 19)
            return "";
        return dateTime.substr(11, 8);
    }

    void drawMapInfo() {
        if (!experimental || !mapViewer || !mapViewer->isOpen())
            return;

        if (!currentMapSubjectHasGps()) {
            return;
        }

        auto windowSize = window->getSize();
        const sf::Texture *mapTexture = mapViewer->getTexture();

        // Draw "Loading map" text below the map if tiles are loading
        if (mapViewer->isLoadingTiles()) {
            // Get current map position and zoom
            double centerLat = mapViewer->getCenterLat();
            double centerLon = mapViewer->getCenterLon();
            int zoom = mapViewer->getCurrentZoom();

            // Format coordinates to 5 decimal places
            std::ostringstream oss;
            oss << "Loading map for " << std::fixed << std::setprecision(5) << centerLat << ", " << centerLon << " z"
                << zoom << "...";

            sf::Text loadingText(uiFont, oss.str());
            loadingText.setFillColor(sf::Color::White);
            loadingText.setCharacterSize(getCalculatedFontSize());

            if (mapTexture) {
                auto mapSize = mapTexture->getSize();
                float mapX = 0.f;
                float mapY = getInlineMapY(windowSize.y, mapSize.y);

                sf::FloatRect loadingBounds = loadingText.getLocalBounds();
                loadingText.setPosition(sf::Vector2f(mapX + 5.f, mapY + mapSize.y + 5.f));
                window->draw(loadingText);
            }
        }
    }

    float drawSearchTopRow(float startX, float startY, unsigned int fontSize, float lineSpacing) {
        hamburgerButtonRect = sf::FloatRect();
        searchOpenButtonRect = sf::FloatRect();
        searchInputHitRect = sf::FloatRect();
        searchSubmitButtonRect = sf::FloatRect();
        searchDismissButtonRect = sf::FloatRect();
        searchTokenDismissRects.clear();
        searchSuggestionRects.clear();

        if (!uiFontLoaded) {
            return startY;
        }

        const float buttonPadX = 7.0f;
        const float buttonPadY = 4.0f;
        const float iconSize = std::max(12.0f, lineSpacing - 8.0f);
        float menuW = iconSize + buttonPadX * 2.0f;
        float menuH = lineSpacing;
        hamburgerButtonRect = sf::FloatRect(sf::Vector2f(startX, startY), sf::Vector2f(menuW, menuH));

        sf::RectangleShape menuBg(hamburgerButtonRect.size);
        menuBg.setPosition(hamburgerButtonRect.position);
        menuBg.setFillColor(sf::Color(35, 35, 35, 220));
        menuBg.setOutlineThickness(1.0f);
        menuBg.setOutlineColor(sf::Color(90, 90, 90));
        window->draw(menuBg);

        auto drawHamburgerIcon = [&](const sf::FloatRect &rect) {
            const float marginX = std::max(3.0f, rect.size.x * 0.25f);
            const float yCenter = rect.position.y + rect.size.y * 0.5f;
            const float gapY = std::max(2.0f, rect.size.y * 0.18f);
            const float lineH = std::max(1.5f, rect.size.y * 0.08f);
            for (int i = -1; i <= 1; i++) {
                sf::RectangleShape line(sf::Vector2f(rect.size.x - marginX * 2.0f, lineH));
                line.setFillColor(sf::Color::White);
                line.setPosition({rect.position.x + marginX, yCenter + static_cast<float>(i) * gapY - lineH * 0.5f});
                window->draw(line);
            }
        };

        auto drawSearchIcon = [&](const sf::FloatRect &rect) {
            const float radius = std::max(3.0f, std::min(rect.size.x, rect.size.y) * 0.22f);
            const sf::Vector2f center(rect.position.x + rect.size.x * 0.45f, rect.position.y + rect.size.y * 0.45f);

            sf::CircleShape ring(radius);
            ring.setOrigin({radius, radius});
            ring.setPosition(center);
            ring.setFillColor(sf::Color::Transparent);
            ring.setOutlineThickness(1.6f);
            ring.setOutlineColor(sf::Color::White);
            window->draw(ring);

            sf::RectangleShape handle(sf::Vector2f(std::max(5.0f, radius * 1.1f), 1.8f));
            handle.setFillColor(sf::Color::White);
            handle.setOrigin({0.0f, 0.9f});
            handle.setPosition({center.x + radius * 0.6f, center.y + radius * 0.6f});
            handle.setRotation(sf::degrees(45.0f));
            window->draw(handle);
        };

        drawHamburgerIcon(hamburgerButtonRect);

        const float gap = 8.0f;
        const float searchX = hamburgerButtonRect.position.x + hamburgerButtonRect.size.x + gap;

        if (!searchUiOpen) {
            float openW = iconSize + buttonPadX * 2.0f;
            float openH = lineSpacing;
            searchOpenButtonRect = sf::FloatRect(sf::Vector2f(searchX, startY), sf::Vector2f(openW, openH));

            sf::RectangleShape searchBg(searchOpenButtonRect.size);
            searchBg.setPosition(searchOpenButtonRect.position);
            searchBg.setFillColor(sf::Color(35, 35, 35, 220));
            searchBg.setOutlineThickness(1.0f);
            searchBg.setOutlineColor(sf::Color(90, 90, 90));
            window->draw(searchBg);
            drawSearchIcon(searchOpenButtonRect);
            return startY + lineSpacing;
        }

        const float rightMargin = 20.0f;
        float infoRight = startX + std::max(280.0f, topLeftInfoBoxWidth);
        float desiredSearchWidth = std::max(220.0f, infoRight - searchX);
        float inputW = std::min(desiredSearchWidth, static_cast<float>(window->getSize().x) - rightMargin - searchX);
        const float rowGap = 4.0f;
        float rowH = std::max(16.0f, lineSpacing - 6.0f);

        auto measureRowsNeeded = [&](float contentLeft, float availableRight) {
            size_t rows = 1;
            float x = contentLeft;

            auto wrapFor = [&](float w) {
                if (x + w > availableRight && x > contentLeft) {
                    rows++;
                    x = contentLeft;
                }
            };

            for (size_t i = 0; i < searchTokens.size(); i++) {
                const std::string tokenLabel = std::string("x ") + repairMojibakeIfNeeded(searchTokens[i]);
                sf::Text tokenText(uiFont, sfStringFromUtf8(tokenLabel), fontSize);
                sf::FloatRect tb = tokenText.getLocalBounds();
                float chipW = tb.size.x + 12.0f;
                wrapFor(chipW);
                x += chipW + 6.0f;
            }

            size_t off = 0;
            while (off < searchPrefix.size()) {
                size_t next = nextUtf8Offset(searchPrefix, off);
                std::string cp = searchPrefix.substr(off, next - off);
                sf::Text glyph(uiFont, sfStringFromUtf8(cp), fontSize);
                sf::FloatRect gb = glyph.getLocalBounds();
                float glyphW = gb.size.x;
                wrapFor(glyphW);
                x += glyphW;
                off = next;
            }

            return std::clamp<size_t>(rows, 1, 8);
        };

        float estimatedContentLeft = searchX + (iconSize + buttonPadX * 2.0f) + 8.0f;
        float estimatedAvailableRight = searchX + inputW - (fontSize + buttonPadX * 2.0f) - 10.0f;
        size_t inputRows = measureRowsNeeded(estimatedContentLeft, estimatedAvailableRight);
        float inputH = 8.0f + static_cast<float>(inputRows) * rowH + static_cast<float>(inputRows - 1) * rowGap;
        sf::FloatRect inputRect(sf::Vector2f(searchX, startY), sf::Vector2f(inputW, inputH));
        searchInputHitRect = inputRect;

        sf::RectangleShape inputBg(inputRect.size);
        inputBg.setPosition(inputRect.position);
        inputBg.setFillColor(sf::Color(25, 25, 25));
        inputBg.setOutlineThickness(1.0f);
        inputBg.setOutlineColor(sf::Color::Cyan);
        window->draw(inputBg);

        float submitW = iconSize + buttonPadX * 2.0f;
        float buttonH = std::max(lineSpacing, rowH + 4.0f);
        searchSubmitButtonRect = sf::FloatRect(sf::Vector2f(inputRect.position.x + 2.0f, inputRect.position.y + 1.0f),
                                               sf::Vector2f(submitW, std::max(0.0f, buttonH - 2.0f)));
        drawSearchIcon(searchSubmitButtonRect);

        sf::Text dismissText(uiFont, "X", fontSize);
        dismissText.setFillColor(sf::Color::White);
        sf::FloatRect dismissBounds = dismissText.getLocalBounds();
        float dismissW = dismissBounds.size.x + buttonPadX * 2.0f;
        searchDismissButtonRect = sf::FloatRect(
            sf::Vector2f(inputRect.position.x + inputRect.size.x - dismissW - 2.0f, inputRect.position.y + 1.0f),
            sf::Vector2f(dismissW, std::max(0.0f, buttonH - 2.0f)));
        dismissText.setPosition({searchDismissButtonRect.position.x + buttonPadX,
                                 searchDismissButtonRect.position.y +
                                     (searchDismissButtonRect.size.y - dismissBounds.size.y) * 0.5f -
                                     dismissBounds.position.y});
        window->draw(dismissText);

        float contentLeft = searchSubmitButtonRect.position.x + searchSubmitButtonRect.size.x + 6.0f;
        float availableRight = searchDismissButtonRect.position.x - 6.0f;
        float rowStartY = inputRect.position.y + 4.0f;
        float rowEndY = inputRect.position.y + inputRect.size.y - rowH;

        auto wrapIfNeeded = [&](float &x, float &y, float neededW) {
            if (x + neededW <= availableRight) {
                return;
            }
            if (y + rowH + rowGap > rowEndY) {
                return;
            }
            x = contentLeft;
            y += rowH + rowGap;
        };

        float cursorX = contentLeft;
        float cursorY = rowStartY;

        for (size_t i = 0; i < searchTokens.size(); i++) {
            const std::string tokenLabel = std::string("x ") + repairMojibakeIfNeeded(searchTokens[i]);
            sf::Text tokenText(uiFont, sfStringFromUtf8(tokenLabel), fontSize);
            tokenText.setFillColor(sf::Color::Black);
            sf::FloatRect tb = tokenText.getLocalBounds();
            float chipW = tb.size.x + 12.0f;
            float chipH = rowH;

            wrapIfNeeded(cursorX, cursorY, chipW);
            if (cursorX + chipW > availableRight) {
                break;
            }

            sf::FloatRect chipRect(sf::Vector2f(cursorX, cursorY), sf::Vector2f(chipW, chipH));
            sf::RectangleShape chipBg(chipRect.size);
            chipBg.setPosition(chipRect.position);
            chipBg.setFillColor(sf::Color(160, 220, 220));
            chipBg.setOutlineThickness(1.0f);
            chipBg.setOutlineColor(sf::Color(120, 190, 190));
            window->draw(chipBg);

            tokenText.setPosition({chipRect.position.x + 6.0f,
                                   chipRect.position.y + (chipRect.size.y - tb.size.y) * 0.5f - tb.position.y});
            window->draw(tokenText);

            sf::Text xText(uiFont, "x", fontSize);
            sf::FloatRect xBounds = xText.getLocalBounds();
            sf::FloatRect xRect(sf::Vector2f(chipRect.position.x + 4.0f, chipRect.position.y),
                                sf::Vector2f(xBounds.size.x + 4.0f, chipRect.size.y));
            searchTokenDismissRects.push_back({i, xRect});

            cursorX += chipW + 6.0f;
        }

        size_t cursorByte = std::min(searchPrefixCursor, searchPrefix.size());
        float caretX = cursorX;
        float caretY = cursorY;
        bool caretAnchored = false;
        sf::Text prefixMetric(uiFont, "Ag", fontSize);
        sf::FloatRect prefixMetricBounds = prefixMetric.getLocalBounds();
        const float prefixRowAnchorY =
            rowStartY + (rowH - prefixMetricBounds.size.y) * 0.5f - prefixMetricBounds.position.y;
        size_t off = 0;
        while (off < searchPrefix.size()) {
            size_t next = nextUtf8Offset(searchPrefix, off);
            std::string cp = searchPrefix.substr(off, next - off);

            sf::Text glyph(uiFont, sfStringFromUtf8(cp), fontSize);
            glyph.setFillColor(sf::Color::White);
            sf::FloatRect gb = glyph.getLocalBounds();
            float glyphW = gb.size.x;

            if (off == cursorByte && !caretAnchored) {
                caretX = cursorX;
                caretY = cursorY;
                caretAnchored = true;
            }

            wrapIfNeeded(cursorX, cursorY, glyphW);
            if (cursorX + glyphW > availableRight) {
                break;
            }

            float lineOffset = cursorY - rowStartY;
            glyph.setPosition({cursorX, prefixRowAnchorY + lineOffset});
            window->draw(glyph);
            cursorX += glyphW;
            off = next;
        }

        if (!caretAnchored) {
            caretX = cursorX;
            caretY = cursorY;
        }

        if (searchInputFocused) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastSearchCursorBlinkTime >= std::chrono::milliseconds(500)) {
                searchCursorVisible = !searchCursorVisible;
                lastSearchCursorBlinkTime = now;
            }
        } else {
            searchCursorVisible = false;
        }
        if (searchCursorVisible) {
            sf::RectangleShape caret(sf::Vector2f(1.5f, std::max(8.0f, rowH - 4.0f)));
            caret.setFillColor(sf::Color::White);
            caret.setPosition({std::min(availableRight, caretX + 1.0f), caretY + 2.0f});
            window->draw(caret);
        }

        float suggestionsY = inputRect.position.y + inputRect.size.y + 4.0f;
        float contentStartY = startY + lineSpacing;
        if (!searchSuggestions.empty()) {
            float maxBottom = static_cast<float>(window->getSize().y) - 8.0f;
            for (size_t i = 0; i < searchSuggestions.size(); i++) {
                const SearchSuggestion &s = searchSuggestions[i];
                std::ostringstream oss;
                oss << s.display << " (" << s.imageCount << ")";

                sf::Text suggestionText(uiFont, sfStringFromUtf8(oss.str()), fontSize);
                suggestionText.setFillColor(i == static_cast<size_t>(std::max(0, highlightedSearchSuggestion))
                                                ? sf::Color::Black
                                                : sf::Color::White);
                sf::FloatRect sb = suggestionText.getLocalBounds();
                float itemH = lineSpacing;
                sf::FloatRect itemRect(
                    sf::Vector2f(inputRect.position.x, suggestionsY + static_cast<float>(i) * itemH),
                    sf::Vector2f(inputRect.size.x, itemH));
                if (itemRect.position.y + itemRect.size.y > maxBottom) {
                    break;
                }

                sf::RectangleShape itemBg(itemRect.size);
                itemBg.setPosition(itemRect.position);
                itemBg.setFillColor(i == static_cast<size_t>(std::max(0, highlightedSearchSuggestion))
                                        ? sf::Color(170, 230, 230)
                                        : sf::Color(20, 20, 20));
                itemBg.setOutlineThickness(1.0f);
                itemBg.setOutlineColor(sf::Color(60, 60, 60));
                window->draw(itemBg);

                suggestionText.setPosition(
                    {itemRect.position.x + 8.0f,
                     itemRect.position.y + (itemRect.size.y - sb.size.y) * 0.5f - sb.position.y});
                window->draw(suggestionText);

                searchSuggestionRects.push_back({i, itemRect});
                float minX = std::min(searchInputHitRect.position.x, itemRect.position.x);
                float minY = std::min(searchInputHitRect.position.y, itemRect.position.y);
                float maxX = std::max(searchInputHitRect.position.x + searchInputHitRect.size.x,
                                      itemRect.position.x + itemRect.size.x);
                float maxY = std::max(searchInputHitRect.position.y + searchInputHitRect.size.y,
                                      itemRect.position.y + itemRect.size.y);
                searchInputHitRect = sf::FloatRect(sf::Vector2f(minX, minY), sf::Vector2f(maxX - minX, maxY - minY));
            }
        } else if (searchZeroMatchesHint) {
            sf::Text zeroText(uiFont, "(0)", fontSize);
            zeroText.setFillColor(sf::Color::White);
            sf::FloatRect zb = zeroText.getLocalBounds();
            float zeroX = searchDismissButtonRect.position.x - zb.size.x - 10.0f;
            float zeroY = rowStartY + (rowH - zb.size.y) * 0.5f - zb.position.y;
            zeroText.setPosition({zeroX, zeroY});
            window->draw(zeroText);

            sf::FloatRect zeroBounds = zeroText.getLocalBounds();
            sf::FloatRect zeroRect(sf::Vector2f(zeroText.getPosition().x + zeroBounds.position.x,
                                                zeroText.getPosition().y + zeroBounds.position.y),
                                   sf::Vector2f(zeroBounds.size.x, zeroBounds.size.y));
            float minX = std::min(searchInputHitRect.position.x, zeroRect.position.x);
            float minY = std::min(searchInputHitRect.position.y, zeroRect.position.y);
            float maxX = std::max(searchInputHitRect.position.x + searchInputHitRect.size.x,
                                  zeroRect.position.x + zeroRect.size.x);
            float maxY = std::max(searchInputHitRect.position.y + searchInputHitRect.size.y,
                                  zeroRect.position.y + zeroRect.size.y);
            searchInputHitRect = sf::FloatRect(sf::Vector2f(minX, minY), sf::Vector2f(maxX - minX, maxY - minY));
        }

        if (searchUiOpen) {
            contentStartY = inputRect.position.y + inputRect.size.y;
        }

        return contentStartY;
    }

    void drawTopLeftInfo() {
        if (folderMode) {
            mapLinkAreas.clear();
            navArrowAreas.clear();

            if (!uiFontLoaded) {
                return;
            }

            const float startX = 20.0f;
            const float startY = 20.0f;
            unsigned int fontSize = getCalculatedFontSize();
            const float lineSpacing = static_cast<float>(fontSize + 4);

            std::string folderDisplay = watchedFoldersMode ? "Watched folders" : currentFolder.string();
            if (!watchedFoldersMode && !folderDisplay.empty() && folderDisplay.back() != '/' &&
                folderDisplay.back() != '\\') {
                folderDisplay.push_back(fs::path::preferred_separator);
            }

            if (uiFontLoaded) {
                sf::Text widthProbe(uiFont, folderDisplay, fontSize);
                sf::FloatRect wb = widthProbe.getLocalBounds();
                float desired = std::max(380.0f, wb.size.x + 60.0f);
                topLeftInfoBoxWidth = std::min(desired, static_cast<float>(window->getSize().x) - 40.0f);
            }

            float contentStartY = drawSearchTopRow(startX, startY, fontSize, lineSpacing);

            if (searchUiOpen && !searchSuggestions.empty()) {
                return;
            }

            float arrowStartX = startX;
            float arrowY = contentStartY;
            float arrowSize = static_cast<float>(fontSize);
            float arrowSpacing = arrowSize + 12.0f;

            std::vector<std::pair<NavArrow, std::string>> arrows = {
                {NavArrow::Left, "<"},
                {NavArrow::Right, ">"},
                {NavArrow::Up, "^"},
                {NavArrow::Down, "v"},
            };

            for (size_t a = 0; a < arrows.size(); a++) {
                sf::Text arrowText(uiFont, arrows[a].second, static_cast<unsigned int>(arrowSize));
                arrowText.setFillColor(sf::Color::White);
                float arrowX = arrowStartX + static_cast<float>(a) * arrowSpacing;
                arrowText.setPosition({arrowX, arrowY});
                window->draw(arrowText);

                float horizontalPadding = arrowSpacing * 0.4f;
                float clickX = arrowX - horizontalPadding;
                float clickY = arrowY - lineSpacing * 0.2f;
                float clickWidth = arrowSpacing * 0.8f;
                float clickHeight = lineSpacing * 1.4f;
                sf::FloatRect clickArea(sf::Vector2f(clickX, clickY), sf::Vector2f(clickWidth, clickHeight));
                navArrowAreas.push_back({arrows[a].first, clickArea});
            }

            sf::Text folderText(uiFont, folderDisplay, fontSize);
            folderText.setFillColor(sf::Color(144, 238, 144));
            folderText.setPosition({startX, contentStartY + lineSpacing});
            window->draw(folderText);
            return;
        }

        if (allImagePaths.empty())
            return;
        const auto &imagePath = allImagePaths[currentIndex];

        std::vector<std::string> lines;

        // Calculate index line. Search results must use the active result set, not the underlying folder list.
        std::string indexLine = getCurrentNavigationIndexLabel();

        if (sortByNameCurrentFolder) {
            indexLine += " by name";
        }
        lines.push_back(indexLine);
        lines.push_back("");
        const std::string installText = "Install Exiftool";
        if (!exiftoolAvailable) {
            lines.push_back(installText);
            lines.push_back("");
        }
        std::string folderPath = trimTrailingSlash(toUtf8String(imagePath.parent_path()));
        lines.push_back(folderPath);
        lines.push_back(toUtf8String(imagePath.filename()));

        std::string dt = getExifString(imagePath, "DateTimeOriginal");
        std::string dateLine = dt.empty() ? "" : formatDate(dt);
        std::string timeLine = dt.empty() ? "" : formatTime(dt);
        if (timeLine == "00:00:00") {
            timeLine.clear();
        }
        if (!dateLine.empty()) {
            lines.push_back("");
            lines.push_back(dateLine);
        }
        if (!timeLine.empty()) {
            lines.push_back(timeLine);
            lines.push_back("");
        }

        // Geographical data display logic
        const std::string &country = getExifString(imagePath, "Country");
        const std::string &state = getExifString(imagePath, "State");
        const std::string &city = getExifString(imagePath, "City");
        const std::string &location = getExifString(imagePath, "Location");

        std::string region;
        if (!geoKeywordPrefix.empty() && !regions.empty()) {
            // Extract region from keywords: first keyword starting with geo_keyword_prefix
            // where the trimmed value (without prefix) is in the configured regions list
            auto keywords = getKeywords(imagePath);
            for (const auto &keyword : keywords) {
                if (keyword.size() > geoKeywordPrefix.size() &&
                    keyword.substr(0, geoKeywordPrefix.size()) == geoKeywordPrefix) {
                    std::string candidate = keyword.substr(geoKeywordPrefix.size());
                    // Check if candidate is in the configured regions list
                    if (std::find(regions.begin(), regions.end(), candidate) != regions.end()) {
                        region = candidate;
                        break;
                    }
                }
            }
        }

        // Print country if: country is not empty AND (home_country != country OR all region/state/city/location are
        // empty)
        if (!country.empty() &&
            (homeCountry != country || (region.empty() && state.empty() && city.empty() && location.empty()))) {
            lines.push_back(country);
        }

        // Print region if: region is not empty AND (home_country != country OR all state/city/location are empty)
        if (!region.empty() && (homeCountry != country || (state.empty() && city.empty() && location.empty()))) {
            lines.push_back(region);
        }

        // Print state if: state is not empty AND ((home_country is empty OR home_country != country) OR (region and
        // city and location are all empty))
        if (!state.empty() && ((homeCountry.empty() || homeCountry != country) ||
                               (region.empty() && city.empty() && location.empty()))) {
            lines.push_back(state);
        }

        // Print city if not empty
        if (!city.empty()) {
            lines.push_back(city);
        }

        // Print location if not empty
        if (!location.empty()) {
            lines.push_back(location);
        }

        const std::string &description = getExifString(imagePath, "Description");
        if (!description.empty()) {
            lines.push_back("");
            lines.push_back(description);
        }

        bool hasValidGps = hasGpsLatitude(imagePath);

        // Add map links if GPS coordinates are available
        if (hasValidGps && !maps.empty()) {
            lines.push_back(""); // Blank line before maps
            for (const auto &m : maps) {
                std::string lowerName = m.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowerName.find("google") == std::string::npos) {
                    lines.push_back(m.name);
                }
            }
        }

        if (currentIndex == 0) {
            hasShownFirstImage = true;
        }

        const sf::Color infoColor(144, 238, 144); // light green
        const sf::Color mapColor(0, 255, 255);    // cyan
        const float startX = 20.0f;
        const float startY = 20.0f;
        unsigned int fontSize = getCalculatedFontSize();
        const float lineSpacing = static_cast<float>(fontSize + 4);

        if (uiFontLoaded) {
            float maxLineWidth = 0.0f;
            for (const auto &line : lines) {
                sf::Text probe(uiFont, sf::String::fromUtf8(line.begin(), line.end()), fontSize);
                sf::FloatRect lb = probe.getLocalBounds();
                maxLineWidth = std::max(maxLineWidth, lb.size.x);
            }
            float desired = std::max(420.0f, maxLineWidth + 80.0f);
            topLeftInfoBoxWidth = std::min(desired, static_cast<float>(window->getSize().x) - 40.0f);
        }

        float contentStartY = drawSearchTopRow(startX, startY, fontSize, lineSpacing);

        if (searchUiOpen && !searchSuggestions.empty()) {
            return;
        }

        // Clear previous map link areas and navigation arrow areas
        mapLinkAreas.clear();
        navArrowAreas.clear();

        if (uiFontLoaded) {
            for (size_t i = 0; i < lines.size(); i++) {
                // Check if this line is a map link and find which map it is
                int mapIndex = -1;
                for (size_t m = 0; m < maps.size(); m++) {
                    if (lines[i] == maps[m].name) {
                        mapIndex = static_cast<int>(m);
                        break;
                    }
                }
                bool isMapLink = (mapIndex >= 0);

                sf::String sfLine = sf::String::fromUtf8(lines[i].begin(), lines[i].end());
                sf::Text text(uiFont, sfLine, fontSize);
                sf::Color textColor =
                    isMapLink ? mapColor
                              : (lines[i] == installText ? sf::Color::Red
                                                         : (lines[i] == indexLine ? sf::Color::White : infoColor));
                text.setFillColor(textColor);
                float yPos = contentStartY + static_cast<float>(i) * lineSpacing;
                text.setPosition({startX, yPos});
                window->draw(text);

                // Draw navigation arrows after the index line (first line)
                if (i == 0 && lines[i] == indexLine) {
                    auto indexBounds = text.getLocalBounds();
                    float arrowStartX = startX + indexBounds.size.x + 20.0f;
                    float arrowY = yPos;
                    float arrowSize = fontSize;
                    float arrowSpacing = arrowSize + 12.0f;

                    // Define arrow symbols using simple characters
                    std::vector<std::pair<NavArrow, std::string>> arrows = {
                        {NavArrow::Left, "<"},  // left
                        {NavArrow::Right, ">"}, // right
                        {NavArrow::Up, "^"},    // up
                        {NavArrow::Down, "v"}   // down
                    };

                    for (size_t a = 0; a < arrows.size(); a++) {
                        sf::Text arrowText(uiFont, arrows[a].second, static_cast<unsigned int>(arrowSize));
                        arrowText.setFillColor(sf::Color::White);
                        float arrowX = arrowStartX + a * arrowSpacing;
                        arrowText.setPosition({arrowX, arrowY});
                        window->draw(arrowText);

                        // Store clickable area for this arrow with generous padding
                        // Add padding on both sides, but leave a gap between buttons
                        float horizontalPadding = arrowSpacing * 0.4f; // 40% of spacing as padding on left

                        // Extend clickable area vertically to cover full line height
                        // Start from same Y as text, extend down by full line spacing
                        float clickX = arrowX - horizontalPadding;
                        float clickY = arrowY - lineSpacing * 0.2f; // Start a bit above the text
                        float clickWidth = arrowSpacing * 0.8f;     // 80% of spacing, leaving gaps between buttons
                        float clickHeight = lineSpacing * 1.4f;     // Extend well below the text

                        sf::FloatRect clickArea(sf::Vector2f(clickX, clickY), sf::Vector2f(clickWidth, clickHeight));
                        navArrowAreas.push_back({arrows[a].first, clickArea});
                    }
                }

                // Draw underline for map links and store clickable area
                if (isMapLink) {
                    auto bounds = text.getLocalBounds();
                    sf::RectangleShape underline(sf::Vector2f(bounds.size.x, 2.0f));
                    underline.setFillColor(mapColor);
                    underline.setPosition({startX, yPos + bounds.size.y + 2.0f});
                    window->draw(underline);

                    // Store clickable area with map index
                    sf::FloatRect clickArea(sf::Vector2f(startX, yPos),
                                            sf::Vector2f(bounds.size.x, bounds.size.y + 4.0f));
                    mapLinkAreas.push_back({mapIndex, clickArea});
                }
            }
            return;
        }
    }

    void handleKeyPress(const sf::Event::KeyPressed &key) {
        log_stdout("DEBUG search: key pressed code=", static_cast<int>(key.code), " searchUiOpen=", searchUiOpen,
                   " searchInputFocused=", searchInputFocused, " tokens=", searchTokens.size(), " prefix='",
                   searchPrefix, "'");

        if (searchUiOpen) {
            if (key.code == sf::Keyboard::Key::Escape) {
                closeSearchUiAndRestore();
                return;
            }

            if (key.code == sf::Keyboard::Key::Enter) {
                log_stdout("DEBUG search: Enter pressed in search UI");
                if (submitSearchQuery()) {
                    closeSearchUiAfterSubmit();
                }
                return;
            }

            if (searchInputFocused) {
                switch (key.code) {
                case sf::Keyboard::Key::Left:
                    searchPrefixCursor = prevUtf8Offset(searchPrefix, searchPrefixCursor);
                    return;
                case sf::Keyboard::Key::Right:
                    searchPrefixCursor = nextUtf8Offset(searchPrefix, searchPrefixCursor);
                    return;
                case sf::Keyboard::Key::Backspace:
                    if (searchPrefixCursor > 0) {
                        size_t prev = prevUtf8Offset(searchPrefix, searchPrefixCursor);
                        searchPrefix.erase(prev, searchPrefixCursor - prev);
                        searchPrefixCursor = prev;
                        refreshSearchSuggestions();
                    } else if (!searchTokens.empty()) {
                        searchTokens.pop_back();
                        refreshSearchSuggestions();
                    }
                    return;
                case sf::Keyboard::Key::Delete:
                    if (searchPrefixCursor < searchPrefix.size()) {
                        size_t next = nextUtf8Offset(searchPrefix, searchPrefixCursor);
                        searchPrefix.erase(searchPrefixCursor, next - searchPrefixCursor);
                        refreshSearchSuggestions();
                    }
                    return;
                case sf::Keyboard::Key::Up:
                    if (!searchSuggestions.empty()) {
                        if (highlightedSearchSuggestion < 0) {
                            highlightedSearchSuggestion = static_cast<int>(searchSuggestions.size()) - 1;
                        } else {
                            highlightedSearchSuggestion =
                                (highlightedSearchSuggestion - 1 + static_cast<int>(searchSuggestions.size())) %
                                static_cast<int>(searchSuggestions.size());
                        }
                    }
                    return;
                case sf::Keyboard::Key::Down:
                    if (!searchSuggestions.empty()) {
                        if (highlightedSearchSuggestion < 0) {
                            highlightedSearchSuggestion = 0;
                        } else {
                            highlightedSearchSuggestion =
                                (highlightedSearchSuggestion + 1) % static_cast<int>(searchSuggestions.size());
                        }
                    }
                    return;
                default:
                    break;
                }
            }
        }

        if (key.code == sf::Keyboard::Key::Backspace && searchResultsActive && thumbnailMode && !folderMode) {
            showCurrentSearchResultInItsFolder();
            return;
        }

        if (key.code == sf::Keyboard::Key::Enter && searchResultsActive && thumbnailMode && !folderMode) {
            thumbnailMode = false;
            loadImage(currentIndex);
            return;
        }

        if (key.code == sf::Keyboard::Key::Enter && thumbnailMode &&
            lastSearchSubmitTime != std::chrono::steady_clock::time_point::min()) {
            auto elapsed = std::chrono::steady_clock::now() - lastSearchSubmitTime;
            if (elapsed <= std::chrono::milliseconds(250)) {
                return;
            }
        }

        // F1 - Toggle help
        if (key.code == sf::Keyboard::Key::F1) {
            showHelp = !showHelp;
            return;
        }

        // Escape - dismiss help or close window
        if (key.code == sf::Keyboard::Key::Escape) {
            // If help is showing, just close it
            if (showHelp) {
                showHelp = false;
            } else {
                navigationMessage = "To exit, use Ctrl-F4";
            }
            return;
        }

        // If help is showing, don't process other keys
        if (showHelp) {
            return;
        }

        if (key.control && key.code == sf::Keyboard::Key::F) {
            openSearchUi();
            return;
        }

        // If pre-caching is happening, queue the key press (max 1 queued)
        if (isPreCaching) {
            std::lock_guard<std::mutex> lock(keyQueueMutex);
            // Clear old queued keys - only keep the most recent one
            while (!pendingKeyPresses.empty())
                pendingKeyPresses.pop();
            pendingKeyPresses.push(key);
            log_stdout("Queued key press (pre-caching in progress, queue size: 1)");
            return;
        }

        // F11 - toggle fullscreen/windowed
        if (key.code == sf::Keyboard::Key::F11) {
            toggleWindowMode();
            return;
        }

        // 0 - reset map zoom to default at the current map center
        if (key.code == sf::Keyboard::Key::Num0 || key.code == sf::Keyboard::Key::Numpad0) {
            if (mapViewer && mapViewer->isOpen()) {
                mapViewer->showMap(mapViewer->getCenterLat(), mapViewer->getCenterLon(), defaultZoom);
            }
            return;
        }

        // Backspace - from full image view, enter thumbnail mode for current folder;
        // from thumbnail view, go up to parent/common folder (only within watched folder)
        if (key.code == sf::Keyboard::Key::Backspace) {
            if (!thumbnailMode) {
                toggleThumbnailMode();
                lastBackspaceEnterThumbnailTime = std::chrono::steady_clock::now();
                return;
            }

            if (!folderMode) {
                auto now = std::chrono::steady_clock::now();
                if (lastBackspaceEnterThumbnailTime != std::chrono::steady_clock::time_point::min() &&
                    now - lastBackspaceEnterThumbnailTime <= std::chrono::milliseconds(250)) {
                    return;
                }
            }

            if (thumbnailMode && folderMode && watchedFoldersMode) {
                navigationMessage = "Reached top level";
                return;
            }

            if (folderModeEligible && !currentWatchedFolder.empty() && isWithinCurrentWatchedFolder(currentFolder)) {
                fs::path previousFolder = normalizePath(fs::absolute(currentFolder));

                if (folderMode && previousFolder == normalizePath(fs::absolute(currentWatchedFolder))) {
                    enterWatchedFoldersMode(previousFolder);
                    return;
                }

                fs::path targetFolder = previousFolder.parent_path();

                if (targetFolder.empty() || !isWithinCurrentWatchedFolder(targetFolder)) {
                    targetFolder = computeSeenImagesCommonFolder();
                    if (targetFolder.empty()) {
                        targetFolder = previousFolder;
                    }
                    targetFolder = normalizePath(fs::absolute(targetFolder));
                    if (targetFolder == previousFolder) {
                        fs::path parent = targetFolder.parent_path();
                        if (!parent.empty() && isWithinCurrentWatchedFolder(parent)) {
                            targetFolder = parent;
                        }
                    }
                }

                if (!targetFolder.empty() && isWithinCurrentWatchedFolder(targetFolder)) {
                    if (targetFolder == previousFolder) {
                        enterWatchedFoldersMode(previousFolder);
                    } else {
                        enterFolderMode(targetFolder, previousFolder);
                    }
                } else {
                    enterWatchedFoldersMode(previousFolder);
                }
            }
            return;
        }

        // Check filter toggle keys before processing navigation
        for (int i = 0; i < static_cast<int>(filters.size()); i++) {
            if (filters[i].key.size() == 1) {
                char keyChar = filters[i].key[0];
                if (keyChar >= 'a' && keyChar <= 'z') {
                    // Map 'a' to 'z' to A to Z keys (assuming sf::Keyboard::Key::A through Z are consecutive)
                    if (key.code >= sf::Keyboard::Key::A && key.code <= sf::Keyboard::Key::Z) {
                        int keyOffset = static_cast<int>(key.code) - static_cast<int>(sf::Keyboard::Key::A);
                        if (keyOffset == (keyChar - 'a')) {
                            if (activeFilterIndex == i) {
                                activeFilterIndex = -1;
                                log_stdout("Filter deactivated");
                            } else {
                                activeFilterIndex = i;
                                log_stdout("Filter activated: ", filters[i].expression);
                            }

                            // Rebuild image list with new filter status
                            if (!allDirectories.empty() && currentIndex < static_cast<int>(allDirectories.size())) {
                                fs::path currentDir = allDirectories[currentIndex];
                                fs::path currentImagePath =
                                    allImagePaths.empty() ? fs::path() : allImagePaths[currentIndex];

                                refreshThumbnailsAfterImageSetChange();

                                allImagePaths.clear();
                                allDirectories.clear();
                                buildImageList(currentDir);

                                // Try to keep the same image if it's still in the filtered list
                                int newIndex = -1;
                                if (!currentImagePath.empty()) {
                                    for (int j = 0; j < static_cast<int>(allImagePaths.size()); j++) {
                                        if (allImagePaths[j] == currentImagePath) {
                                            newIndex = j;
                                            break;
                                        }
                                    }
                                }

                                // If current image was filtered out, jump to the first available image
                                if (newIndex == -1) {
                                    newIndex = 0;
                                }

                                currentIndex = newIndex;

                                // Ensure currentIndex is valid
                                if (currentIndex >= static_cast<int>(allImagePaths.size())) {
                                    currentIndex = 0;
                                }

                                // If the filtered list has images, load the one at currentIndex
                                if (!allImagePaths.empty()) {
                                    loadImage(static_cast<size_t>(currentIndex));
                                }
                            }

                            return;
                        }
                    }
                }
            }
        }

        if (thumbnailMode && !folderMode && allImagePaths.empty()) {
            return;
        }

        if (thumbnailMode && folderMode) {
            if (folderModeEntries.empty()) {
                return;
            }

            folderModeFocusIndex = std::min(folderModeFocusIndex, folderModeEntries.size() - 1);
            ThumbnailLayout layout = computeThumbnailLayout();
            size_t last = folderModeEntries.size() - 1;
            size_t pageSize =
                std::max<size_t>(1, static_cast<size_t>(layout.visibleRows) * static_cast<size_t>(thumbnailColumns));

            switch (key.code) {
            case sf::Keyboard::Key::Home:
                folderModeFocusIndex = 0;
                break;
            case sf::Keyboard::Key::End:
                folderModeFocusIndex = last;
                break;
            case sf::Keyboard::Key::PageUp:
                if (folderModeFocusIndex >= pageSize) {
                    folderModeFocusIndex -= pageSize;
                } else {
                    folderModeFocusIndex = 0;
                }
                break;
            case sf::Keyboard::Key::PageDown:
                folderModeFocusIndex = std::min(last, folderModeFocusIndex + pageSize);
                break;
            case sf::Keyboard::Key::Left:
                if (folderModeFocusIndex > 0) {
                    folderModeFocusIndex--;
                }
                break;
            case sf::Keyboard::Key::Right:
                if (folderModeFocusIndex < last) {
                    folderModeFocusIndex++;
                }
                break;
            case sf::Keyboard::Key::Up:
                if (folderModeFocusIndex >= static_cast<size_t>(thumbnailColumns)) {
                    folderModeFocusIndex -= static_cast<size_t>(thumbnailColumns);
                }
                break;
            case sf::Keyboard::Key::Down:
                if (folderModeFocusIndex + static_cast<size_t>(thumbnailColumns) <= last) {
                    folderModeFocusIndex += static_cast<size_t>(thumbnailColumns);
                }
                break;
            case sf::Keyboard::Key::Enter:
                openFolderModeEntry(folderModeFocusIndex);
                return;
            default:
                break;
            }

            ensureFolderModeFocusVisible();
            refreshMapForFolderFocusSelection();
            return;
        }

        switch (key.code) {
        case sf::Keyboard::Key::Home:
            if (isSearchResultSetActive()) {
                navigateWithinSearchResultsToBoundary(false, thumbnailMode);
            } else if (thumbnailMode) {
                currentIndex = getFirstInFolder();
                onThumbnailSelectionChanged();
            } else {
                loadImage(getFirstInFolder());
            }
            break;

        case sf::Keyboard::Key::PageDown:
            if (isSearchResultSetActive()) {
                int pageStep = thumbnailMode
                                   ? std::max(1, computeThumbnailLayout().visibleRows * std::max(1, thumbnailColumns))
                                   : 10;
                navigateWithinSearchResultsByOffset(pageStep, thumbnailMode);
            } else if (thumbnailMode) {
                moveThumbnailPageDownOrNextFolder();
            } else {
                metadataCollectionMessageActive = true;
                presentFrameNow();
                loadImage(getFirstInNextFolder());
                metadataCollectionMessageActive = false;
            }
            break;

        case sf::Keyboard::Key::Left:
            if (isSearchResultSetActive()) {
                navigateWithinSearchResultsByOffset(-1, thumbnailMode);
            } else if (thumbnailMode) {
                currentIndex = getPrevInFolder();
                onThumbnailSelectionChanged();
            } else {
                loadImage(getPrevInFolder());
            }
            break;

        case sf::Keyboard::Key::Right:
            if (isSearchResultSetActive()) {
                navigateWithinSearchResultsByOffset(1, thumbnailMode);
            } else if (thumbnailMode) {
                currentIndex = getNextInFolder();
                onThumbnailSelectionChanged();
            } else {
                loadImage(getNextInFolder());
            }
            break;

        case sf::Keyboard::Key::PageUp:
            if (isSearchResultSetActive()) {
                int pageStep = thumbnailMode
                                   ? std::max(1, computeThumbnailLayout().visibleRows * std::max(1, thumbnailColumns))
                                   : 10;
                navigateWithinSearchResultsByOffset(-pageStep, thumbnailMode);
            } else if (thumbnailMode) {
                moveThumbnailPageUpOrPrevFolder();
            } else {
                metadataCollectionMessageActive = true;
                presentFrameNow();
                loadImage(getFirstInPrevFolder());
                metadataCollectionMessageActive = false;
            }
            break;

        case sf::Keyboard::Key::End:
            if (isSearchResultSetActive()) {
                navigateWithinSearchResultsToBoundary(true, thumbnailMode);
            } else if (thumbnailMode) {
                currentIndex = getLastInFolder();
                onThumbnailSelectionChanged();
            } else {
                loadImage(getLastInFolder());
            }
            break;

        case sf::Keyboard::Key::Up:
            if (isSearchResultSetActive()) {
                int step = thumbnailMode ? -std::max(1, thumbnailColumns) : -1;
                navigateWithinSearchResultsByOffset(step, thumbnailMode);
            } else if (thumbnailMode && currentIndex >= static_cast<size_t>(thumbnailColumns)) {
                currentIndex -= static_cast<size_t>(thumbnailColumns);
                onThumbnailSelectionChanged();
            }
            break;

        case sf::Keyboard::Key::Down:
            if (isSearchResultSetActive()) {
                int step = thumbnailMode ? std::max(1, thumbnailColumns) : 1;
                navigateWithinSearchResultsByOffset(step, thumbnailMode);
            } else if (thumbnailMode) {
                size_t candidate = currentIndex + static_cast<size_t>(thumbnailColumns);
                if (candidate < allImagePaths.size()) {
                    currentIndex = candidate;
                    onThumbnailSelectionChanged();
                }
            }
            break;

        case sf::Keyboard::Key::Enter:
            if (thumbnailMode && !allImagePaths.empty() && currentIndex < allImagePaths.size()) {
                thumbnailMode = false;
                folderMode = false;
                closeContextMenu();
                loadImage(currentIndex);
            }
            break;

        case sf::Keyboard::Key::W:
            // Alt+F4 is handled by OS, Ctrl+W would need modifiers
            if (key.control) {
                window->close();
            }
            break;

        case sf::Keyboard::Key::C:
            if (key.control) {
                copyImagePathToClipboard();
            }
            break;

        case sf::Keyboard::Key::Period:
            // Toggle embedded map viewer
            if (mapViewer && mapViewer->isOpen()) {
                mapViewer->close();
            } else if (mapViewer && !allImagePaths.empty()) {
                const auto &imagePath = allImagePaths[currentIndex];
                if (hasGpsLatitude(imagePath)) {
                    double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
                    double lon = getGpsValueOrZero(imagePath, "GPSLongitude");
                    mapViewer->showMap(lat, lon, defaultZoom);
                    log_stdout("Opening map for ", lat, ", ", lon);

                    // Collect all GPS points from current folder (only for images passing filter)
                    std::vector<std::pair<double, double>> folderGpsPoints;
                    fs::path currentDir = imagePath.parent_path();
                    for (size_t i = 0; i < allImagePaths.size(); i++) {
                        if (allDirectories[i] == currentDir && hasGpsLatitude(allImagePaths[i]) &&
                            passesActiveFilter(allImagePaths[i])) {
                            double ptLat = getGpsValueOrZero(allImagePaths[i], "GPSLatitude");
                            double ptLon = getGpsValueOrZero(allImagePaths[i], "GPSLongitude");
                            folderGpsPoints.push_back({ptLat, ptLon});
                        }
                    }
                    mapViewer->setGPSPoints(folderGpsPoints);
                } else {
                    log_stdout("No GPS data for current image");
                }
            }
            break;

        default:
            break;
        }
    }

    void processPendingKeyPresses() {
        if (isPreCaching)
            return;

        std::lock_guard<std::mutex> lock(keyQueueMutex);
        while (!pendingKeyPresses.empty()) {
            sf::Event::KeyPressed key = pendingKeyPresses.front();
            pendingKeyPresses.pop();
            log_stdout("Processing queued key press");
            handleKeyPress(key);
        }
    }
};

// Cache mode logic is implemented in precache.cpp.

int runSearchMode(const std::vector<std::string> &requiredTokensInput, const std::string &prefixInput,
                  const std::string &configPath) {
    if (prefixInput.empty()) {
        log_stderr("Error: search requires non-empty prefix");
        return 1;
    }

    std::string prefix = prefixInput;
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<std::string> requiredTokens;
    std::unordered_set<std::string> seenRequired;
    requiredTokens.reserve(requiredTokensInput.size());
    for (const auto &token : requiredTokensInput) {
        if (token.empty()) {
            continue;
        }
        if (seenRequired.insert(token).second) {
            requiredTokens.push_back(token);
        }
    }

    bool asciiOnly = true;
    for (unsigned char c : prefix) {
        if (c >= 128) {
            asciiOnly = false;
            break;
        }
    }

    fs::path cacheRoot;
    try {
        fs::path configDir = fs::path(configPath).parent_path();
        json config = loadAndValidateConfig(configDir);
        std::string location = config["map"]["cache"]["location"];
        if (!location.empty()) {
            cacheRoot = location;
        }
    } catch (const std::exception &e) {
        log_stderr("Error loading config: ", e.what());
        return 1;
    }

    if (cacheRoot.empty()) {
        cacheRoot = getDefaultCacheLocation();
    }

    std::string metadataCacheError;
    fs::path metadataCacheFile = metadata_cache::defaultMetadataCacheFile(cacheRoot);
    if (!metadata_cache::initializeMetadataCache(metadataCacheFile, metadataCacheError)) {
        log_stderr("Metadata cache initialization failed: ", metadataCacheError);
        return 1;
    }

    sqlite3 *db = nullptr;
    if (sqlite3_open(pathToString(metadataCacheFile).c_str(), &db) != SQLITE_OK) {
        log_stderr("Failed to open metadata cache DB: ", sqlite3_errmsg(db));
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return 1;
    }

    const std::string wordColumn = asciiOnly ? "w.simple" : "w.name";
    std::ostringstream queryBuilder;
    int prefixParamIndex = 1;

    if (!requiredTokens.empty()) {
        queryBuilder << "WITH selected_tokens(name) AS (";
        for (size_t i = 0; i < requiredTokens.size(); ++i) {
            if (i > 0) {
                queryBuilder << " UNION ALL ";
            }
            queryBuilder << "SELECT ?" << (i + 1);
        }
        queryBuilder << "), matched_content AS ("
                     << "SELECT ct.content_id "
                     << "FROM content_token ct "
                     << "JOIN token ft ON ft.id = ct.token_id "
                     << "JOIN selected_tokens st ON st.name = ft.name "
                     << "GROUP BY ct.content_id "
                     << "HAVING COUNT(DISTINCT st.name) = " << requiredTokens.size() << ") ";
        prefixParamIndex = static_cast<int>(requiredTokens.size()) + 1;
    }

    queryBuilder << "SELECT t.name, COUNT(DISTINCT ct.content_id) AS image_count "
                 << "FROM word w "
                 << "JOIN token_word tw ON tw.word_id = w.id "
                 << "JOIN token t ON t.id = tw.token_id "
                 << "JOIN content_token ct ON ct.token_id = t.id ";

    if (!requiredTokens.empty()) {
        queryBuilder << "JOIN matched_content mc ON mc.content_id = ct.content_id ";
    }

    queryBuilder << "WHERE " << wordColumn << " LIKE ?" << prefixParamIndex << " || '%' "
                 << "GROUP BY t.id, t.name "
                 << "ORDER BY image_count DESC, t.name ASC "
                 << "LIMIT 10;";

    const std::string query = queryBuilder.str();

    sqlite3_stmt *stmt = nullptr;
    if (sql::prepare(db, query.c_str(), &stmt) != SQLITE_OK) {
        log_stderr("Failed to prepare search query: ", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    for (size_t i = 0; i < requiredTokens.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), requiredTokens[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, prefixParamIndex, prefix.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *tokenName = sqlite3_column_text(stmt, 0);
        const sqlite3_int64 imageCount = sqlite3_column_int64(stmt, 1);
        std::cout << (tokenName ? reinterpret_cast<const char *>(tokenName) : "") << "\t" << imageCount << std::endl;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

// Helper function to extract metadata and output as YAML (used by --exiftool and --poor modes)
int show_yaml_metadata(std::function<std::map<fs::path, json>(const std::vector<fs::path> &)> extractFunc, int argc,
                       char *argv[], const std::string &modeName) {
    if (argc != 3) {
        log_stderr("Usage: ", argv[0], " ", modeName, " <image_file>");
        return 1;
    }

    std::vector<fs::path> expandedPaths = expandGlob(argv[2]);
    if (expandedPaths.empty()) {
        log_stderr("Error: File not found: ", argv[2]);
        return 1;
    }
    if (expandedPaths.size() > 1) {
        log_stderr("Error: Pattern matched multiple files; provide a single file.");
        return 1;
    }

    std::vector<fs::path> paths = {expandedPaths[0]};
    auto results = extractFunc(paths);
    auto resultIt = results.find(paths[0]);
    if (resultIt == results.end()) {
        log_stderr("Error: No metadata extracted for ", paths[0].string());
        return 1;
    }

    YAML::Node output = jsonToYamlNode(resultIt->second);
    YAML::Emitter emitter;
    emitter << output;
    std::cout << emitter.c_str() << std::endl;
    return 0;
}

// Self-check: validate all YAML files and schemas
int runSelfCheck(const std::string &exePath, const std::string &configPath) {
    log_stdout("Running self-check...");

    try {
        // 1. Validate built-in schemas
        log_stdout("  Checking embedded schemas...");
        validateBuiltInSchemas();
        validateSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, "EXIFTOOL_RESPONSE_SCHEMA_YAML");
        log_stdout("    ✓ Embedded schemas valid");

        // 2. Load and validate specified config file with enrichment test
        log_stdout("  Checking config file with enrichment...");
        log_stdout("    Config path: " + configPath);

        try {
            // Load the specific config file (don't search)
            if (!fs::exists(configPath)) {
                log_stderr("    ✗ Config file not found: ", configPath);
                fs::remove(exePath);
                return 1;
            }

            json enrichedConfig = loadAndValidateConfigFile(fs::path(configPath));
            log_stdout("    ✓ Config file enriched and validated successfully");
        } catch (const std::exception &e) {
            log_stderr("    ✗ Config enrichment failed: ", e.what());
            fs::remove(exePath);
            return 1;
        }

        // 3. Load and validate embedded exiftool schema YAML
        log_stdout("  Checking exiftool response schema...");
        YAML::Node schemaYaml = YAML::Load(EXIFTOOL_RESPONSE_SCHEMA_YAML);
        log_stdout("    ✓ Exiftool schema valid");

        log_stdout("Self-check passed!");
        return 0;

    } catch (const YAML::Exception &e) {
        log_stderr("YAML error: ", e.what());
        fs::remove(exePath);
        return 1;
    } catch (const std::exception &e) {
        log_stderr("Error: ", e.what());
        fs::remove(exePath);
        return 1;
    }
}

static int resolveLogRetentionDays(const fs::path &configFilePath) {
    constexpr int defaultRetentionDays = 3;

    try {
        json logConfig = loadAndValidateConfigFile(configFilePath);
        if (logConfig.contains("log") && logConfig["log"].is_object() && logConfig["log"].contains("retention_days") &&
            logConfig["log"]["retention_days"].is_number_integer()) {
            int retentionDays = logConfig["log"]["retention_days"].get<int>();
            return std::max(0, retentionDays);
        }
    } catch (...) {
        // Fall back to default when config cannot be parsed here.
    }

    return defaultRetentionDays;
}

// Simple argument parsing for our specific modes
int main(int argc, char *argv[]) {
#ifdef _WIN32
    // Set Windows console to UTF-8 mode for proper Unicode output
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    (void)initialize_app_logging(3);

    // Validate built-in schemas on startup
    try {
        validateBuiltInSchemas();
        validateSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, "EXIFTOOL_RESPONSE_SCHEMA_YAML");
    } catch (const std::exception &e) {
        log_stderr("Fatal: Schema validation failed: ", e.what());
        return 1;
    }

    // Parse command line arguments
    std::string configFile; // Default will be set by loadConfig if empty
    std::string imageFile;
    std::string mode; // "", "self-check", "cache", "search", "exiftool", "poor"
    std::vector<std::string> cachePaths;
    bool cacheUseExistingThumb = false;
    bool cacheIgnoreDirMtime = false;
    CacheRefreshTarget cacheForceRefreshTarget = CacheRefreshTarget::None;
    std::vector<std::string> searchArgs;

    // Default config is exe path with .exe replaced by .yaml
    {
        fs::path exe(argv[0]);
        configFile = (exe.parent_path() / exe.stem()).string() + ".yaml";
    }

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--self-check") {
            mode = "self-check";
        } else if (arg == "--config") {
            if (i + 1 < argc) {
                configFile = argv[++i];
            } else {
                log_stderr("Error: --config requires a file path");
                return 1;
            }
        } else if (arg == "--cache") {
            mode = "cache";
            // Collect remaining arguments as paths/options for cache mode.
            for (int j = i + 1; j < argc; ++j) {
                std::string cacheArg(argv[j]);
                if (cacheArg == "--use-existing-thumb") {
                    cacheUseExistingThumb = true;
                    continue;
                }
                if (cacheArg == "--ignore-dir-mtime") {
                    cacheIgnoreDirMtime = true;
                    continue;
                }
                if (cacheArg == "-f" || cacheArg == "--force-refresh") {
                    if (j + 1 >= argc) {
                        log_stderr("Error: ", cacheArg, " requires a cache type (metadata, thumb, map_tile)");
                        return 1;
                    }
                    auto refreshTarget = parseCacheRefreshTarget(argv[++j]);
                    if (!refreshTarget.has_value()) {
                        log_stderr("Error: invalid cache type for ", cacheArg,
                                   ", expected metadata, thumb, or map_tile");
                        return 1;
                    }
                    cacheForceRefreshTarget = *refreshTarget;
                    continue;
                }
                if (!cacheArg.empty() && cacheArg[0] == '-') {
                    log_stderr("Error: Unknown option for --cache: ", cacheArg);
                    return 1;
                }
                cachePaths.push_back(cacheArg);
            }
            break; // Consumed rest of argv
        } else if (arg == "search") {
            mode = "search";
            if (i + 1 >= argc) {
                log_stderr("Error: search requires a prefix");
                return 1;
            }
            for (int j = i + 1; j < argc; ++j) {
                searchArgs.push_back(argv[j]);
            }
            break; // Consumed rest of argv
        } else if (arg == "--exiftool") {
            mode = "exiftool";
            if (i + 1 < argc) {
                imageFile = argv[++i];
            } else {
                log_stderr("Error: --exiftool requires an image file");
                return 1;
            }
        } else if (arg == "--poor") {
            mode = "poor";
            if (i + 1 < argc) {
                imageFile = argv[++i];
            } else {
                log_stderr("Error: --poor requires an image file");
                return 1;
            }
        } else if (arg[0] == '-') {
            log_stderr("Error: Unknown option: ", arg);
            return 1;
        } else {
            if (mode == "search") {
                log_stderr("Error: search arguments must follow the search verb");
                return 1;
            }
            // Positional argument
            imageFile = arg;
        }
    }

    apply_log_retention_policy(resolveLogRetentionDays(fs::path(configFile)));

    // Log CLI command
    std::string cliLine = "CLI:";
    for (int i = 0; i < argc; ++i) {
        cliLine += " " + std::string(argv[i]);
    }
    log_stdout(cliLine);

    // Handle self-check mode
    if (mode == "self-check") {
        return runSelfCheck(argv[0], configFile);
    }

    // Handle cache mode
    if (mode == "cache") {
        if (cachePaths.empty()) {
            log_stderr("Error: --cache requires at least one path");
            return 1;
        }
        return runCacheMode(cachePaths, configFile, cacheUseExistingThumb, cacheForceRefreshTarget,
                            cacheIgnoreDirMtime);
    }

    // Handle search mode
    if (mode == "search") {
        if (searchArgs.empty()) {
            log_stderr("Error: search requires a prefix");
            return 1;
        }
        const std::string prefix = searchArgs.back();
        std::vector<std::string> requiredTokens(searchArgs.begin(), searchArgs.end() - 1);
        return runSearchMode(requiredTokens, prefix, configFile);
    }

    // Handle exiftool mode
    if (mode == "exiftool") {
        std::string resolvedExiftoolPath;
        bool found = metadata::findExiftool(resolvedExiftoolPath);
        g_exiftoolPath = resolvedExiftoolPath;
        if (!found || g_exiftoolPath.empty()) {
            log_stderr("Error: exiftool not found. Please install exiftool.");
            return 1;
        }
        const char *newArgv[] = {argv[0], "--exiftool", imageFile.c_str()};
        auto extractFunc = [](const std::vector<fs::path> &paths) {
            return metadata::extractExiftoolData(paths, g_exiftoolPath);
        };
        return show_yaml_metadata(extractFunc, 3, (char **)newArgv, "--exiftool");
    }

    // Handle poor mode
    if (mode == "poor") {
        const char *newArgv[] = {argv[0], "--poor", imageFile.c_str()};
        return show_yaml_metadata(extractImageMetadata, 3, (char **)newArgv, "--poor");
    }

    try {
        if (imageFile.empty()) {
            MgVwr viewer{fs::path(), fs::path(argv[0]), configFile};
            viewer.run();
            return 0;
        }

        // Expand glob patterns (especially important on Windows where shell doesn't expand *)
        std::vector<fs::path> expandedPaths = expandGlob(imageFile);

        if (expandedPaths.empty()) {
            log_stderr("Error: File not found: ", imageFile);
            return 1;
        }

        if (expandedPaths.size() > 1) {
            log_stdout("Multiple files matched pattern. Opening first: ", expandedPaths[0].filename());
        }

        MgVwr viewer{expandedPaths[0], fs::path(argv[0]), configFile};
        viewer.run();
    } catch (const std::exception &e) {
        log_stderr("Error: ", e.what());
        return 1;
    }

    return 0;
}
