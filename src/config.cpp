#include "utils.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "config_schema_embedded.h"
#include "schema_engine.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace config_schema {

static std::string getSchemaYamlWithWinDir() {
    const char *windir = std::getenv("windir");
    const std::string actualWindowsDir = windir ? windir : "C:\\Windows";
    std::string schema = SCHEMA_YAML;

    const std::string from = "C:\\Windows\\";
    const std::string to = actualWindowsDir + "\\";

    size_t pos = 0;
    while ((pos = schema.find(from, pos)) != std::string::npos) {
        schema.replace(pos, from.length(), to);
        pos += to.length();
    }

    return schema;
}

} // namespace config_schema

void validateBuiltInSchemas() {
    std::string schemaYaml = config_schema::getSchemaYamlWithWinDir();
    schema_engine::validateSchemaYaml(schemaYaml, "SCHEMA_YAML");
}

static std::optional<json> loadYamlConfigFile(const fs::path &path) {
    try {
        YAML::Node yamlConfig = YAML::LoadFile(path.string());
        return schema_engine::yamlToJson(yamlConfig);
    } catch (const YAML::Exception &e) {
        throw std::runtime_error(std::string("Failed to parse mgvwr.yaml at ") + path.string() + ": " + e.what());
    }
}

static std::optional<json> loadYamlConfigFromSearchPath(const fs::path &searchDir) {
    std::vector<fs::path> configPaths;
    if (!searchDir.empty()) {
        configPaths.push_back(searchDir / "mgvwr.yaml");
    }
    configPaths.push_back("mgvwr.yaml");

    for (const auto &path : configPaths) {
        if (fs::exists(path)) {
            return loadYamlConfigFile(path);
        }
    }

    return std::nullopt;
}

json loadAndValidateConfig(const fs::path &searchDir) {
    try {
        const std::optional<json> configJson = loadYamlConfigFromSearchPath(searchDir);
        json instance = configJson.has_value() ? *configJson : json::object();
        const std::string schemaYaml = config_schema::getSchemaYamlWithWinDir();
        return schema_engine::enrichAndValidateJsonWithSchemaYaml(schemaYaml, instance);
    } catch (const nlohmann::json::exception &je) {
        throw std::runtime_error(std::string("Config loading JSON error: ") + je.what());
    } catch (const std::exception &) {
        throw;
    }
}

json loadAndValidateConfigFile(const fs::path &configFile) {
    try {
        if (!fs::exists(configFile)) {
            throw std::runtime_error("Config file not found: " + configFile.string());
        }

        YAML::Node yamlConfig = YAML::LoadFile(configFile.string());
        const json configJson = schema_engine::yamlToJson(yamlConfig);
        const std::string schemaYaml = config_schema::getSchemaYamlWithWinDir();
        return schema_engine::enrichAndValidateJsonWithSchemaYaml(schemaYaml, configJson);
    } catch (const nlohmann::json::exception &je) {
        throw std::runtime_error(std::string("Config loading JSON error: ") + je.what());
    } catch (const std::exception &) {
        throw;
    }
}

int runSelfCheck(const std::string &exePath, const std::string &configPath,
                 const std::string &exiftoolResponseSchemaYaml) {
    log_stdout("Running self-check...");

    try {
        log_stdout("  Checking embedded schemas...");
        validateBuiltInSchemas();
        schema_engine::validateSchemaYaml(exiftoolResponseSchemaYaml, "EXIFTOOL_RESPONSE_SCHEMA_YAML");
        log_stdout("    [OK] Embedded schemas valid");

        log_stdout("  Checking config file with enrichment...");
        log_stdout("    Config path: " + configPath);

        try {
            if (!fs::exists(configPath)) {
                log_stderr("    [FAIL] Config file not found: ", configPath);
                fs::remove(exePath);
                return 1;
            }

            const json enrichedConfig = loadAndValidateConfigFile(fs::path(configPath));
            (void)enrichedConfig;
            log_stdout("    [OK] Config file enriched and validated successfully");
        } catch (const std::exception &e) {
            log_stderr("    [FAIL] Config enrichment failed: ", e.what());
            fs::remove(exePath);
            return 1;
        }

        log_stdout("  Checking exiftool response schema...");
        YAML::Node schemaYaml = YAML::Load(exiftoolResponseSchemaYaml);
        (void)schemaYaml;
        log_stdout("    [OK] Exiftool schema valid");

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
