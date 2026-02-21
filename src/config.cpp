#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <yaml-cpp/yaml.h>


namespace fs = std::filesystem;
using json = nlohmann::json;

static const char *getWinDir() {
    const char *windir = std::getenv("windir");
    return windir ? windir : "C:\\Windows";
}

// Embedded schema YAML as a string constant
static constexpr const char *SCHEMA_YAML = R"(
$defs:
  window_size:
    type: array
    minItems: 2
    maxItems: 2
    items:
      oneOf:
        - type: string
          pattern: '^(100|([1-9][0-9])(?:[.][0-9]+)?)%$'
          description: 'Percentage of screen size: 10% to 100%'
        - type: integer
          minimum: 100
          description: 'Absolute pixels: minimum 100'
type: object
additionalProperties: false
properties:
  home_country:
    type: string
    default: ''
  geo_keyword_prefix:
    type: string
    default: ''
  regions:
    type: array
    items:
      type: string
      minLength: 1
      example: Duna
    default: []
  image_file:
    type: object
    additionalProperties: false
    properties:
      supported_suffixes:
        type: array
        items:
          type: string
        example:
          - .jpg
          - .jpeg
          - .png
          - .bmp
          - .gif
          - .tiff
        default:
          - .jpg
          - .jpeg
          - .png
          - .bmp
          - .gif
          - .tiff
  font:
    type: object
    additionalProperties: false
    properties:
      by_os:
        type: object
        additionalProperties: false
        patternProperties:
          ^(windows|macos|linux)$:
            type: object
            additionalProperties: false
            patternProperties:
              ^(main|monospace)$:
                type: array
                minItems: 1
                items:
                  type: string
                  minLength: 1
                  example: 'C:\Windows\Fonts\Verdana.ttf'
        default:
          windows:
            main:
              - 'C:\Windows\Fonts\segoeui.ttf'
              - 'C:\Windows\Fonts\arial.ttf'
              - 'C:\Windows\Fonts\tahoma.ttf'
            monospace:
              - 'C:\Windows\Fonts\consola.ttf'
              - 'C:\Windows\Fonts\cour.ttf'
              - 'C:\Windows\Fonts\lucon.ttf'
          macos:
            main:
              - /Library/Fonts/Arial.ttf
              - /System/Library/Fonts/Supplemental/Arial.ttf
            monospace:
              - /Library/Fonts/Courier New.ttf
              - /System/Library/Fonts/Monaco.dfont
              - /System/Library/Fonts/Menlo.ttc
          linux:
            main:
              - /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
              - /usr/share/fonts/TTF/DejaVuSans.ttf
            monospace:
              - /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
              - /usr/share/fonts/TTF/DejaVuSansMono.ttf
              - /usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf
      candidates:
        type: array
        items:
          type: string
          description: Font path
          example: 'C:\Windows\Fonts\Verdana.ttf'
        default: []
      size:
        oneOf:
          - type: string
            pattern: '^(100|[1-9][0-9]?)%$'
            description: 'Percentage of window width: 1% to 100%'
          - type: integer
            minimum: 10
            description: 'Absolute pixels: minimum 10'
        default: 0.625%
  single_instance_mode:
    type: boolean
    default: true
  path_classifications:
    type: array
    items:
      type: object
      additionalProperties: false
      properties:
        pattern:
          type: string
          pattern: '[(].+[)]'
          example: '(/(\d{4})/(\d{2})/(\d{2})(?:/|$))'
        names:
          type: array
          items:
            type: string
            pattern: \\S
          minItems: 1
          example:
            - year
            - month
            - day
    default: []
  watched_folders:
    type: array
    items:
      type: string
      minLength: 1
      example: 'C:\photos'
    default: []
  window_mode:
    type: object
    additionalProperties: false
    properties:
      is_default:
        type: boolean
        default: false
      default_size:
        $ref: '#/$defs/window_size'
        default:
          - 80%
          - 80%
  quiet_mode:
    type: boolean
    default: false
  filters:
    type: array
    items:
      type: object
      additionalProperties: false
      properties:
        expression:
          type: string
          pattern: '^[a-zA-Z0-9_]+(:[a-zA-Z0-9_]+)? % ''.+''$'
          example: Keywords % 'NOMINUS'
        key:
          type: string
          pattern: '^[a-z0-9]$'
          example: 'n'
    default: []
  map:
    type: object
    additionalProperties: false
    properties:
      cache:
        type: object
        additionalProperties: false
        properties:
          location:
            type: string
            description: >-
              Optional path to cache directory. If missing or empty, a default
              location will be used.
            example: 'C:\map_cache'
            default: ''
          enabled:
            type: boolean
            default: true
          max_size_mb:
            type: integer
            minimum: 0
            maximum: 100000
            default: 5000
      viewer:
        type: object
        additionalProperties: false
        properties:
          zoom:
            type: object
            additionalProperties: false
            properties:
              default:
                type: integer
                default: 15
              minimum:
                type: integer
                default: 2
              maximum:
                type: integer
                default: 18
          window:
            type: object
            properties:
              inline:
                type: boolean
                default: true
              size:
                $ref: '#/$defs/window_size'
                default:
                  - 25%
                  - 18.75%
      links:
        type: array
        items:
          type: object
          additionalProperties: false
          properties:
            name:
              type: string
              example: OpenStreetMap
            enabled:
              type: boolean
              default: true
            zoom:
              type: integer
              minimum: 10
              maximum: 50
              default: 15
            gui_url_template:
              type: string
              pattern: '^http(s)?://.+\{\{\s*GPSLatitude\s*\}\}.+\{\{\s*GPSLongitude\s*\}\}'
              example: >-
                https://www.openstreetmap.org/?mlat={{GPSLatitude}}&mlon={{GPSLongitude}}#map={{zoom}}/{{GPSLatitude}}/{{GPSLongitude}}
        default: []
)";

