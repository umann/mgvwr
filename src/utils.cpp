#include "utils.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string formatLogPrefixTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto nowTime = std::chrono::system_clock::to_time_t(now);

    std::tm localTm = *std::localtime(&nowTime);
    std::tm utcTm = *std::gmtime(&nowTime);

    auto localEpoch = std::mktime(&localTm);
    auto utcEpoch = std::mktime(&utcTm);
    long offsetSeconds = static_cast<long>(std::difftime(localEpoch, utcEpoch));

    char sign = '+';
    if (offsetSeconds < 0) {
        sign = '-';
        offsetSeconds = -offsetSeconds;
    }

    int offsetHours = static_cast<int>(offsetSeconds / 3600);
    int offsetMinutes = static_cast<int>((offsetSeconds % 3600) / 60);

    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S") << sign << std::setw(2) << std::setfill('0') << offsetHours
        << ":" << std::setw(2) << std::setfill('0') << offsetMinutes;
    return oss.str();
}

std::string formatLogFileTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTm = *std::localtime(&nowTime);

    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y%m%d_%H%M%S");
    return oss.str();
}

fs::path getDefaultLogDirectory() {
#ifdef _WIN32
    const char *localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && *localAppData) {
        return fs::path(localAppData) / "Umann" / "MgVwr" / "logs";
    }
#endif
    return fs::current_path() / "logs";
}

class TimestampPrefixTeeBuf : public std::streambuf {
  public:
    TimestampPrefixTeeBuf(std::streambuf *primaryBuf, std::ostream *fileStream, std::mutex *fileMutex)
        : primary(primaryBuf), file(fileStream), writeMutex(fileMutex), atLineStart(true) {}

  protected:
    int overflow(int ch) override {
        if (ch == traits_type::eof()) {
            return traits_type::not_eof(ch);
        }

        if (primary && primary->sputc(static_cast<char>(ch)) == traits_type::eof()) {
            return traits_type::eof();
        }

        if (file && file->good()) {
            std::lock_guard<std::mutex> lock(*writeMutex);
            char c = static_cast<char>(ch);
            if (atLineStart && c != '\n') {
                (*file) << formatLogPrefixTimestamp() << " ";
                atLineStart = false;
            }
            file->put(c);
            if (c == '\n') {
                atLineStart = true;
            }
            file->flush();
        }

        return ch;
    }

    std::streamsize xsputn(const char *s, std::streamsize count) override {
        std::streamsize written = 0;
        for (; written < count; ++written) {
            if (overflow(static_cast<unsigned char>(s[written])) == traits_type::eof()) {
                break;
            }
        }
        return written;
    }

    int sync() override {
        if (primary && primary->pubsync() == -1) {
            return -1;
        }
        if (file) {
            std::lock_guard<std::mutex> lock(*writeMutex);
            file->flush();
        }
        return 0;
    }

  private:
    std::streambuf *primary;
    std::ostream *file;
    std::mutex *writeMutex;
    bool atLineStart;
};

std::ofstream g_logFile;
std::unique_ptr<TimestampPrefixTeeBuf> g_coutTee;
std::unique_ptr<TimestampPrefixTeeBuf> g_cerrTee;
std::streambuf *g_originalCout = nullptr;
std::streambuf *g_originalCerr = nullptr;
bool g_loggingInitialized = false;
std::mutex g_logFileWriteMutex;

void shutdownAppLogging() {
    if (!g_loggingInitialized) {
        return;
    }

    if (g_originalCout) {
        std::cout.rdbuf(g_originalCout);
    }
    if (g_originalCerr) {
        std::cerr.rdbuf(g_originalCerr);
    }

    g_coutTee.reset();
    g_cerrTee.reset();

    if (g_logFile.is_open()) {
        g_logFile.flush();
        g_logFile.close();
    }

    g_loggingInitialized = false;
}

