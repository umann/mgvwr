#include "serialize.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {

using json = nlohmann::json;

static std::string stringify_json_number(const json &value) {
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<std::uint64_t>());
    }
    if (value.is_number_float()) {
        std::ostringstream oss;
        oss << std::setprecision(std::numeric_limits<double>::max_digits10) << value.get<double>();
        return oss.str();
    }
    return value.dump();
}

static std::optional<double> parse_json_number_string(const std::string &text) {
    try {
        size_t parsedChars = 0;
        double value = std::stod(text, &parsedChars);
        if (parsedChars == text.size()) {
            return value;
        }
    } catch (...) {
    }
    return std::nullopt;
}

static bool schema_expects_string(const json &schema) {
    if (!schema.is_object() || !schema.contains("type")) {
        return false;
    }
    const json &typeConstraint = schema["type"];
    auto matches = [](const std::string &typeName) { return typeName == "string"; };
    if (typeConstraint.is_string()) {
        return matches(typeConstraint.get<std::string>());
    }
    if (typeConstraint.is_array()) {
        for (const auto &entry : typeConstraint) {
            if (entry.is_string() && matches(entry.get<std::string>())) {
                return true;
            }
        }
    }
    return false;
}

static bool schema_expects_number(const json &schema) {
    if (!schema.is_object() || !schema.contains("type")) {
        return false;
    }
    const json &typeConstraint = schema["type"];
    auto matches = [](const std::string &typeName) { return typeName == "number" || typeName == "integer"; };
    if (typeConstraint.is_string()) {
        return matches(typeConstraint.get<std::string>());
    }
    if (typeConstraint.is_array()) {
        for (const auto &entry : typeConstraint) {
            if (entry.is_string() && matches(entry.get<std::string>())) {
                return true;
            }
        }
    }
    return false;
}

struct SchemaResolver {
    const json &root;

    json resolve(const json &schema) const {
        if (!schema.is_object() || !schema.contains("$ref") || !schema["$ref"].is_string()) {
            return schema;
        }

        const std::string ref = schema["$ref"].get<std::string>();
        if (ref.empty() || ref[0] != '#') {
            throw std::runtime_error("Only local refs starting with # are supported");
        }

        std::string pointer = ref.substr(1);
        if (pointer.empty() || pointer[0] != '/') {
            return root;
        }

        json current = root;
        size_t start = 1;
        while (start < pointer.size()) {
            size_t end = pointer.find('/', start);
            if (end == std::string::npos) {
                end = pointer.size();
            }
            std::string token = pointer.substr(start, end - start);
            while (true) {
                size_t pos = token.find('~');
                if (pos == std::string::npos) {
                    break;
                }
                if (pos + 1 < token.size()) {
                    if (token[pos + 1] == '1') {
                        token.replace(pos, 2, "/");
                    } else if (token[pos + 1] == '0') {
                        token.replace(pos, 2, "~");
                    }
                }
                pos = pos + 1;
                if (pos >= token.size()) {
                    break;
                }
            }
            if (!current.is_object() || !current.contains(token)) {
                throw std::runtime_error("Unresolvable $ref: " + ref);
            }
            current = current[token];
            start = end + 1;
        }
        return current;
    }
};

static void coerce_string_like_values(const SchemaResolver &resolver, const json &schema, json &instance) {
    if (!schema.is_object()) {
        return;
    }

    json resolvedSchema = resolver.resolve(schema);

    if (schema_expects_string(resolvedSchema) && instance.is_number()) {
        instance = stringify_json_number(instance);
        return;
    }

    if (schema_expects_number(resolvedSchema) && instance.is_string()) {
        const auto parsedNumber = parse_json_number_string(instance.get<std::string>());
        if (parsedNumber.has_value()) {
            if (resolvedSchema.contains("type") && resolvedSchema["type"].is_string() &&
                resolvedSchema["type"].get<std::string>() == "integer") {
                instance = static_cast<std::int64_t>(*parsedNumber);
            } else {
                instance = *parsedNumber;
            }
            return;
        }
    }

    if (resolvedSchema.contains("allOf") && resolvedSchema["allOf"].is_array()) {
        for (const auto &subSchema : resolvedSchema["allOf"]) {
            coerce_string_like_values(resolver, subSchema, instance);
        }
    }

    if (instance.is_object() && resolvedSchema.contains("properties") && resolvedSchema["properties"].is_object()) {
        const json &props = resolvedSchema["properties"];
        for (const auto &[key, propSchema] : props.items()) {
            if (instance.contains(key)) {
                coerce_string_like_values(resolver, propSchema, instance[key]);
            }
        }
    }

    if (instance.is_array() && resolvedSchema.contains("items")) {
        for (auto &element : instance) {
            coerce_string_like_values(resolver, resolvedSchema["items"], element);
        }
    }
}

