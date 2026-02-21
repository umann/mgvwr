#include "config.h"
#include "help.h"
#include "json.hpp"
#include "map_viewer.h"
#include "poor_mans_exiftool.h"
#include "utils.h"
#include <SFML/Graphics.hpp>
#include <SFML/Window/Clipboard.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <yaml-cpp/yaml.h>

#ifdef _WIN32
#include <windows.h>
#include <regex>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

using ImageMetadataCache = std::map<fs::path, json>;

static std::string g_exiftoolPath;

// POOR_MANS_SUPPORTED_SUFFIXES = {".jpg", ".jpeg"}

// Embedded schema YAML as a string constant
static constexpr const char *EXIFTOOL_RESPONSE_SCHEMA_YAML = R"(
type: array
items:  
  type: object
  additionalProperties: false
  properties:
    SourceFile:
      type: string
      minLength: 1
      example: "F:/photos/2026/01/02/kornel_20260102_114713.jpg"
    DateTimeOriginal:
      type: string
      pattern: "^\\d{4}:\\d{2}:\\d{2} \\d{2}:\\d{2}:\\d{2}$"
      default: "0000:00:00 00:00:00"
      example: "2026:01:02 11:47:14"
    City:
      type: string
      default: ""
      example: "Budapest"
    Location:
      type: string
      default: ""
      example: "Margit-sziget"
    Country:
      type: string
      default: ""
      example: "Belgium"
    State:
      type: string
      default: ""
      example: "Pest megye"
    Description:
      type: string
      default: ""
      example: "A beautiful landscape photo."
    Orientation:
      type: integer
      minimum: 1
      maximum: 8
      default: 1
      example: 7
    Keywords:
      type: array
      items:
        type: string
        minLength: 1
        example: "apple"
      default: []
      example: ["apple", "banana"]
    GPSLatitude:
      type: ["number", "null"]
      minimum: -90
      maximum: 90
      default: null
      example: 47.675997
    GPSLongitude:
      type: ["number", "null"]
      minimum: -180
      maximum: 180
      default: null
      example: 19.1444994
    filters:
      type: object
      additionalProperties: false
      default: {}
  required:
    - SourceFile
)";

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

// Check if exiftool is available in PATH and return full path
std::pair<bool, std::string> findExiftool() {
#ifdef _WIN32
    std::string command = "where";
    std::string stderr_redirect = "2>nul";
#else
    std::string command = "which";
    std::string stderr_redirect = "2>/dev/null";
#endif

    FILE *pipe = popen((command + " exiftool " + stderr_redirect).c_str(), "r");
    if (!pipe) {
        log_stdout("No exiftool found");
        return {false, ""};
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line = buffer;
        // Remove trailing newline
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        if (line.empty())
            continue;

#ifdef _WIN32
        // Windows: verify line ends with .exe or .bat
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

        log_stdout("Exiftool found at ", line);
        pclose(pipe);
        return {true, line};
    }

    pclose(pipe);
    log_stdout("No exiftool found");
    return {false, ""};
}

// Extract metadata using exiftool
static const std::vector<std::string> EXIF_STRING_KEYS = {
    "City", "Country", "DateTimeOriginal", "Description", "Location", "Orientation", "State",
};

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

// Unified exiftool extraction for one or more image paths
// Returns: map<image_path, metadata object>
std::map<fs::path, json> extractExiftoolData(const std::vector<fs::path> &imagePaths) {

    std::map<fs::path, json> results;

    if (imagePaths.empty() || g_exiftoolPath.empty()) {
        return results;
    }

    try {
        // Build CLI flags for all required keys
        std::string cliFlags = "-j -n -q";

        for (const auto &key : EXIF_STRING_KEYS) {
            cliFlags += " -" + key;
        }
        cliFlags += " -Keywords -GPSLatitude -GPSLongitude";

        // Build image paths argument
        std::string imagePathsArg;
        for (size_t i = 0; i < imagePaths.size(); ++i) {
            if (i > 0)
                imagePathsArg += "\" \"";
            imagePathsArg += imagePaths[i].string();
        }

        // Construct platform-specific command
        std::string cmd;
        std::string exiftoolPathFixed = g_exiftoolPath;

#ifdef _WIN32
        // Windows: convert any forward slashes to backslashes for exiftool
        for (char &c : exiftoolPathFixed) {
            if (c == '/')
                c = '\\';
        }
        cmd = "cmd /c \"\"" + exiftoolPathFixed + "\" " + cliFlags + " \"" + imagePathsArg + "\"\"";
#else
        // Unix/Linux: convert any backslashes to forward slashes
        for (char &c : exiftoolPathFixed) {
            if (c == '\\')
                c = '/';
        }
        cmd = "\"" + exiftoolPathFixed + "\" " + cliFlags + " \"" + imagePathsArg + "\"";
#endif

        log_stdout("DEBUG", "Exiftool batch command: ", cmd);

        // Execute exiftool
        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            log_stdout("DEBUG", "popen failed for exiftool");
            return results;
        }

        std::string output;
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        int exitCode = pclose(pipe);

        log_stdout("DEBUG", "exiftool exit code: ", exitCode, ", output size: ", output.size(), " bytes");

        if (exitCode != 0) {
            log_stdout("DEBUG", "exiftool failed or returned no output");
            return results;
        }

        // Parse JSON response (should be array of objects that is check with schema)
        auto jsonData = json::parse(output);

        // Apply schema defaults and validate exiftool response
        try {
            jsonData = enrichAndValidateJsonWithSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, jsonData);
        } catch (const std::exception &e) {
            log_stdout("DEBUG", "Exiftool response schema validation failed: ", e.what());
            return results;
        }

        for (const auto &record : jsonData) {
            if (!record.is_object() || !record.contains("SourceFile")) {
                continue;
            }
            try {
                fs::path imagePath(record["SourceFile"].get<std::string>());
                results[imagePath] = record;
                log_stdout("DEBUG", "Extracted metadata for ", imagePath.filename().string());
            } catch (const std::exception &e) {
                log_stdout("DEBUG", "Error processing record: ", e.what());
            }
        }

    } catch (const std::exception &e) {
        log_stdout("DEBUG", "Exception in extractExiftoolData: ", e.what());
    }

    return results;
}