/*
Below is a concrete, implementable enrichment algorithm that matches exactly what you described:

- Only create missing container objects if doing so would materialize at least one real default somewhere below.

- No need to sprinkle default: {} everywhere.

- Works with $defs + $ref, allOf, and oneOf with your rule: don’t use defaults inside oneOf branches; if a oneOf node
can be missing, it must have a default at the oneOf node itself.

I’ll describe the algorithm first, then give tight C++-style pseudocode.

# Semantics

We define enrich(schema S, instance I) as “return an enriched JSON value for I under S”.

Core rules

1. If instance exists (property present):

    - Recurse into it and apply defaults to missing descendants.

    - Never delete user-provided values.

2. If instance is missing:

    - If S has default: materialize it (deep-copy the JSON) and then recurse into it (so nested defaults still apply).

    - Else if S is an object schema with properties:

        - Look at each property p and ask: “would p materialize anything if missing?”

        - Only if at least one property would materialize, create {} and fill just those properties.

        - Otherwise keep the whole object missing.

    -Else: keep missing.

3. Arrays

    - Do not invent elements to satisfy minItems.

    - Only materialize a missing array if there’s an explicit default for the array node itself.

    - If the array exists (from user or default), recurse into existing elements with items.

# Handling composition keywords

## $ref

Always resolve $ref to the referenced schema before applying any logic.

- Local refs like #/$defs/foo should be resolved against the root schema document.

## allOf

Merge logically by applying enrichment against each subschema in sequence:

- Start with current instance (maybe missing).

- For each subschema: enrich again using the result as the new instance.

- For missing instance, this can cause materialization if any subschema contains defaults (directly or nested).

This matches “defaults anywhere in allOf can pull ancestors”.

(You don’t need a perfect JSON-Schema merge; you’re only using it to drive default materialization + recursion.)

## oneOf

Your policy makes this easy:

- If instance is missing:

    - If the oneOf node has default: use it and recurse.

    - Else: keep missing (don’t guess branch).

- If instance exists:

    - Find the unique matching branch:

        - For each branch, run enrich(branch, copy_of_instance) and then validate that copy against the branch.

        - Keep the branches that validate.

        - If exactly one validates: commit that enriched copy.

        - If 0 or >1: stop enrichment here (or throw as “ambiguous oneOf”), and let final validation produce the real
error.

This avoids guessing and keeps oneOf deterministic.

# Two-phase flow for your app

1. Load config (missing/empty ⇒ “missing root”).

2. Enrich with the algorithm below (may materialize lots of defaults).

3. Validate the enriched config against the full schema (raise on failure).

4. Return enriched config.

# C++-style pseudocode (close to drop-in)

Assume:

- json is nlohmann::json

- You have a resolver SchemaResolver that can resolve $ref (local refs) to a json schema object.

- You have a function bool validate_against(const json& schema, const json& instance) (use your validator).

We represent “missing” as std::optional<json>{} rather than null.
*/