static bool validate_against_with_path(const SchemaResolver &resolver, const json &schema, const json &instance,
                                       const std::string &path, bool suppressErrors) {
    if (!schema.is_object()) {
        return true;
    }

    json resolvedSchema = resolver.resolve(schema);

    if (resolvedSchema.contains("oneOf") && resolvedSchema["oneOf"].is_array()) {
        int matches = 0;
        for (const auto &branch : resolvedSchema["oneOf"]) {
            if (validate_against_with_path(resolver, branch, instance, path, true)) {
                ++matches;
            }
        }
        if (matches != 1) {
            if (!suppressErrors) {
                std::cerr << "oneOf validation failed at " << path << ": need exactly 1 match, got " << matches
                          << "\n";
            }
            return false;
        }
        return true;
    }

    if (resolvedSchema.contains("type")) {
        const auto &typeConstraint = resolvedSchema["type"];
        bool typeMatches = false;
        auto checkType = [&](const std::string &expected) {
            if (expected == "object" && instance.is_object())
                return true;
            if (expected == "array" && instance.is_array())
                return true;
            if (expected == "string" && instance.is_string())
                return true;
            if (expected == "number" && instance.is_number())
                return true;
            if (expected == "integer" && instance.is_number_integer())
                return true;
            if (expected == "boolean" && instance.is_boolean())
                return true;
            if (expected == "null" && instance.is_null())
                return true;
            return false;
        };
        if (typeConstraint.is_string()) {
            typeMatches = checkType(typeConstraint.get<std::string>());
        } else if (typeConstraint.is_array()) {
            for (const auto &t : typeConstraint) {
                if (t.is_string() && checkType(t.get<std::string>())) {
                    typeMatches = true;
                    break;
                }
            }
        }
        if (!typeMatches) {
            if (!suppressErrors) {
                std::cerr << "Type mismatch at " << path << ": expected " << typeConstraint.dump() << ", got "
                          << instance.type_name() << "\n";
            }
            return false;
        }
    }

    if (instance.is_object() && resolvedSchema.contains("properties")) {
        if (resolvedSchema.contains("required") && resolvedSchema["required"].is_array()) {
            for (const auto &req : resolvedSchema["required"]) {
                if (req.is_string() && !instance.contains(req.get<std::string>())) {
                    if (!suppressErrors) {
                        std::cerr << "Missing required property at " << path << ": " << req.get<std::string>() << "\n";
                    }
                    return false;
                }
            }
        }
        const json &props = resolvedSchema["properties"];
        for (const auto &[key, propSchema] : props.items()) {
            if (instance.contains(key)) {
                json resolvedPropSchema = resolver.resolve(propSchema);
                if (!validate_against_with_path(resolver, resolvedPropSchema, instance[key], path + "." + key,
                                                suppressErrors)) {
                    return false;
                }
            }
        }
    }

    if (instance.is_array() && resolvedSchema.contains("items")) {
        for (size_t i = 0; i < instance.size(); ++i) {
            if (!validate_against_with_path(resolver, resolver.resolve(resolvedSchema["items"]), instance[i],
                                            path + "[" + std::to_string(i) + "]", suppressErrors)) {
                return false;
            }
        }
    }

    return true;
}

} // namespace