// Get default cache directory location based on platform environment variables
// Returns base cache directory (without subdirectories like "osm")
static fs::path getDefaultCacheLocation() {
    const char *localAppDataEnv = std::getenv("LOCALAPPDATA");
    const char *homeEnv = std::getenv("HOME");

    if (localAppDataEnv != nullptr) {
        return fs::path(localAppDataEnv) / "Umann" / "MgVwr" / "cache";
    } else if (homeEnv != nullptr) {
        return fs::path(homeEnv) / ".cache" / "umann" / "mgvwr";
    } else {
        return fs::path(".") / "cache";
    }
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
    } else {
        try {
            return static_cast<unsigned int>(std::stoul(sizeStr));
        } catch (...) {
            return maxValue;
        }
    }
}

unsigned int parseSizeValue(const nlohmann::json &v, unsigned int maxValue) {
    if (v.is_null())
        return maxValue;

    if (v.is_number_unsigned())
        return v.get<unsigned int>();
    if (v.is_number_integer()) {
        auto x = v.get<long long>();
        return x > 0 ? static_cast<unsigned int>(x) : 0u; // or maxValue if you prefer
    }
    if (v.is_number_float()) {
        auto x = v.get<double>();
        return x > 0 ? static_cast<unsigned int>(x) : 0u; // or maxValue
    }
    if (v.is_string()) {
        return parseSizeValue(v.get_ref<const std::string &>(), maxValue);
    }

    return maxValue; // unexpected type
}

class MgVwr {
  private:
    json config;
    std::vector<fs::path> allImagePaths;
    std::vector<fs::path> allDirectories;
    fs::path currentWatchedFolder;
    fs::path currentFolder; // Currently displayed folder
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

    // Navigation messages
    std::string navigationMessage;
    float navigationMessageTime = 0.0f;

    // Font configuration
    json fontSizeConfig;

    // Config values
    bool quietMode;
    bool singleInstanceMode;
    bool experimental;
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

    // Navigation arrow system
    enum class NavArrow { Left, Right, Up, Down };
    std::vector<std::pair<NavArrow, sf::FloatRect>>
        navArrowAreas; // Store (arrow_direction, clickable_area) for navigation arrows

    // Help system
    bool showHelp = false;
    std::vector<std::string> helpLines;

    std::vector<std::string> supportedSuffixes;
    std::vector<PathClassification> pathClassifications;

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
        // Schema guarantees Keywords is always an array of strings
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

    // Initialize metadata entry with defaults from schema and complete: false
    void initializeIncompleteMetadata(const fs::path &imagePath) {
        json metaObject = json::object();
        metaObject["SourceFile"] = imagePath.string();

        // Enrich with schema defaults
        try {
            json enriched = enrichMetadataWithSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, metaObject);
            imageMetadataCache[imagePath] = enriched;
        } catch (const std::exception &e) {
            // If enrichment fails, fall back to basic object
            imageMetadataCache[imagePath] = metaObject;
        }

        // Mark as incomplete
        imageMetadataCache[imagePath]["complete"] = false;
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