using json = nlohmann::json;

struct SchemaResolver {
    const json &root; // entire schema document

    // Resolve $ref like "#/$defs/window_size"
    json resolve_ref(const std::string &ref) const {
        if (ref.empty() || ref[0] != '#') {
            throw std::runtime_error("Only local refs starting with # are supported");
        }

        // Parse JSON pointer (e.g., "#/$defs/window_size" -> ["$defs", "window_size"])
        std::string pointer = ref.substr(1); // Remove '#'
        if (pointer.empty() || pointer[0] != '/') {
            return root; // "#" refers to root
        }

        json current = root;
        size_t start = 1; // Skip initial '/'
        while (start < pointer.size()) {
            size_t end = pointer.find('/', start);
            if (end == std::string::npos)
                end = pointer.size();

            std::string token = pointer.substr(start, end - start);

            // Unescape JSON pointer tokens: ~1 -> /, ~0 -> ~
            size_t pos = 0;
            while ((pos = token.find('~', pos)) != std::string::npos) {
                if (pos + 1 < token.size()) {
                    if (token[pos + 1] == '1') {
                        token.replace(pos, 2, "/");
                    } else if (token[pos + 1] == '0') {
                        token.replace(pos, 2, "~");
                    }
                }
                pos++;
            }

            if (current.is_object() && current.contains(token)) {
                current = current[token];
            } else {
                throw std::runtime_error("Invalid $ref: " + ref);
            }

            start = end + 1;
        }

        return current;
    }

    json resolve(const json &schema) const {
        if (schema.is_object() && schema.contains("$ref")) {
            const auto &ref = schema["$ref"];
            if (!ref.is_string()) {
                throw std::runtime_error("$ref must be a string, got: " + ref.dump());
            }
            return resolve_ref(ref.get<std::string>());
        }
        return schema;
    }
};

// Forward declarations for validation with resolver support
static bool validate_against(const SchemaResolver &R, const json &schema, const json &instance);
static bool validate_against_with_path(const SchemaResolver &R, const json &schema, const json &instance,
                                       const std::string &path, bool suppressErrors = false);

static bool is_object_schema(const json &S) {
    if (!S.is_object())
        return false;
    if (S.contains("type")) {
        const auto &t = S["type"];
        if (t.is_string())
            return t.get<std::string>() == "object";
        if (t.is_array()) {
            for (const auto &x : t)
                if (x.is_string() && x.get<std::string>() == "object")
                    return true;
        }
    }
    // If type is absent, many schemas still behave as objects when "properties" exists;
    // you can choose to treat that as object-like for defaulting:
    if (S.contains("properties") && S["properties"].is_object())
        return true;
    return false;
}