json yamlToJson(const YAML::Node &node) {
    if (node.IsNull()) {
        return nullptr;
    }
    if (node.IsScalar()) {
        const std::string strVal = node.as<std::string>();
        if (strVal == "true" || strVal == "false" || strVal == "yes" || strVal == "no" || strVal == "on" ||
            strVal == "off" || strVal == "True" || strVal == "False" || strVal == "Yes" || strVal == "No" ||
            strVal == "On" || strVal == "Off" || strVal == "TRUE" || strVal == "FALSE" || strVal == "YES" ||
            strVal == "NO" || strVal == "ON" || strVal == "OFF") {
            try {
                return node.as<bool>();
            } catch (...) {
            }
        }
        try {
            if (strVal.find('.') == std::string::npos && strVal.find('e') == std::string::npos &&
                strVal.find('E') == std::string::npos) {
                return node.as<int>();
            }
        } catch (...) {
        }
        try {
            return node.as<double>();
        } catch (...) {
        }
        return strVal;
    }
    if (node.IsSequence()) {
        json arr = json::array();
        for (const auto &item : node) {
            arr.push_back(yamlToJson(item));
        }
        return arr;
    }
    if (node.IsMap()) {
        json obj = json::object();
        for (const auto &kv : node) {
            obj[kv.first.as<std::string>()] = yamlToJson(kv.second);
        }
        return obj;
    }
    return nullptr;
}

void validateSchemaYaml(const std::string &schemaYaml, const std::string &schemaName) {
    YAML::Node yamlSchema;
    try {
        yamlSchema = YAML::Load(schemaYaml);
    } catch (const YAML::Exception &e) {
        throw std::runtime_error("Built-in schema '" + schemaName + "' is not valid YAML: " + e.what());
    }

    json schemaJson;
    try {
        schemaJson = yamlToJson(yamlSchema);
    } catch (const std::exception &e) {
        throw std::runtime_error("Built-in schema '" + schemaName + "' cannot be converted to JSON: " + e.what());
    }

    if (!schemaJson.is_object() && !schemaJson.is_boolean()) {
        throw std::runtime_error("Built-in schema '" + schemaName +
                                 "' is not a valid JSON schema (must be object or boolean)");
    }

    if (schemaJson.is_object()) {
        if (!schemaJson.contains("type") && !schemaJson.contains("$ref") && !schemaJson.contains("oneOf") &&
            !schemaJson.contains("anyOf") && !schemaJson.contains("allOf")) {
            throw std::runtime_error("Built-in schema '" + schemaName +
                                     "' missing required schema keyword (type, $ref, oneOf, anyOf, or allOf)");
        }
    }
}

static void apply_schema_defaults_recursively(const SchemaResolver &resolver, const json &schema, json &instance) {
    if (!schema.is_object()) {
        return;
    }

    json resolved = resolver.resolve(schema);
    if (instance.is_null()) {
        if (resolved.contains("default")) {
            instance = resolved["default"];
        } else if (resolved.contains("properties") && resolved["properties"].is_object()) {
            instance = json::object();
        } else if (resolved.contains("items") && resolved["items"].is_object()) {
            instance = json::array();
        }
    }

    if (instance.is_object() && resolved.contains("properties") && resolved["properties"].is_object()) {
        const json &props = resolved["properties"];
        for (const auto &[key, propSchema] : props.items()) {
            if (!instance.contains(key) || instance[key].is_null()) {
                if (propSchema.contains("default")) {
                    instance[key] = propSchema["default"];
                } else if (propSchema.contains("properties") && propSchema["properties"].is_object()) {
                    instance[key] = json::object();
                } else if (propSchema.contains("items") && propSchema["items"].is_object()) {
                    instance[key] = json::array();
                }
            }
            if (instance.contains(key)) {
                apply_schema_defaults_recursively(resolver, propSchema, instance[key]);
            }
        }
    }

    if (instance.is_array() && resolved.contains("items")) {
        for (auto &element : instance) {
            apply_schema_defaults_recursively(resolver, resolved["items"], element);
        }
    }
}

json applySchemaDefaultsAndValidate(const json &schemaJson, const json &instance) {
    const SchemaResolver resolver{schemaJson};
    json enriched = instance;

    apply_schema_defaults_recursively(resolver, schemaJson, enriched);
    coerce_string_like_values(resolver, schemaJson, enriched);

    if (!validate_against_with_path(resolver, schemaJson, enriched, "$", false)) {
        std::stringstream errorMsg;
        errorMsg << "Schema validation failed.\n\nCheck stderr output above for validation error details.\n";
        throw std::runtime_error(errorMsg.str());
    }

    return enriched;
}

json enrichAndValidateJsonWithSchemaYaml(const std::string &schemaYaml, const json &instance) {
    YAML::Node yamlSchema = YAML::Load(schemaYaml);
    json schemaJson = yamlToJson(yamlSchema);
    return applySchemaDefaultsAndValidate(schemaJson, instance);
}
