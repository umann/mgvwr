#include "cli.h"

#include "utils.h"

std::string shlexQuoteArg(const char *rawArg) {
    std::string arg = rawArg ? std::string(rawArg) : std::string();

    // Quote only when needed to keep output readable.
    if (arg.find_first_of(" \t\r\n\"") == std::string::npos) {
        return arg;
    }

    std::string escaped;
    escaped.reserve(arg.size() + 8);
    for (char c : arg) {
        if (c == '\\' || c == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }

    return "\"" + escaped + "\"";
}

bool parseArguments(int argc, char *argv[], const std::map<std::string, std::string> &argValidator,
                    ParsedArguments &out) {
    out.options.clear();
    out.positional.clear();

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        auto validatorIt = argValidator.find(arg);
        if (validatorIt != argValidator.end()) {
            std::string type = validatorIt->second;

            if (type == "boolean") {
                out.options[arg] = "";
            } else {
                if (i + 1 >= argc) {
                    log_stderr("Error: Option ", arg, " requires a parameter");
                    return false;
                }

                std::string value = argv[++i];

                if (type == "integer") {
                    try {
                        std::stoi(value);
                        out.options[arg] = value;
                    } catch (...) {
                        log_stderr("Error: Option ", arg, " requires an integer value, got: ", value);
                        return false;
                    }
                } else if (type == "number") {
                    try {
                        std::stod(value);
                        out.options[arg] = value;
                    } catch (...) {
                        log_stderr("Error: Option ", arg, " requires a numeric value, got: ", value);
                        return false;
                    }
                } else { // "string"
                    out.options[arg] = value;
                }
            }
        } else {
            out.positional.push_back(arg);
        }
    }

    return true;
}