static bool is_array_schema(const json &S) {
    if (!S.is_object())
        return false;
    if (S.contains("type")) {
        const auto &t = S["type"];
        if (t.is_string())
            return t.get<std::string>() == "array";
        if (t.is_array()) {
            for (const auto &x : t)
                if (x.is_string() && x.get<std::string>() == "array")
                    return true;
        }
    }
    if (S.contains("items"))
        return true;
    return false;
}

// Forward declaration
std::optional<json> enrich(const SchemaResolver &R, const json &schema, std::optional<json> inst);

// Enrich an object that exists (inst has value and is object)
static std::optional<json> enrich_object_existing(const SchemaResolver &R, const json &S, json obj) {
    if (!S.contains("properties") || !S["properties"].is_object())
        return obj;

    const json &props = S["properties"];
    for (auto it = props.begin(); it != props.end(); ++it) {
        const std::string &key = it.key();
        json propSchema = R.resolve(it.value());

        if (obj.contains(key)) {
            // present: enrich in place
            auto enrichedChild = enrich(R, propSchema, std::optional<json>(obj[key]));
            if (enrichedChild.has_value())
                obj[key] = std::move(*enrichedChild);
            // if enrich returned missing for an existing property, keep user value as-is
        } else {
            // missing: decide whether it should materialize
            auto enrichedChild = enrich(R, propSchema, std::optional<json>());
            if (enrichedChild.has_value())
                obj[key] = std::move(*enrichedChild);
        }
    }
    return obj;
}

// Enrich an object that is missing: only materialize it if any child would materialize
static std::optional<json> enrich_object_missing(const SchemaResolver &R, const json &S) {
    if (!S.contains("properties") || !S["properties"].is_object()) {
        return std::nullopt; // missing object stays missing
    }

    json result = json::object();
    bool any = false;

    const json &props = S["properties"];
    for (auto it = props.begin(); it != props.end(); ++it) {
        const std::string &key = it.key();
        json propSchema = R.resolve(it.value());

        auto enrichedChild = enrich(R, propSchema, std::optional<json>());
        if (enrichedChild.has_value()) {
            result[key] = std::move(*enrichedChild);
            any = true;
        }
    }

    if (!any)
        return std::nullopt;
    return result;
}

static std::optional<json> enrich_array_existing(const SchemaResolver &R, const json &S, json arr) {
    if (!arr.is_array())
        return arr;
    if (!S.contains("items"))
        return arr;

    json itemsSchema = R.resolve(S["items"]);
    for (auto &el : arr) {
        auto enrichedEl = enrich(R, itemsSchema, std::optional<json>(el));
        if (enrichedEl.has_value())
            el = std::move(*enrichedEl);
    }
    return arr;
}

// Forward declarations for validation (moved after SchemaResolver definition below)

// allOf: apply sequentially
static std::optional<json> enrich_allOf(const SchemaResolver &R, const json &S, std::optional<json> inst) {
    std::optional<json> cur = inst;
    for (const auto &sub0 : S["allOf"]) {
        json sub = R.resolve(sub0);
        cur = enrich(R, sub, cur);
    }
    return cur;
}

// oneOf: choose branch only if instance exists; if missing require default at node
static std::optional<json> enrich_oneOf(const SchemaResolver &R, const json &S, std::optional<json> inst) {
    if (!inst.has_value()) {
        if (S.contains("default")) {
            json d = S["default"];
            // still recurse into default to pick up nested defaults (outside of oneOf branches)
            return enrich(R, S, d);
        }
        return std::nullopt;
    }

    // instance exists: try each branch, enrich+validate, pick unique
    std::vector<json> candidates;
    for (const auto &branch0 : S["oneOf"]) {
        json branch = R.resolve(branch0);

        json trial = *inst;
        auto enrichedTrialOpt = enrich(R, branch, trial);
        if (!enrichedTrialOpt.has_value())
            continue; // shouldn't happen for present, but safe
        json enrichedTrial = std::move(*enrichedTrialOpt);

        // Suppress errors during branch exploration - we expect only one to match
        if (validate_against_with_path(R, branch, enrichedTrial, "$", true)) {
            candidates.push_back(std::move(enrichedTrial));
            if (candidates.size() > 1)
                break; // ambiguous
        }
    }

    if (candidates.size() == 1)
        return candidates[0];

    // ambiguous or none: don't guess; return original instance (or throw if you prefer)
    return inst;
}

