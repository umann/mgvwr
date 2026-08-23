#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

json yamlToJson(const YAML::Node &node);

void validateSchemaYaml(const std::string &schemaYaml, const std::string &schemaName);

json applySchemaDefaultsAndValidate(const json &schemaJson, const json &instance);

json enrichAndValidateJsonWithSchemaYaml(const std::string &schemaYaml, const json &instance);