void pruneOldLogs(const fs::path &logDir, int retentionDays) {
    if (retentionDays < 0) {
        retentionDays = 0;
    }

    std::error_code ec;
    if (!fs::exists(logDir, ec) || !fs::is_directory(logDir, ec)) {
        return;
    }

    auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * retentionDays);
    std::regex namePattern(R"(^mrvwr_\d{8}_\d{6}\.log$)");

    for (const auto &entry : fs::directory_iterator(logDir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }

        const std::string fileName = entry.path().filename().string();
        if (!std::regex_match(fileName, namePattern)) {
            continue;
        }

        std::error_code timeEc;
        auto writeTime = fs::last_write_time(entry.path(), timeEc);
        if (timeEc) {
            continue;
        }

        if (writeTime < cutoff) {
            std::error_code removeEc;
            fs::remove(entry.path(), removeEc);
        }
    }
}

} // namespace

// Convert path to display string with single backslashes on Windows
std::string pathToString(const fs::path &path) {
    std::string str = path.string();
#ifdef _WIN32
    // Replace double backslashes with single backslashes for display
    std::string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        result += str[i];
        if (str[i] == '\\' && i + 1 < str.size() && str[i + 1] == '\\') {
            ++i; // Skip the second backslash
        }
    }
    return result;
#else
    return str;
#endif
}

// Normalize path: convert drive letter to uppercase on Windows
fs::path normalizePath(const fs::path &path) {
    std::string pathStr = path.string();
#ifdef _WIN32
    // Convert drive letter to uppercase (e.g., f:\ -> F:\)
    if (pathStr.size() >= 2 && pathStr[1] == ':') {
        pathStr[0] = std::toupper(pathStr[0]);
    }
#endif
    return fs::path(pathStr);
}

// Get current timestamp as YYYY-MM-DD HH:MM:SS
std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

bool initialize_app_logging(int retentionDays) {
    if (g_loggingInitialized) {
        apply_log_retention_policy(retentionDays);
        return true;
    }

    fs::path logDir = getDefaultLogDirectory();
    std::error_code ec;
    fs::create_directories(logDir, ec);
    if (ec) {
        return false;
    }

    fs::path logFilePath = logDir / ("mrvwr_" + formatLogFileTimestamp() + ".log");
    g_logFile.open(logFilePath, std::ios::out | std::ios::app);
    if (!g_logFile.is_open()) {
        return false;
    }

    g_originalCout = std::cout.rdbuf();
    g_originalCerr = std::cerr.rdbuf();
    g_coutTee = std::make_unique<TimestampPrefixTeeBuf>(g_originalCout, &g_logFile, &g_logFileWriteMutex);
    g_cerrTee = std::make_unique<TimestampPrefixTeeBuf>(g_originalCerr, &g_logFile, &g_logFileWriteMutex);
    std::cout.rdbuf(g_coutTee.get());
    std::cerr.rdbuf(g_cerrTee.get());

    g_loggingInitialized = true;
    std::atexit(shutdownAppLogging);
    apply_log_retention_policy(retentionDays);
    return true;
}

void apply_log_retention_policy(int retentionDays) {
    pruneOldLogs(getDefaultLogDirectory(), retentionDays);
}

