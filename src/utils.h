#pragma once

#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Convert path to display string with single backslashes on Windows
std::string pathToString(const fs::path &path);

// Normalize path: convert drive letter to uppercase on Windows
fs::path normalizePath(const fs::path &path);

// Get current timestamp as YYYY-MM-DD HH:MM:SS
std::string getTimestamp();

// Get current OS name ("windows", "macos", or "linux")
std::string getOs();

// Trim whitespace from string (inline so it's available to templates)
inline std::string trimWhitespace(const std::string &value) {
    size_t start = value.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = value.find_last_not_of(" \t\n\r");
    return value.substr(start, end - start + 1);
}

// Split keywords string into individual keywords (supports; , \n \r as separators)
std::vector<std::string> splitKeywords(const std::string &raw);

// Join keywords vector into semicolon-separated string
std::string joinKeywords(const std::vector<std::string> &keywords);

// Smart join implementation helper
std::string smart_join_impl(const std::vector<std::string> &parts);

// Smart join: concatenate stringified args with intelligent spacing
// No space if left ends with "(", "{", "[" or right begins with ")", "}", "]", ".", ",", ";", "?", "!"
template <typename... Args> std::string smart_join(const Args &...args) {
    std::vector<std::string> parts;
    (..., ([&parts](const auto &arg) {
         std::ostringstream oss;
         oss << arg;
         std::string str = oss.str();
         // Strip whitespace
         std::string trimmed = trimWhitespace(str);
         if (!trimmed.empty()) {
             parts.push_back(trimmed);
         }
     }(args)));

    return smart_join_impl(parts);
}

// Log message implementations
void log_stdout_impl(const std::string &message);
void log_stderr_impl(const std::string &message);

// Initialize tee logging to console and log file under LOCALAPPDATA/Umann/MgVwr/logs.
// Returns true if logging is active.
bool initialize_app_logging(int retentionDays = 3);

// Apply retention policy for log files in the app log directory.
void apply_log_retention_policy(int retentionDays);

// Collapse SQL whitespace so it prints in one line.
std::string collapseSqlWhitespace(const std::string &sql);

// Render a SQLite column value in a compact YAML-like single-line form.
std::string sqliteValueToYamlString(sqlite3_stmt *stmt, int columnIndex);

// Centralized SQL abstraction used by all database components.
namespace sql {
void enableTrace(sqlite3 *db);
int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt);
int exec(sqlite3 *db, const char *sql, char **errmsg);
int step(sqlite3_stmt *stmt, std::string &firstRow, int &rowCount);
} // namespace sql

// Log a SQL statement in one line.
void logSqlStatement(const std::string &sql);

// Log a SELECT result summary and the first row in YAML-like form.
void logSqlSelectResult(const std::string &sql, sqlite3_stmt *stmt);

// Log message to stdout with timestamp (supports variable arguments)
template <typename... Args> void log_stdout(const Args &...args) {
    log_stdout_impl(smart_join(args...));
}

// Log message to stderr with timestamp (supports variable arguments)
template <typename... Args> void log_stderr(const Args &...args) {
    log_stderr_impl(smart_join(args...));
}

// Convert filesystem path to UTF-8 string
std::string toUtf8String(const fs::path &path);

// Trim trailing slashes from path string
std::string trimTrailingSlash(std::string path);

// Expand glob patterns to matching paths
std::vector<fs::path> expandGlob(const std::string &pattern);
