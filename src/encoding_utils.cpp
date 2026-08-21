/*
Module for encoding utilities.
It addresses the issue of mojibake in EXIF metadata by trying to detect and repair common encoding mistakes.
Mojibake means that UTF-8 bytes were misinterpreted as single-byte encodings like Latin-1, Latin-2, or CP1252,
resulting in garbled text.
This is a port of pyumann/src/umann/utils/encoding_utils.py that is just 47 lines. Both created by Copilot.
IDK why the C++ version is 5 times longer. Anyway, it works.
*/

#include "encoding_utils.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class SourceEncoding {
    Latin2,
    Latin1,
    Cp1252,
};

// Code point -> CP1252 byte mappings for the non-ISO extension block.
static constexpr std::array<std::pair<uint32_t, uint8_t>, 27> kCp1252SpecialMap = {{
    {0x20AC, 0x80}, // €
    {0x201A, 0x82}, // ‚
    {0x0192, 0x83}, // ƒ
    {0x201E, 0x84}, // „
    {0x2026, 0x85}, // …
    {0x2020, 0x86}, // †
    {0x2021, 0x87}, // ‡
    {0x02C6, 0x88}, // ˆ
    {0x2030, 0x89}, // ‰
    {0x0160, 0x8A}, // Š
    {0x2039, 0x8B}, // ‹
    {0x0152, 0x8C}, // Œ
    {0x017D, 0x8E}, // Ž
    {0x2018, 0x91}, // ‘
    {0x2019, 0x92}, // ’
    {0x201C, 0x93}, // “
    {0x201D, 0x94}, // ”
    {0x2022, 0x95}, // •
    {0x2013, 0x96}, // –
    {0x2014, 0x97}, // —
    {0x02DC, 0x98}, // ˜
    {0x2122, 0x99}, // ™
    {0x0161, 0x9A}, // š
    {0x203A, 0x9B}, // ›
    {0x0153, 0x9C}, // œ
    {0x017E, 0x9E}, // ž
    {0x0178, 0x9F}, // Ÿ
}};

bool containsMojibake1(const std::string &text) {
    // UTF-8 bytes for code points: U+00C3 (Ã), U+00C2 (Â), U+00C5 (Å)
    const std::vector<std::string> markers = {
        "\xC3\x83",
        "\xC3\x82",
        "\xC3\x85",
    };

    for (const auto &marker : markers) {
        if (text.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool containsMojibake2(const std::string &text) {
    // Approximation of regex: [ÃÂÅ][\x80-\xbf]
    // In UTF-8 bytes this is: (C3 83|C3 82|C3 85) (C2 80..BF)
    const auto n = text.size();
    for (size_t i = 0; i + 3 < n; ++i) {
        const uint8_t b0 = static_cast<uint8_t>(text[i]);
        const uint8_t b1 = static_cast<uint8_t>(text[i + 1]);
        const uint8_t b2 = static_cast<uint8_t>(text[i + 2]);
        const uint8_t b3 = static_cast<uint8_t>(text[i + 3]);

        const bool firstMatch = (b0 == 0xC3) && (b1 == 0x83 || b1 == 0x82 || b1 == 0x85);
        const bool secondMatch = (b2 == 0xC2) && (b3 >= 0x80 && b3 <= 0xBF);
        if (firstMatch && secondMatch) {
            return true;
        }
    }
    return false;
}

bool decodeOneUtf8CodePoint(const std::string &s, size_t &i, uint32_t &outCp) {
    if (i >= s.size()) {
        return false;
    }

    const uint8_t b0 = static_cast<uint8_t>(s[i]);

    if ((b0 & 0x80) == 0) {
        outCp = b0;
        ++i;
        return true;
    }

    if ((b0 & 0xE0) == 0xC0) {
        if (i + 1 >= s.size()) {
            return false;
        }
        const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
        if ((b1 & 0xC0) != 0x80) {
            return false;
        }
        outCp = static_cast<uint32_t>(((b0 & 0x1F) << 6) | (b1 & 0x3F));
        i += 2;
        return true;
    }

    if ((b0 & 0xF0) == 0xE0) {
        if (i + 2 >= s.size()) {
            return false;
        }
        const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
        const uint8_t b2 = static_cast<uint8_t>(s[i + 2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
            return false;
        }
        outCp = static_cast<uint32_t>(((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F));
        i += 3;
        return true;
    }

    if ((b0 & 0xF8) == 0xF0) {
        if (i + 3 >= s.size()) {
            return false;
        }
        const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
        const uint8_t b2 = static_cast<uint8_t>(s[i + 2]);
        const uint8_t b3 = static_cast<uint8_t>(s[i + 3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
            return false;
        }
        outCp = static_cast<uint32_t>(((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F));
        i += 4;
        return true;
    }

    return false;
}

bool isValidUtf8(const std::string &s) {
    size_t i = 0;
    uint32_t cp = 0;
    while (i < s.size()) {
        if (!decodeOneUtf8CodePoint(s, i, cp)) {
            return false;
        }
    }
    return true;
}

bool encodeCodePointToByte(uint32_t cp, SourceEncoding encoding, uint8_t &outByte) {
    // For mojibake repair we mostly encounter code points in U+0000..U+00FF.
    // Keeping this strict avoids speculative transformations.
    if (cp <= 0xFF) {
        outByte = static_cast<uint8_t>(cp);
        return true;
    }

    // Minimal CP1252 support for the 0x80..0x9F extension block when represented as Unicode code points.
    if (encoding == SourceEncoding::Cp1252) {
        for (const auto &[codePoint, byteValue] : kCp1252SpecialMap) {
            if (cp == codePoint) {
                outByte = byteValue;
                return true;
            }
        }
    }

    return false;
}

bool encodeUtf8ToSingleByte(const std::string &text, SourceEncoding encoding, std::string &outBytes) {
    outBytes.clear();
    outBytes.reserve(text.size());

    size_t i = 0;
    uint32_t cp = 0;
    while (i < text.size()) {
        if (!decodeOneUtf8CodePoint(text, i, cp)) {
            return false;
        }
        uint8_t b = 0;
        if (!encodeCodePointToByte(cp, encoding, b)) {
            return false;
        }
        outBytes.push_back(static_cast<char>(b));
    }

    return true;
}

} // namespace

std::string fixStringEncoding(const std::string &text) {
    if (text.empty()) {
        return text;
    }

    if (!containsMojibake1(text)) {
        return text;
    }

    const SourceEncoding candidates[] = {
        SourceEncoding::Latin2,
        SourceEncoding::Latin1,
        SourceEncoding::Cp1252,
    };

    for (const SourceEncoding candidate : candidates) {
        std::string candidateBytes;
        if (!encodeUtf8ToSingleByte(text, candidate, candidateBytes)) {
            continue;
        }

        // Equivalent to Python: text.encode(source_encoding).decode("utf-8")
        // If bytes are valid UTF-8, the decoded Unicode string has the same UTF-8 bytes.
        if (!isValidUtf8(candidateBytes)) {
            continue;
        }

        if (!containsMojibake1(candidateBytes)) {
            return candidateBytes;
        }
    }

    if (containsMojibake2(text)) {
        throw std::runtime_error("Could not fix suspicious mojibake text");
    }

    return text;
}