// Get current OS name ("windows", "macos", or "linux")
std::string getOs() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// Split keywords string into individual keywords
std::vector<std::string> splitKeywords(const std::string &raw) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : raw) {
        if (ch == ';' || ch == ',' || ch == '\n' || ch == '\r') {
            std::string trimmed = trimWhitespace(current);
            if (!trimmed.empty()) {
                result.push_back(trimmed);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    std::string trimmed = trimWhitespace(current);
    if (!trimmed.empty()) {
        result.push_back(trimmed);
    }
    return result;
}

// Join keywords vector into semicolon-separated string
std::string joinKeywords(const std::vector<std::string> &keywords) {
    std::string result;
    for (size_t i = 0; i < keywords.size(); ++i) {
        if (i > 0) {
            result += "; ";
        }
        result += keywords[i];
    }
    return result;
}

// Smart join: concatenate stringified args with intelligent spacing
// No space if left ends with "(", "{", "[", "\"", "'"
// or right begins with ")", "}", "]", ".", ",", ";", "?", "!", "\"", "'"
std::string smart_join_impl(const std::vector<std::string> &parts) {
    std::string result;

    // Join with smart spacing
    static const std::string leftNoSpaceChars = "({['\"";
    static const std::string rightNoSpaceChars = ")}].,;?!'\"";

    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            // Check if we should add space between parts[i-1] and parts[i]
            bool leftEndsWithNoSpace =
                !parts[i - 1].empty() && leftNoSpaceChars.find(parts[i - 1].back()) != std::string::npos;
            bool rightStartsWithNoSpace =
                !parts[i].empty() && rightNoSpaceChars.find(parts[i][0]) != std::string::npos;

            if (!leftEndsWithNoSpace && !rightStartsWithNoSpace) {
                result += " ";
            }
        }
        result += parts[i];
    }

    return result;
}

// Log message to stdout with timestamp
void log_stdout_impl(const std::string &message) {
    std::cout << "[" << getTimestamp() << "] " << message << "\n";
}

// Log message to stderr with timestamp
void log_stderr_impl(const std::string &message) {
    std::cerr << "[" << getTimestamp() << "] " << message << "\n";
}

// Convert filesystem path to UTF-8 string
std::string toUtf8String(const fs::path &path) {
#if defined(_WIN32)
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.u8string();
#endif
}

// Trim trailing slashes from path string
std::string trimTrailingSlash(std::string path) {
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    return path;
}

// Expand glob patterns to matching paths
std::vector<fs::path> expandGlob(const std::string &pattern) {
    std::vector<fs::path> results;

    // Check if pattern contains wildcards
    if (pattern.find('*') == std::string::npos && pattern.find('?') == std::string::npos) {
        // No wildcards, return as-is if it exists
        fs::path p(pattern);
        if (fs::exists(p)) {
            results.push_back(p);
        }
        return results;
    }

    // Find the base directory (part before first wildcard)
    std::string baseDir;
    size_t wildcardPos = pattern.find_first_of("*?");
    if (wildcardPos == std::string::npos) {
        baseDir = pattern;
    } else {
        // Find the last path separator before the wildcard
        size_t lastSep = pattern.rfind(fs::path::preferred_separator, wildcardPos);
#ifdef _WIN32
        if (lastSep == std::string::npos) {
            lastSep = pattern.rfind('/', wildcardPos);
        }
#endif
        if (lastSep != std::string::npos) {
            baseDir = pattern.substr(0, lastSep);
        } else {
            baseDir = ".";
        }
    }

    // Convert pattern to regex
    auto escapeRegex = [](const std::string &input) {
        static const std::regex specialChars(R"([.^$|()\\[\]{}+])");
        return std::regex_replace(input, specialChars, R"(\\$&)");
    };
    std::string regexPattern = escapeRegex(pattern);
    // Replace * with .* and ? with .
    std::string::size_type pos = 0;
    while ((pos = regexPattern.find('*', pos)) != std::string::npos) {
        regexPattern.replace(pos, 1, ".*");
        pos += 2;
    }
    pos = 0;
    while ((pos = regexPattern.find('?', pos)) != std::string::npos) {
        regexPattern.replace(pos, 1, ".");
        pos += 1;
    }

    std::regex regex_pattern("^" + regexPattern + "$");

    // Search in base directory
    try {
        if (fs::is_directory(baseDir)) {
            for (const auto &entry :
                 fs::recursive_directory_iterator(baseDir, fs::directory_options::skip_permission_denied)) {
                std::string pathStr = entry.path().string();
                // Normalize path separators for comparison
#ifdef _WIN32
                for (char &c : pathStr) {
                    if (c == '/')
                        c = '\\';
                }
#endif
                if (std::regex_match(pathStr, regex_pattern)) {
                    results.push_back(entry.path());
                }
            }
        }
    } catch (...) {
        // Ignore errors during directory iteration
    }

    return results;
}
