#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <yaml-cpp/yaml.h>

namespace schema_engine {

using json = nlohmann::json;

json yamlToJson(const YAML::Node &node);
json applySchemaDefaultsAndValidate(const json &schemaJson, const json &instance);
json enrichAndValidateJsonWithSchemaYaml(const std::string &schemaYaml, const json &instance);
void validateSchemaYaml(const std::string &schemaYaml, const std::string &schemaName);

} // namespace schema_engine
