#pragma once

#include <map>
#include <string>
#include <vector>

struct ParsedArguments {
    std::map<std::string, std::string> options;
    std::vector<std::string> positional;
};

// Parse CLI arguments using the provided validator map.
// Returns false when parsing fails and logs the reason.
bool parseArguments(int argc, char *argv[], const std::map<std::string, std::string> &argValidator,
                    ParsedArguments &out);

// Quote and escape an argv-style argument for readable shell-like logging.
std::string shlexQuoteArg(const char *rawArg);