            // Watched folders
            watchedFolders.clear();
            for (const auto &folder : config["watched_folders"]) {
                fs::path folderPath = normalizePath(fs::path(folder.get<std::string>()));
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

    void createWindow(bool fullscreen) {
        isFullscreen = fullscreen;
        sf::VideoMode mode = fullscreen ? desktopMode : sf::VideoMode(windowedSize);
        sf::State state = fullscreen ? sf::State::Fullscreen : sf::State::Windowed;
        std::uint32_t style = fullscreen ? sf::Style::None : sf::Style::Default;
        std::string title = fullscreen ? "MgVwr" : windowTitle;

        window = std::make_shared<sf::RenderWindow>(mode, title, style, state);
        window->setFramerateLimit(60);

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
        float posX, posY;

        if (experimental) {
            // Experimental layout: reserve map width pixels on LEFT, no right space
            float mapReserved = static_cast<float>(parseSizeValue(mapWindowWidth, windowSize.x));
            float availableWidth = windowWidth - mapReserved;

            // Calculate scale based on available width and full window height
            float scaleX = availableWidth / textureWidth;
            float scaleY = windowHeight / textureHeight;
            scale = std::min(scaleX, scaleY);

            sprite->setScale({scale, scale});

            // Calculate scaled dimensions
            float scaledWidth = textureWidth * scale;
            float scaledHeight = textureHeight * scale;

            // Try to center image horizontally in full window
            posX = (windowWidth - scaledWidth) / 2.0f + scaledWidth / 2.0f;
            posY = (windowHeight - scaledHeight) / 2.0f + scaledHeight / 2.0f;

            // If image overlaps with map area (left side), move it right
            float leftEdgeX = posX - scaledWidth / 2.0f;
            if (leftEdgeX < mapReserved) {
                float shiftNeeded = mapReserved - leftEdgeX;
                posX += shiftNeeded;
            }

            sprite->setPosition({posX, posY});
        } else {
            // Original layout: center image in full window
            float scaleX = windowWidth / textureWidth;
            float scaleY = windowHeight / textureHeight;
            scale = std::min(scaleX, scaleY);

            sprite->setScale({scale, scale});

            float scaledWidth = textureWidth * scale;
            float scaledHeight = textureHeight * scale;
            posX = (windowWidth - scaledWidth) / 2.0f + scaledWidth / 2.0f;
            posY = (windowHeight - scaledHeight) / 2.0f + scaledHeight / 2.0f;

            sprite->setPosition({posX, posY});
        }
    }

    void ensureMetadataForImage(const fs::path &imagePath) {
        // Check if metadata is already complete
        auto imageIt = imageMetadataCache.find(imagePath);
        if (imageIt != imageMetadataCache.end() && imageIt->second.contains("complete") &&
            imageIt->second["complete"] == true) {
            return; // Already complete
        }

        // Initialize with defaults if not exists
        if (imageIt == imageMetadataCache.end()) {
            initializeIncompleteMetadata(imagePath);
        }

        // Try exiftool first
        if (exiftoolAvailable && !g_exiftoolPath.empty()) {
            std::vector<fs::path> singleImage = {imagePath};
            auto results = extractExiftoolData(singleImage);
            if (results.find(imagePath) != results.end()) {
                imageMetadataCache[imagePath] = results[imagePath];
                imageMetadataCache[imagePath]["complete"] = true;
                populateFilterResults(imagePath);
                return;
            }
        }

        // Fallback to manual parsing
        std::vector<fs::path> singleImage = {imagePath};
        auto results = extractImageMetadata(singleImage);
        if (results.find(imagePath) != results.end()) {
            imageMetadataCache[imagePath] = results[imagePath];
            imageMetadataCache[imagePath]["complete"] = true;
            populateFilterResults(imagePath);
        }
    }

    void buildImageList(const fs::path &startDir) {
        allImagePaths.clear();
        allDirectories.clear();
        currentFolder = normalizePath(startDir);

        // Update current watched folder when changing directories
        fs::path newWatchedFolder = findWatchedFolder(currentFolder);
        if (!newWatchedFolder.empty()) {
            if (newWatchedFolder != currentWatchedFolder) {
                log_stdout("DEBUG", "buildImageList: Updating currentWatchedFolder from '",
                           currentWatchedFolder.string(), "' to '", newWatchedFolder.string(), "'");
            }
            currentWatchedFolder = newWatchedFolder;
        } else {
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

        size_t imageFileCount = 0;
        for (const auto &img : dirImages) {
            std::string ext = img.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            for (const auto &suffix : supportedSuffixes) {
                if (ext == suffix) {
                    imageFileCount++;
                    break;
                }
            }
        }

        bool tooManyImages = imageFileCount > 1000;
        bool noExiftoolMany = (!exiftoolAvailable || g_exiftoolPath.empty()) && dirImages.size() > 20;
        deferMetadataCurrentFolder = tooManyImages || noExiftoolMany;
        sortByNameCurrentFolder = deferMetadataCurrentFolder;

        // Initialize incomplete metadata for all images when deferred
        if (deferMetadataCurrentFolder) {
            for (const auto &imagePath : dirImages) {
                initializeIncompleteMetadata(imagePath);
            }
        }

        // Use exiftool in batch mode if available for all images at once
        if (!deferMetadataCurrentFolder && exiftoolAvailable && !g_exiftoolPath.empty() && !dirImages.empty()) {
            log_stdout("DEBUG", "Using exiftool batch mode for ", dirImages.size(), " images");

            auto results = extractExiftoolData(dirImages);

            for (const auto &[imagePath, metadata] : results) {
                imageMetadataCache[imagePath] = metadata;
                imageMetadataCache[imagePath]["complete"] = true;

                const std::string &storedDateTime = getExifString(imagePath, "DateTimeOriginal");
                const auto keywords = getKeywords(imagePath);
                log_stdout("DEBUG", "Batch exiftool - ", imagePath.filename().string(), " DateTime: '", storedDateTime,
                           "' Keywords: '", joinKeywords(keywords), "'");

                // Populate filter results
                populateFilterResults(imagePath);
            }

            log_stdout("DEBUG", "Batch exiftool parsed ", results.size(), " records");
        }

        // Fill in any missing metadata using manual parsing
        if (!deferMetadataCurrentFolder) {
            std::vector<fs::path> missing;
            for (const auto &entry : dirImages) {
                auto imageIt = imageMetadataCache.find(entry);
                if (imageIt == imageMetadataCache.end() || !imageIt->second.contains("complete") ||
                    imageIt->second["complete"] != true) {
                    missing.push_back(entry);
                }
            }

            if (!missing.empty()) {
                log_stdout("DEBUG", "Using manual parsing for ", missing.size(), " images");
                auto results = extractImageMetadata(missing);
                for (const auto &entry : missing) {
                    auto resultIt = results.find(entry);
                    if (resultIt != results.end()) {
                        imageMetadataCache[entry] = resultIt->second;
                        imageMetadataCache[entry]["complete"] = true;
                        populateFilterResults(entry);
                    } else {
                        initializeIncompleteMetadata(entry);
                    }
                }
            }
        }

        // Sort images by shooting date/time if allowed, else/then by filename if datetimes are equal
        std::ranges::sort(dirImages, std::less<>{}, [this](const fs::path &p) {
            std::string name = p.filename().string();
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            std::string date = sortByNameCurrentFolder ? std::string{} : getExifString(p, "DateTimeOriginal");
            return std::tuple{date, name};
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

        float scale;
        float posX, posY;

        if (experimental) {
            // Experimental layout: reserve map width pixels on LEFT, no right space
            float mapReserved = static_cast<float>(parseSizeValue(mapWindowWidth, windowSize.x));
            float availableWidth = windowWidth - mapReserved;

            log_stdout("DEBUG Experimental Layout: windowWidth=", windowWidth, ", mapReserved=", mapReserved,
                       ", availableWidth=", availableWidth);

            if (availableWidth < 0) {
                throw std::runtime_error(
                    "Horizontal pixels left for image < 0: available=" + std::to_string(availableWidth) +
                    ", window=" + std::to_string(windowWidth) + ", mapReserved=" + std::to_string(mapReserved));
            }

            // Calculate scale based on available width and full window height
            float scaleX = availableWidth / textureWidth;
            float scaleY = windowHeight / textureHeight;
            scale = std::min(scaleX, scaleY);

            sprite->setScale({scale, scale});

            // Set rotation origin to center for proper rotation
            auto origSize = texture->getSize();
            sprite->setOrigin({origSize.x / 2.0f, origSize.y / 2.0f});
            sprite->setRotation(sf::degrees(rotation));

            // Calculate scaled dimensions
            float scaledWidth = textureWidth * scale;
            float scaledHeight = textureHeight * scale;

            // Try to center image horizontally in full window
            posX = (windowWidth - scaledWidth) / 2.0f + scaledWidth / 2.0f;
            posY = (windowHeight - scaledHeight) / 2.0f + scaledHeight / 2.0f;

            // If image overlaps with map area (left side), move it right
            float leftEdgeX = posX - scaledWidth / 2.0f;
            if (leftEdgeX < mapReserved) {
                float shiftNeeded = mapReserved - leftEdgeX;
                posX += shiftNeeded;
            }

            sprite->setPosition({posX, posY});
        } else {
            // Original layout: center image in full window
            float scaleX = windowWidth / textureWidth;
            float scaleY = windowHeight / textureHeight;
            scale = std::min(scaleX, scaleY);

            sprite->setScale({scale, scale});

            // Set rotation origin to center for proper rotation
            auto origSize = texture->getSize();
            sprite->setOrigin({origSize.x / 2.0f, origSize.y / 2.0f});
            sprite->setRotation(sf::degrees(rotation));

            float scaledWidth = textureWidth * scale;
            float scaledHeight = textureHeight * scale;
            posX = (windowWidth - scaledWidth) / 2.0f + scaledWidth / 2.0f;
            posY = (windowHeight - scaledHeight) / 2.0f + scaledHeight / 2.0f;

            sprite->setPosition({posX, posY});
        }

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

        // Update map viewer if it's open
        if (mapViewer && mapViewer->isOpen()) {
            if (hasGpsLatitude(imagePath)) {
                double lat = getGpsValueOrZero(imagePath, "GPSLatitude");
                double lon = getGpsValueOrZero(imagePath, "GPSLongitude");

                // Only recenter map if new point is outside the center 50% visible area
                if (!mapViewer->isPointInStayPutArea(lat, lon)) {
                    mapViewer->updateGPS(lat, lon);
                } else {
                    // Inside center 50% - keep map centered but update marker position
                    mapViewer->updateMarkerOnly(lat, lon);
                }
            }

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

        size_t imageFileCount = 0;
        for (const auto &img : dirImages) {
            std::string ext = img.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            for (const auto &suffix : supportedSuffixes) {
                if (ext == suffix) {
                    imageFileCount++;
                    break;
                }
            }
        }

        bool tooManyImages = imageFileCount > 1000;
        bool noExiftoolMany = (!exiftoolAvailable || g_exiftoolPath.empty()) && dirImages.size() > 20;
        bool deferMetadata = tooManyImages || noExiftoolMany;

        // Use exiftool in batch mode if available for all images at once
        ImageMetadataCache imageMetadata;

        // Helper to initialize incomplete metadata for deferred mode
        auto initializeLocalIncomplete = [&imageMetadata](const fs::path &imagePath) {
            json metaObject = json::object();
            metaObject["SourceFile"] = imagePath.string();

            // Enrich with schema defaults
            try {
                json enriched = enrichMetadataWithSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, metaObject);
                imageMetadata[imagePath] = enriched;
            } catch (const std::exception &e) {
                // If enrichment fails, fall back to basic object
                imageMetadata[imagePath] = metaObject;
            }

            // Mark as incomplete
            imageMetadata[imagePath]["complete"] = false;
        };

        // Initialize incomplete metadata for all images when deferred
        if (deferMetadata) {
            for (const auto &imagePath : dirImages) {
                initializeLocalIncomplete(imagePath);
            }
        }

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
            // Schema guarantees Keywords is always an array of strings
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

        if (!deferMetadata && exiftoolAvailable && !g_exiftoolPath.empty() && !dirImages.empty()) {
            log_stdout("DEBUG", "Pre-caching: using exiftool batch mode for ", dirImages.size(), " images");

            auto results = extractExiftoolData(dirImages);

            for (const auto &[imagePath, metadata] : results) {
                imageMetadata[imagePath] = metadata;
                imageMetadata[imagePath]["complete"] = true;
                // Populate filter results
                populateLocalFilterResults(imagePath);
            }

            log_stdout("DEBUG", "Pre-caching: extracted metadata for ", results.size(), " images");
        }

        // Fill in any missing metadata using manual parsing
        if (!deferMetadata) {
            std::vector<fs::path> missing;
            for (const auto &entry : dirImages) {
                auto imageIt = imageMetadata.find(entry);
                if (imageIt == imageMetadata.end() || !imageIt->second.contains("complete") ||
                    imageIt->second["complete"] != true) {
                    missing.push_back(entry);
                }
            }

            if (!missing.empty()) {
                auto results = extractImageMetadata(missing);
                for (const auto &entry : missing) {
                    auto resultIt = results.find(entry);
                    if (resultIt != results.end()) {
                        imageMetadata[entry] = resultIt->second;
                        imageMetadata[entry]["complete"] = true;
                        populateLocalFilterResults(entry);
                    } else {
                        initializeLocalIncomplete(entry);
                    }
                }
            }
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
                return getLocalExif(a, "DateTimeOriginal") < getLocalExif(b, "DateTimeOriginal");
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
            navigationMessage = "Jumped to of prev " + navigationType + " (last of folder)";
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
            navigationMessage = "Jumped to of prev " + navigationType + " (1st of folder)";
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
        auto [found, path] = findExiftool();
        g_exiftoolPath = path;
        exiftoolAvailable = found && !g_exiftoolPath.empty();

        // Initialize map viewer with cache configuration
        if (cacheEnabled) {
            // Use default cache location if not specified
            if (cacheLocation.empty()) {
                cacheLocation = getDefaultCacheLocation().string();
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

        // Find oldest image in the current directory
        fs::path argFileDir = absPath.parent_path();
        size_t oldestIndex = 0;
        std::string oldestDateTime = "9999:99:99 99:99:99";
        bool foundOldest = false;

        for (size_t i = 0; i < allImagePaths.size(); i++) {
            if (allDirectories[i] == argFileDir) {
                const std::string &dateTime = getExifString(allImagePaths[i], "DateTimeOriginal");
                if (dateTime < oldestDateTime) {
                    oldestDateTime = dateTime;
                    oldestIndex = i;
                    foundOldest = true;
                }
            }
        }

        currentIndex = oldestIndex;
        jumpedToOldest = (allImagePaths[currentIndex] != absPath) && foundOldest;

        if (jumpedToOldest) {
            log_stdout("Jumped to oldest image: ", allImagePaths[currentIndex].filename());
        }

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
                        auto windowSize = window->getSize();
                        auto mapSize = mapTexture->getSize();
                        float mapX = 0.f;
                        float mapY = (windowSize.y - mapSize.y) / 2.0f;

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
                    handleKeyPress(*keyEvent);
                } else if (const auto *mouseEvent = event->getIf<sf::Event::MouseButtonPressed>()) {
                    if (mouseEvent->button == sf::Mouse::Button::Left) {
                        sf::Vector2f clickPos(mouseEvent->position.x, mouseEvent->position.y);
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
                                            if (mapIdx == -1) {
                                                // Toggle map visibility
                                                if (mapViewer) {
                                                    if (mapViewer->isOpen()) {
                                                        mapViewer->close();
                                                        log_stdout("Closing map");
                                                    } else {
                                                        mapViewer->showMap(lat, lon, defaultZoom);
                                                        log_stdout("Opening map for ", lat, ", ", lon);
                                                    }
                                                }
                                            } else if (mapIdx >= 0 && mapIdx < static_cast<int>(maps.size())) {
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
                            float mapX = windowSize.x - mapSize.x;
                            float mapY = (windowSize.y - mapSize.y) / 2.0f;

                            // Check if mouse is over map area - if it is, don't process wheel for images
                            sf::Vector2i mousePos = wheelEvent->position;
                            if (mousePos.x >= mapX && mousePos.x < mapX + mapSize.x && mousePos.y >= mapY &&
                                mousePos.y < mapY + mapSize.y) {
                                shouldProcessWheel = false;
                            }
                        }
                    }

                    if (shouldProcessWheel) {
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
                }
            }

            // Process any queued key presses after pre-caching completes
            processPendingKeyPresses();

            // Update cursor based on hover over any map link or navigation arrow
            auto mousePos = sf::Vector2f(sf::Mouse::getPosition(*window));
            bool mouseOverLink = false;
            for (const auto &[mapIdx, clickArea] : mapLinkAreas) {
                if (clickArea.contains(mousePos)) {
                    mouseOverLink = true;
                    break;
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

            if (quietMode) {
                mapLinkAreas.clear();
                navArrowAreas.clear();
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
            if (sprite) {
                window->draw(*sprite);
            }

            // Draw inline map if experimental mode and map is shown
            if (experimental && mapViewer && mapViewer->isOpen()) {
                const sf::Texture *mapTexture = mapViewer->getTexture();
                if (mapTexture) {
                    sf::Sprite mapSprite(*mapTexture);
                    auto windowSize = window->getSize();
                    auto mapSize = mapTexture->getSize();
                    // Position on left side, vertically centered
                    float mapX = 0.f;
                    float mapY = (windowSize.y - mapSize.y) / 2.0f;
                    mapSprite.setPosition(sf::Vector2f(mapX, mapY));
                    window->draw(mapSprite);
                }
            }

            if (!quietMode) {
                drawTopLeftInfo();
                drawFilterInfo();
                drawMapInfo();

                // Draw navigation message if available
                if (!navigationMessage.empty()) {
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

            window->display();

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
            sf::Vector2f boxPos(15.0f, window->getSize().y - boxSize.y - 15.0f);

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
            // Position at bottom left, leave space for 2 lines of navigation text below (yellow text)
            float reservedSpaceBelow = lineSpacing * 2 + padding * 2 + 15.0f;
            sf::Vector2f boxPos(15.0f, window->getSize().y - boxSize.y - reservedSpaceBelow);

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
        if (navigationMessage.empty())
            return;

        if (uiFontLoaded) {
            sf::Text text(uiFont, navigationMessage, getCalculatedFontSize());

            // Use red for "Reached" messages (boundary warnings), yellow for others
            text.setFillColor(navigationMessage.find("Reached") == 0 ? sf::Color::Red : sf::Color::Yellow);

            sf::FloatRect bounds = text.getLocalBounds();
            const float paddingX = 12.0f;
            const float paddingY = 8.0f;
            sf::Vector2f boxSize(bounds.size.x + paddingX * 2.0f, bounds.size.y + paddingY * 2.0f);
            sf::Vector2f boxPos(15.0f, window->getSize().y - boxSize.y - 15.0f);

            sf::RectangleShape background(boxSize);
            background.setPosition(boxPos);
            background.setFillColor(sf::Color::Black);
            window->draw(background);

            text.setPosition({boxPos.x + paddingX - bounds.position.x, boxPos.y + paddingY - bounds.position.y});
            window->draw(text);
            return;
        }
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

        // Draw coordinates and zoom above map on left side
        char coordStr[64];
        snprintf(coordStr, sizeof(coordStr), "%.4f, %.4f z%d", mapViewer->getCenterLat(), mapViewer->getCenterLon(),
                 mapViewer->getCurrentZoom());

        sf::Text coordText(uiFont, coordStr);
        coordText.setFillColor(sf::Color::White);
        coordText.setCharacterSize(getCalculatedFontSize());

        auto windowSize = window->getSize();
        const sf::Texture *mapTexture = mapViewer->getTexture();
        if (mapTexture) {
            auto mapSize = mapTexture->getSize();
            float mapX = 0.f;
            float mapY = (windowSize.y - mapSize.y) / 2.0f;

            sf::FloatRect textBounds = coordText.getLocalBounds();
            // Position above map, left aligned with padding and blank line
            coordText.setPosition(sf::Vector2f(mapX + 5.f, mapY - textBounds.size.y - getCalculatedFontSize() - 15.f));
            window->draw(coordText);
        }

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
                float mapY = (windowSize.y - mapSize.y) / 2.0f;

                sf::FloatRect loadingBounds = loadingText.getLocalBounds();
                loadingText.setPosition(sf::Vector2f(mapX + 5.f, mapY + mapSize.y + 5.f));
                window->draw(loadingText);
            }
        }
    }

    void drawTopLeftInfo() {
        if (allImagePaths.empty())
            return;
        const auto &imagePath = allImagePaths[currentIndex];

        std::vector<std::string> lines;

        // Calculate index line - adjust for active filter if present
        std::string indexLine;
        if (activeFilterIndex >= 0 && activeFilterIndex < static_cast<int>(filters.size())) {
            // Filter is active - count filtered images and find current position
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

            indexLine = std::to_string(currentFilteredPosition) + "/" + std::to_string(filteredCount);
        } else {
            // No filter active - show total count
            indexLine = std::to_string(currentIndex + 1) + "/" + std::to_string(allImagePaths.size());
        }

        if (sortByNameCurrentFolder) {
            indexLine += " by name";
        }
        lines.push_back(indexLine);
        lines.push_back("");
        const std::string installText = "Install Exiftool";
        // Dynamic map text: "Hide map" when map is open, "Show on map" when closed
        const std::string showOnMapText = (mapViewer && mapViewer->isOpen()) ? "Hide map" : "Show on map";
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

        // Add embedded map link if GPS coordinates are available
        bool hasValidGps = hasGpsLatitude(imagePath);
        if (hasValidGps) {
            lines.push_back("");
            lines.push_back(showOnMapText);
        }

        // Add map links if GPS coordinates are available
        if (hasValidGps && !maps.empty()) {
            lines.push_back(""); // Blank line before maps
            for (const auto &m : maps) {
                lines.push_back(m.name);
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

        // Clear previous map link areas and navigation arrow areas
        mapLinkAreas.clear();
        navArrowAreas.clear();

        if (uiFontLoaded) {
            for (size_t i = 0; i < lines.size(); i++) {
                // Check if this line is a map link and find which map it is
                int mapIndex = -2;
                if (lines[i] == showOnMapText) {
                    mapIndex = -1;
                } else {
                    for (size_t m = 0; m < maps.size(); m++) {
                        if (lines[i] == maps[m].name) {
                            mapIndex = static_cast<int>(m);
                            break;
                        }
                    }
                }
                bool isMapLink = (mapIndex >= -1);

                sf::String sfLine = sf::String::fromUtf8(lines[i].begin(), lines[i].end());
                sf::Text text(uiFont, sfLine, fontSize);
                sf::Color textColor =
                    isMapLink ? mapColor
                              : (lines[i] == installText ? sf::Color::Red
                                                         : (lines[i] == indexLine ? sf::Color::White : infoColor));
                text.setFillColor(textColor);
                float yPos = startY + static_cast<float>(i) * lineSpacing;
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
                return;
            }

            std::lock_guard<std::mutex> lock(keyQueueMutex);
            // Clear any queued keys
            while (!pendingKeyPresses.empty())
                pendingKeyPresses.pop();
            window->close();
            return;
        }

        // If help is showing, don't process other keys
        if (showHelp) {
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

        switch (key.code) {
        case sf::Keyboard::Key::Num1:
            if (key.shift) { // ! key (Shift+1)
                quietMode = !quietMode;
                if (quietMode) {
                    mapLinkAreas.clear();
                }
            }
            break;

        case sf::Keyboard::Key::Right:
            loadImage(getNextInFolder());
            break;

        case sf::Keyboard::Key::PageDown:
            loadImage(getFirstInNextFolder());
            break;

        case sf::Keyboard::Key::Left:
            loadImage(getPrevInFolder());
            break;

        case sf::Keyboard::Key::PageUp:
            loadImage(getFirstInPrevFolder());
            break;

        case sf::Keyboard::Key::Home:
            loadImage(getFirstInFolder());
            break;

        case sf::Keyboard::Key::End:
            loadImage(getLastInFolder());
            break;

        case sf::Keyboard::Key::Enter:
            toggleWindowMode();
            break;

        case sf::Keyboard::Key::W:
            // Alt+F4 is handled by OS, Ctrl+W would need modifiers
            if (key.control) {
                window->close();
            }
            break;

        case sf::Keyboard::Key::C:
            if (key.control && !allImagePaths.empty()) {
                std::string fullPath = toUtf8String(allImagePaths[currentIndex]);
                sf::String clip = sf::String::fromUtf8(fullPath.begin(), fullPath.end());
                sf::Clipboard::setString(clip);
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

// Recursively find all image files in a directory or return single file
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

// Calculate tiles needed for a GPS coordinate at a given zoom and window size
std::vector<MapViewer::TileCoord> calculateTilesForView(double latitude, double longitude, int zoom, int windowWidth,
                                                        int windowHeight) {
    std::vector<MapViewer::TileCoord> tiles;

    // Use the same logic as MapViewer::updateTileLoadingState
    const int TILE_SIZE = 256;

    // Convert GPS to pixel coordinates
    double n = std::pow(2.0, zoom);
    double centerPixX = (longitude + 180.0) / 360.0 * n * TILE_SIZE;
    double latRad = latitude * M_PI / 180.0;
    double centerPixY = (1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * n * TILE_SIZE;

    // Calculate visible tile range
    int screenCenterX = windowWidth / 2;
    int screenCenterY = windowHeight / 2;

    int minTileX = static_cast<int>((centerPixX - screenCenterX) / TILE_SIZE) - 1;
    int maxTileX = static_cast<int>((centerPixX + screenCenterX) / TILE_SIZE) + 1;
    int minTileY = static_cast<int>((centerPixY - screenCenterY) / TILE_SIZE) - 1;
    int maxTileY = static_cast<int>((centerPixY + screenCenterY) / TILE_SIZE) + 1;

    // Clamp to valid tile range
    int maxTileIndex = (1 << zoom) - 1;
    minTileX = std::max(0, minTileX);
    maxTileX = std::min(maxTileIndex, maxTileX);
    minTileY = std::max(0, minTileY);
    maxTileY = std::min(maxTileIndex, maxTileY);

    // Generate tile coordinates
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

// Main function for --cache-osm mode
int runCacheOsmMode(const std::vector<std::string> &paths, const std::string &configPath) {
    log_stdout("OSM tile caching mode");

    // Load configuration using schema validation
    std::string cacheDir;
    int defaultZoom = 15;
    unsigned int windowWidth = 1024;
    unsigned int windowHeight = 768;

    std::vector<std::string> supportedSuffixes;

    try {
        // Load and validate config using schema-driven system (structure guaranteed by validation)
        fs::path configDir = fs::path(configPath).parent_path();
        json config = loadAndValidateConfig(configDir);

        // Extract map cache location (schema-validated, safe to access directly)
        std::string location = config["map"]["cache"]["location"];
        if (!location.empty()) {
            cacheDir = location;
        }

        // Extract zoom config
        defaultZoom = config["map"]["viewer"]["zoom"]["default"];

        // Parse window size (handles both string percentages and integer pixels)
        sf::VideoMode desktopMode = sf::VideoMode::getDesktopMode();
        const auto &sizeArray = config["map"]["viewer"]["window"]["size"];
        windowWidth = parseSizeValue(sizeArray[0], desktopMode.size.x);
        windowHeight = parseSizeValue(sizeArray[1], desktopMode.size.y);

        // Extract supported image suffixes
        for (const auto &suffix : config["image_file"]["supported_suffixes"]) {
            supportedSuffixes.push_back(suffix.get<std::string>());
        }
    } catch (const std::exception &e) {
        log_stderr("Error loading config: ", e.what());
        throw;
    }

    // Use same default cache location as GUI app if not specified
    if (cacheDir.empty()) {
        fs::path baseCacheLocation = getDefaultCacheLocation();
        // Use different fallback for --cache-osm mode (osm_cache) vs GUI app (./cache/osm)
        if (baseCacheLocation == fs::path(".") / "cache") {
            cacheDir = "osm_cache"; // Backward compatibility for local directory fallback
        } else {
            cacheDir = (baseCacheLocation / "osm").string();
        }
    }

    log_stdout("Cache directory (config): ", cacheDir);

    // Cache larger area to handle panning (3x window size in each direction)
    const int expandedWidth = windowWidth * 3;
    const int expandedHeight = windowHeight * 3;
    log_stdout("Default view: ", windowWidth, "x", windowHeight, " at zoom ", defaultZoom);
    log_stdout("Expanded cache area: ", expandedWidth, "x", expandedHeight, " (to handle panning)");

    // Ensure cache directory exists
    fs::create_directories(cacheDir);
    log_stdout("Cache directory (resolved): ", fs::absolute(cacheDir).string());

    // Find exiftool using existing function
    auto [exiftoolFound, exiftoolPath] = findExiftool();
    g_exiftoolPath = exiftoolPath;
    if (!exiftoolFound || g_exiftoolPath.empty()) {
        log_stderr("Error: exiftool not found. Please install exiftool.");
        return 1;
    }

    log_stdout("Using exiftool: ", g_exiftoolPath);

    // Collect all image files from input paths
    std::vector<fs::path> allFiles;
    for (const auto &pathStr : paths) {
        fs::path path(pathStr);
        log_stdout("Scanning: ", path);

        auto files = findImageFiles(path, supportedSuffixes);
        log_stdout("Found ", files.size(), " image file(s)");

        allFiles.insert(allFiles.end(), files.begin(), files.end());
    }

    if (allFiles.empty()) {
        log_stderr("No image files found");
        return 1;
    }

    log_stdout("Total image files: ", allFiles.size());

    // Extract metadata in batches (to avoid command line length limits)
    const size_t batchSize = 50;
    ImageMetadataCache allMetadata;

    for (size_t i = 0; i < allFiles.size(); i += batchSize) {
        size_t end = std::min(i + batchSize, allFiles.size());
        std::vector<fs::path> batch(allFiles.begin() + i, allFiles.begin() + end);

        log_stdout("Extracting metadata from files ", (i + 1), "-", end, "...");
        auto batchMetadata = extractExiftoolData(batch);
        allMetadata.insert(batchMetadata.begin(), batchMetadata.end());
    }

    auto hasValidGps = [](const json &meta) {
        return meta.contains("GPSLatitude") && meta["GPSLatitude"].is_number() && meta.contains("GPSLongitude") &&
               meta["GPSLongitude"].is_number();
    };

    // Count files with valid GPS
    int validGPSCount = 0;
    for (const auto &entry : allMetadata) {
        if (hasValidGps(entry.second)) {
            validGPSCount++;
        }
    }

    log_stdout("Files with valid GPS: ", validGPSCount, "/", allFiles.size());

    if (validGPSCount == 0) {
        log_stderr("No files with GPS coordinates found");
        return 1;
    }

    // Calculate all unique tiles needed (using expanded area for panning)
    std::set<std::tuple<int, int, int>> uniqueTiles; // <x, y, zoom>

    for (const auto &file : allFiles) {
        auto metaIt = allMetadata.find(file);
        if (metaIt != allMetadata.end() && hasValidGps(metaIt->second)) {
            const json &meta = metaIt->second;
            double lat = meta["GPSLatitude"].get<double>();
            double lon = meta["GPSLongitude"].get<double>();
            auto tiles = calculateTilesForView(lat, lon, defaultZoom, expandedWidth, expandedHeight);

            for (const auto &tile : tiles) {
                uniqueTiles.insert(std::make_tuple(tile.x, tile.y, tile.zoom));
            }
        }
    }

    log_stdout("Total unique tiles needed: ", uniqueTiles.size());

    // Download missing tiles with rate limiting (2 tiles/sec)
    const int TILE_SIZE = 256;
    const char *TILE_SERVER_HOST = "tile.openstreetmap.org";
    const char *USER_AGENT = "mgvwr/1.0";

    int downloadedCount = 0;
    int skippedCount = 0;
    auto lastDownloadTime = std::chrono::steady_clock::now();

    for (const auto &tileCoord : uniqueTiles) {
        int x = std::get<0>(tileCoord);
        int y = std::get<1>(tileCoord);
        int zoom = std::get<2>(tileCoord);

        // Build tile path
        fs::path tilePath =
            fs::path(cacheDir) / std::to_string(zoom) / std::to_string(x) / (std::to_string(y) + ".png");

        // Skip if already cached
        if (fs::exists(tilePath) && fs::file_size(tilePath) > 0) {
            skippedCount++;
            continue;
        }

        // Rate limiting: ensure at least 500ms (2 tiles/sec) between downloads
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDownloadTime);
        if (elapsed.count() < 250) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250 - elapsed.count()));
        }

        // Download tile
        std::string url = "https://" + std::string(TILE_SERVER_HOST) + "/" + std::to_string(zoom) + "/" +
                          std::to_string(x) + "/" + std::to_string(y) + ".png";

        fs::create_directories(tilePath.parent_path());

        std::string command =
            "curl -s -f -L -A \"" + std::string(USER_AGENT) + "\" -o \"" + tilePath.string() + "\" \"" + url + "\"";

        int result = system(command.c_str());
        lastDownloadTime = std::chrono::steady_clock::now();

        if (result == 0 && fs::exists(tilePath) && fs::file_size(tilePath) > 0) {
            downloadedCount++;
            log_stdout("Downloaded (", downloadedCount, "/", (uniqueTiles.size() - skippedCount), "): ", zoom, "/", x,
                       "/", y);
        } else {
            if (fs::exists(tilePath)) {
                fs::remove(tilePath);
            }
            log_stderr("Failed to download: ", zoom, "/", x, "/", y);
        }
    }

    log_stdout("Caching complete!");
    log_stdout("Downloaded: ", downloadedCount, " tiles");
    log_stdout("Already cached: ", skippedCount, " tiles");

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

// Simple argument parsing for our specific modes
int main(int argc, char *argv[]) {
#ifdef _WIN32
    // Set Windows console to UTF-8 mode for proper Unicode output
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // Validate built-in schemas on startup
    try {
        validateBuiltInSchemas();
        validateSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, "EXIFTOOL_RESPONSE_SCHEMA_YAML");
    } catch (const std::exception &e) {
        log_stderr("Fatal: Schema validation failed: ", e.what());
        return 1;
    }

    // Log CLI command
    std::string cliLine = "CLI:";
    for (int i = 0; i < argc; ++i) {
        cliLine += " " + std::string(argv[i]);
    }
    log_stdout(cliLine);

    // Parse command line arguments
    std::string configFile; // Default will be set by loadConfig if empty
    std::string imageFile;
    std::string mode; // "", "self-check", "cache-osm", "exiftool", "poor"
    std::vector<std::string> cachePaths;

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
        } else if (arg == "--cache-osm") {
            mode = "cache-osm";
            // Collect remaining arguments as paths
            for (int j = i + 1; j < argc; ++j) {
                cachePaths.push_back(argv[j]);
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
            // Positional argument
            imageFile = arg;
        }
    }

    // Handle self-check mode
    if (mode == "self-check") {
        return runSelfCheck(argv[0], configFile);
    }

    // Handle cache-osm mode
    if (mode == "cache-osm") {
        if (cachePaths.empty()) {
            log_stderr("Error: --cache-osm requires at least one path");
            return 1;
        }
        return runCacheOsmMode(cachePaths, configFile);
    }

    // Handle exiftool mode
    if (mode == "exiftool") {
        auto [found, path] = findExiftool();
        g_exiftoolPath = path;
        if (!found || g_exiftoolPath.empty()) {
            log_stderr("Error: exiftool not found. Please install exiftool.");
            return 1;
        }
        const char *newArgv[] = {argv[0], "--exiftool", imageFile.c_str()};
        return show_yaml_metadata(extractExiftoolData, 3, (char **)newArgv, "--exiftool");
    }

    // Handle poor mode
    if (mode == "poor") {
        const char *newArgv[] = {argv[0], "--poor", imageFile.c_str()};
        return show_yaml_metadata(extractImageMetadata, 3, (char **)newArgv, "--poor");
    }

    // Normal viewer mode
    if (imageFile.empty()) {
        log_stderr("Error: Image file required");
        log_stderr("");
        log_stderr("Usage: ", argv[0], " <image_file>");
        log_stderr("   or: ", argv[0], " --config <file> <image_file>");
        log_stderr("   or: ", argv[0], " --self-check [--config <file>]");
        log_stderr("   or: ", argv[0], " --cache-osm <path> [<path> ...]");
        log_stderr("   or: ", argv[0], " --exiftool <image_file>");
        log_stderr("   or: ", argv[0], " --poor <image_file>");
        log_stderr("");
        log_stderr("Supported formats: JPG, PNG, BMP, GIF, TIFF");
        return 1;
    }

    try {
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
