#pragma once

#include <string>

// Detect and repair common mojibake caused by UTF-8 bytes decoded as legacy single-byte encodings.
// Returns fixed UTF-8 text when a safe repair is found.
// Throws std::runtime_error only for clearly suspicious text that cannot be repaired.
std::string fixStringEncoding(const std::string &text);
