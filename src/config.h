#pragma once

#include "serialize.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// Load and validate configuration from mgvwr.yaml using embedded schema
// Applies defaults from schema to missing config values
// Throws std::runtime_error on validation failure
// Returns enriched configuration as JSON object
json loadAndValidateConfig(const fs::path &searchDir = fs::path());

// Load and enrich a specific config file
// Throws std::runtime_error on validation failure
json loadAndValidateConfigFile(const fs::path &configFile);

// Validate all built-in schemas on startup.
// Throws std::runtime_error if any schema is invalid.
void validateBuiltInSchemas();

// Self-check: validate built-in schemas, EXIF schema, and config enrichment.
int runSelfCheck(const std::string &exePath, const std::string &configPath,
                 const std::string &exiftoolResponseSchemaYaml);
