#pragma once

#include "json.hpp"
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Convert YAML node to JSON
json yamlToJson(const YAML::Node& node);

// Load and validate configuration from mgvwr.yaml using embedded schema
// Applies defaults from schema to missing config values
// Throws std::runtime_error on validation failure
// Returns enriched configuration as JSON object
json loadAndValidateConfig(const fs::path& searchDir = fs::path());

// Load and enrich a specific config file
// Throws std::runtime_error on validation failure
json loadAndValidateConfigFile(const fs::path& configFile);

// Enrich and validate a JSON value using a schema YAML string.
// Applies defaults from schema to missing values.
// Throws std::runtime_error on validation failure.
json enrichAndValidateJsonWithSchemaYaml(const std::string& schemaYaml, const json& instance);

// Enrich a single metadata object with schema defaults.
// schemaYaml: Schema YAML where root is array type; extracts items schema for enrichment.
// metadataObject: Single metadata object to enrich.
// Returns enriched metadata object with all schema defaults applied.
json enrichMetadataWithSchemaYaml(const std::string& schemaYaml, const json& metadataObject);

// Validate that a schema YAML string is valid YAML and valid JSON schema.
// Throws std::runtime_error if validation fails.
void validateSchemaYaml(const std::string& schemaYaml, const std::string& schemaName);

// Validate all built-in schemas on startup.
// Throws std::runtime_error if any schema is invalid.
void validateBuiltInSchemas();