std::optional<json> enrich(const SchemaResolver &R, const json &schema0, std::optional<json> inst) {
    json S = R.resolve(schema0);

    // composition first
    if (S.contains("allOf") && S["allOf"].is_array()) {
        return enrich_allOf(R, S, inst);
    }
    if (S.contains("oneOf") && S["oneOf"].is_array()) {
        // Your policy: no defaults in branches; parent default if missing
        return enrich_oneOf(R, S, inst);
    }

    // If missing: default wins
    if (!inst.has_value()) {
        if (S.contains("default")) {
            json d = S["default"];
            // IMPORTANT: recurse after materializing default (so child defaults apply)
            return enrich(R, S, d);
        }

        // No direct default: only autovivify objects if they have default-bearing descendants
        if (is_object_schema(S)) {
            return enrich_object_missing(R, S);
        }

        // arrays: do not invent, unless default exists (handled above)
        return std::nullopt;
    }

    // Present instance: recurse based on shape/schema
    json I = std::move(*inst);

    if (is_object_schema(S) && I.is_object()) {
        return enrich_object_existing(R, S, std::move(I));
    }
    if (is_array_schema(S) && I.is_array()) {
        return enrich_array_existing(R, S, std::move(I));
    }

    return I;
}

// Get schema YAML with Windows directory injected from environment
static std::string getSchemaYamlWithWinDir() {
    const char *windir = getWinDir();
    std::string schema = SCHEMA_YAML;

    // Replace C:\\Windows\\ with actual windows directory
    std::string from = "C:\\Windows\\";
    std::string to = std::string(windir) + "\\";

    size_t pos = 0;
    while ((pos = schema.find(from, pos)) != std::string::npos) {
        schema.replace(pos, from.length(), to);
        pos += to.length();
    }

    return schema;
}

// Basic JSON Schema validator - checks type, required properties, and basic constraints
// with detailed path tracking for better error messages
static bool validate_against(const SchemaResolver &R, const json &schema, const json &instance) {
    return validate_against_with_path(R, schema, instance, "$", false);
}

static bool validate_against_with_path(const SchemaResolver &R, const json &schema, const json &instance,
                                       const std::string &path, bool suppressErrors) {
    if (!schema.is_object())
        return true;

    // Resolve $ref before processing
    json resolvedSchema = R.resolve(schema);

    // Handle oneOf: try each branch, succeed if exactly one matches (suppress errors during exploration)
    if (resolvedSchema.contains("oneOf") && resolvedSchema["oneOf"].is_array()) {
        int matches = 0;
        for (const auto &branch : resolvedSchema["oneOf"]) {
            if (validate_against_with_path(R, branch, instance, path, true)) { // suppress during branch exploration
                matches++;
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

    // Check type constraint
    if (resolvedSchema.contains("type")) {
        const auto &typeConstraint = resolvedSchema["type"];
        bool typeMatches = false;

        auto checkType = [&](const std::string &t) {
            if (t == "object" && instance.is_object())
                return true;
            if (t == "array" && instance.is_array())
                return true;
            if (t == "string" && instance.is_string())
                return true;
            if (t == "number" && instance.is_number())
                return true;
            if (t == "integer" && instance.is_number_integer())
                return true;
            if (t == "boolean" && instance.is_boolean())
                return true;
            if (t == "null" && instance.is_null())
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
                std::string actualType = instance.type_name();
                std::string expectedType =
                    typeConstraint.is_string() ? typeConstraint.get<std::string>() : typeConstraint.dump();
                std::cerr << "Type mismatch at " << path << ": expected " << expectedType << ", got " << actualType
                          << " (value: " << instance.dump() << ")\n";
            }
            return false;
        }
    }

    // Check object properties
    if (instance.is_object() && resolvedSchema.contains("properties")) {
        // Check required properties
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

        // Recursively validate properties
        const json &props = resolvedSchema["properties"];
        for (const auto &[key, propSchema] : props.items()) {
            if (instance.contains(key)) {
                std::string newPath = path + "." + key;
                json resolvedPropSchema = R.resolve(propSchema);
                if (!validate_against_with_path(R, resolvedPropSchema, instance[key], newPath, suppressErrors)) {
                    return false;
                }
            }
        }
    }

    // Check array constraints and items
    if (instance.is_array()) {
        if (resolvedSchema.contains("minItems") && resolvedSchema["minItems"].is_number()) {
            if (instance.size() < resolvedSchema["minItems"].get<size_t>()) {
                if (!suppressErrors) {
                    std::cerr << "Array too small at " << path << ": minimum "
                              << resolvedSchema["minItems"].get<size_t>() << ", got " << instance.size() << "\n";
                }
                return false;
            }
        }
        if (resolvedSchema.contains("maxItems") && resolvedSchema["maxItems"].is_number()) {
            if (instance.size() > resolvedSchema["maxItems"].get<size_t>()) {
                if (!suppressErrors) {
                    std::cerr << "Array too large at " << path << ": maximum "
                              << resolvedSchema["maxItems"].get<size_t>() << ", got " << instance.size() << "\n";
                }
                return false;
            }
        }

        // Validate array items
        if (resolvedSchema.contains("items")) {
            json itemsSchema = R.resolve(resolvedSchema["items"]);
            for (size_t i = 0; i < instance.size(); ++i) {
                std::string itemPath = path + "[" + std::to_string(i) + "]";
                if (!validate_against_with_path(R, itemsSchema, instance[i], itemPath, suppressErrors)) {
                    return false;
                }
            }
        }
    }

    // Check number constraints
    if (instance.is_number()) {
        if (resolvedSchema.contains("minimum") && resolvedSchema["minimum"].is_number()) {
            if (instance.get<double>() < resolvedSchema["minimum"].get<double>()) {
                if (!suppressErrors) {
                    std::cerr << "Number too small at " << path << ": minimum " << schema["minimum"].get<double>()
                              << ", got " << instance.get<double>() << "\n";
                }
                return false;
            }
        }
        if (resolvedSchema.contains("maximum") && resolvedSchema["maximum"].is_number()) {
            if (instance.get<double>() > resolvedSchema["maximum"].get<double>()) {
                if (!suppressErrors) {
                    std::cerr << "Number too large at " << path << ": maximum "
                              << resolvedSchema["maximum"].get<double>() << ", got " << instance.get<double>() << "\n";
                }
                return false;
            }
        }
    }

    return true;
}

// Convert YAML to JSON
static json yamlToJson(const YAML::Node &node) {
    if (node.IsNull()) {
        return nullptr;
    } else if (node.IsScalar()) {
        // Get string representation first to check type
        std::string strVal = node.as<std::string>();

        // Check for boolean keywords (YAML 1.2 spec: only true/false are standard)
        // But YAML-cpp might also recognize yes/no, on/off for YAML 1.1 compatibility
        if (strVal == "true" || strVal == "false" || strVal == "yes" || strVal == "no" || strVal == "on" ||
            strVal == "off" || strVal == "True" || strVal == "False" || strVal == "Yes" || strVal == "No" ||
            strVal == "On" || strVal == "Off" || strVal == "TRUE" || strVal == "FALSE" || strVal == "YES" ||
            strVal == "NO" || strVal == "ON" || strVal == "OFF") {
            try {
                return node.as<bool>();
            } catch (...) {
            }
        }

        // Try to parse as integer
        try {
            // Check if it looks like an integer (no decimal point)
            if (strVal.find('.') == std::string::npos && strVal.find('e') == std::string::npos &&
                strVal.find('E') == std::string::npos) {
                return node.as<int>();
            }
        } catch (...) {
        }

        // Try to parse as double
        try {
            return node.as<double>();
        } catch (...) {
        }

        // Default: return as string
        return strVal;
    } else if (node.IsSequence()) {
        json arr = json::array();
        for (const auto &item : node) {
            arr.push_back(yamlToJson(item));
        }
        return arr;
    } else if (node.IsMap()) {
        json obj = json::object();
        for (const auto &kv : node) {
            obj[kv.first.as<std::string>()] = yamlToJson(kv.second);
        }
        return obj;
    }
    return nullptr;
}

// Enrich and validate a JSON value using a schema YAML string.
// Applies defaults from schema to missing values.
void validateSchemaYaml(const std::string &schemaYaml, const std::string &schemaName) {
    // Parse schema YAML
    YAML::Node yamlSchema;
    try {
        yamlSchema = YAML::Load(schemaYaml);
    } catch (const YAML::Exception &e) {
        throw std::runtime_error("Built-in schema '" + schemaName + "' is not valid YAML: " + e.what());
    }

    // Convert to JSON to verify it's a valid structure
    json schemaJson;
    try {
        schemaJson = yamlToJson(yamlSchema);
    } catch (const std::exception &e) {
        throw std::runtime_error("Built-in schema '" + schemaName + "' cannot be converted to JSON: " + e.what());
    }

    // Validate that it has required JSON schema properties
    if (!schemaJson.is_object() && !schemaJson.is_boolean()) {
        throw std::runtime_error("Built-in schema '" + schemaName +
                                 "' is not a valid JSON schema (must be object or boolean)");
    }

    // If it's an object, check for 'type' property as basic schema validation
    if (schemaJson.is_object()) {
        if (!schemaJson.contains("type") && !schemaJson.contains("$ref") && !schemaJson.contains("oneOf") &&
            !schemaJson.contains("anyOf") && !schemaJson.contains("allOf")) {
            throw std::runtime_error("Built-in schema '" + schemaName +
                                     "' missing required schema keyword (type, $ref, oneOf, anyOf, or allOf)");
        }
    }
}

void validateBuiltInSchemas() {
    // Validate the config schema
    std::string schemaYaml = getSchemaYamlWithWinDir();
    validateSchemaYaml(schemaYaml, "SCHEMA_YAML");
}

json enrichAndValidateJsonWithSchemaYaml(const std::string &schemaYaml, const json &instance) {
    // Parse embedded schema from constant
    json schemaJson;
    try {
        YAML::Node yamlSchema = YAML::Load(schemaYaml);
        schemaJson = yamlToJson(yamlSchema);
    } catch (const YAML::Exception &e) {
        throw std::runtime_error(std::string("Failed to parse embedded schema: ") + e.what());
    }

    // Enrich config with defaults from schema
    SchemaResolver resolver{schemaJson};
    std::optional<json> enriched;
    try {
        enriched = enrich(resolver, schemaJson, instance);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Schema enrichment failed: ") + e.what());
    }

    // If enrichment produced nothing (no config, no defaults), return empty object
    if (!enriched.has_value()) {
        enriched = json::object();
    }

    // Validate the enriched config
    try {
        if (!validate_against(resolver, schemaJson, *enriched)) {
            // Build detailed error message
            std::string enrichedStr = enriched.has_value() ? enriched->dump(2) : "{}";
            std::stringstream errorMsg;
            errorMsg << "Schema validation failed.\n\n";
            errorMsg << "Enriched configuration data:\n";
            errorMsg << enrichedStr << "\n\n";
            errorMsg << "Check stderr output above for validation error details.\n";
            throw std::runtime_error(errorMsg.str());
        }
    } catch (const nlohmann::json::exception &je) {
        std::string enrichedStr = enriched.has_value() ? enriched->dump(2) : "{}";
        std::stringstream errorMsg;
        errorMsg << "JSON validation error: " << je.what() << "\n\n";
        errorMsg << "Enriched configuration:\n" << enrichedStr << "\n";
        throw std::runtime_error(errorMsg.str());
    }

    return *enriched;
}

// Public API: Load config and schema, enrich with defaults, validate, and return enriched config
json loadAndValidateConfig(const fs::path &searchDir = fs::path()) {
    try {
        // Try to find config file
        std::vector<fs::path> configPaths;
        if (!searchDir.empty()) {
            configPaths.push_back(searchDir / "mgvwr.yaml");
        }
        configPaths.push_back("mgvwr.yaml");

        std::optional<json> configJson;
        fs::path loadedPath;
        for (const auto &path : configPaths) {
            if (fs::exists(path)) {
                try {
                    loadedPath = path;
                    YAML::Node yamlConfig = YAML::LoadFile(path.string());
                    configJson = yamlToJson(yamlConfig);
                    break;
                } catch (const YAML::Exception &e) {
                    throw std::runtime_error(std::string("Failed to parse mgvwr.yaml at ") + path.string() + ": " +
                                             e.what());
                }
            }
        }

        json instance = configJson.has_value() ? *configJson : json::object();
        std::string schemaYaml = getSchemaYamlWithWinDir();
        return enrichAndValidateJsonWithSchemaYaml(schemaYaml, instance);
    } catch (const nlohmann::json::exception &je) {
        throw std::runtime_error(std::string("Config loading JSON error: ") + je.what());
    } catch (const std::exception &e) {
        // Re-throw with context preserved
        throw;
    }
}

// Load and enrich a specific config file
json loadAndValidateConfigFile(const fs::path &configFile) {
    try {
        if (!fs::exists(configFile)) {
            throw std::runtime_error("Config file not found: " + configFile.string());
        }

        YAML::Node yamlConfig = YAML::LoadFile(configFile.string());
        json configJson = yamlToJson(yamlConfig);
        std::string schemaYaml = getSchemaYamlWithWinDir();
        return enrichAndValidateJsonWithSchemaYaml(schemaYaml, configJson);
    } catch (const nlohmann::json::exception &je) {
        throw std::runtime_error(std::string("Config loading JSON error: ") + je.what());
    } catch (const std::exception &e) {
        throw;
    }
}
// Enrich a single metadata object (which normally would be an array item) with schema defaults.
// schemaYaml: Schema YAML where the root is an array type; we extract items schema.
// metadataObject: The metadata object to enrich (normally a single item from the array).
// Returns enriched metadata object with defaults applied.
json enrichMetadataWithSchemaYaml(const std::string &schemaYaml, const json &metadataObject) {
    try {
        // Parse the schema YAML
        YAML::Node schemaNode = YAML::Load(schemaYaml);
        json schemaJson = yamlToJson(schemaNode);

        // Extract the items schema (the root schema is for an array, so items contains the object schema)
        if (!schemaJson.is_object() || !schemaJson.contains("items")) {
            throw std::runtime_error("Schema must be an array type with 'items' property");
        }

        json itemsSchema = schemaJson["items"];

        // Create a synthetic single-item array to enrich using the existing function
        json tempArray = json::array();
        tempArray.push_back(metadataObject);

        // Enrich the array (which will apply enrichment to the single item)
        json enrichedArray = enrichAndValidateJsonWithSchemaYaml(schemaYaml, tempArray);

        // Extract and return the enriched item
        if (enrichedArray.is_array() && enrichedArray.size() > 0) {
            return enrichedArray[0];
        }

        // Fallback: return the metadata object as-is
        return metadataObject;

    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Metadata enrichment error: ") + e.what());
    }
}