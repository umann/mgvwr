// poor_mans_exiftool.cpp
// Direct JPEG metadata reading functions - no external tools required
// These functions read EXIF, XMP, and IPTC metadata directly from JPEG files
// without requiring external tools like exiftool

#include "poor_mans_exiftool.h"
#include "config.h"
#include "utils.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <regex>
#include <string>
#include <vector>

static const std::vector<std::string> POOR_MANS_SUPPORTED_SUFFIXES = {".jpg", ".jpeg"};

static constexpr const char *EXIFTOOL_RESPONSE_SCHEMA_YAML = R"(
type: array
items:  
    type: object
    additionalProperties: false
    properties:
        SourceFile:
            type: string
            minLength: 1
            example: "F:/photos/2026/01/02/kornel_20260102_114713.jpg"
        DateTimeOriginal:
            type: string
            pattern: "^\\d{4}:\\d{2}:\\d{2} \\d{2}:\\d{2}:\\d{2}$"
            default: "0000:00:00 00:00:00"
            example: "2026:01:02 11:47:14"
        City:
            type: string
            default: ""
            example: "Budapest"
        Country:
            type: string
            default: ""
            example: "Belgium"
        State:
            type: string
            default: ""
            example: "Pest megye"
        Orientation:
            type: integer
            minimum: 1
            maximum: 8
            default: 1
            example: 1
        Keywords:
            type: array
            items:
                type: string
                minLength: 1
                example: "apple"
            default: []
            example: ["apple", "banana"]
        GPSLatitude:
            type: ["number", "null"]
            minimum: -90
            maximum: 90
            default: null
            example: 47.675997
        GPSLongitude:
            type: ["number", "null"]
            minimum: -180
            maximum: 180
            default: null
            example: 19.1444994
    required:
        - SourceFile
)";

static bool isPoorMansSupportedExtension(const fs::path &imagePath) {
    std::string ext = imagePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (const auto &suffix : POOR_MANS_SUPPORTED_SUFFIXES) {
        if (ext == suffix) {
            return true;
        }
    }
    return false;
}

// Read EXIF orientation tag (0x0112) from JPEG file
// Returns orientation value (1-8) or 1 if not found or not a JPEG
static int getExifOrientation(const std::string &imagePath) {
    try {
        std::ifstream file(imagePath, std::ios::binary);
        if (!file)
            return 1;

        unsigned char byte1, byte2;
        file.read(reinterpret_cast<char *>(&byte1), 1);
        file.read(reinterpret_cast<char *>(&byte2), 1);

        if (byte1 != 0xFF || byte2 != 0xD8) {
            return 1; // Not a JPEG
        }

        // Search for APP1 (EXIF) marker
        while (file.read(reinterpret_cast<char *>(&byte1), 1)) {
            if (byte1 != 0xFF)
                continue;
            if (!file.read(reinterpret_cast<char *>(&byte2), 1))
                break;

            if (byte2 == 0xE1) {
                // Found APP1 - read length
                unsigned char len_h, len_l;
                if (!file.read(reinterpret_cast<char *>(&len_h), 1))
                    break;
                if (!file.read(reinterpret_cast<char *>(&len_l), 1))
                    break;

                int length = (len_h << 8) | len_l;
                if (length < 10)
                    continue;

                std::vector<unsigned char> exif(length - 2);
                if (!file.read(reinterpret_cast<char *>(exif.data()), length - 2))
                    break;

                // Look for TIFF header (II or MM)
                bool littleEndian = false;
                size_t tiffStart = 0;

                // "Exif\0\0" is 6 bytes, then TIFF starts
                if (exif.size() > 8) {
                    if (exif[6] == 'I' && exif[7] == 'I') {
                        littleEndian = true;
                        tiffStart = 6;
                    } else if (exif[6] == 'M' && exif[7] == 'M') {
                        littleEndian = false;
                        tiffStart = 6;
                    }
                }

                if (tiffStart == 0 || tiffStart + 8 > exif.size())
                    continue;

                // Read IFD offset
                int ifdOffset = 0;
                if (littleEndian) {
                    ifdOffset = exif[tiffStart + 4] | (exif[tiffStart + 5] << 8) | (exif[tiffStart + 6] << 16) |
                                (exif[tiffStart + 7] << 24);
                } else {
                    ifdOffset = (exif[tiffStart + 4] << 24) | (exif[tiffStart + 5] << 16) |
                                (exif[tiffStart + 6] << 8) | exif[tiffStart + 7];
                }

                if (tiffStart + ifdOffset + 2 > exif.size())
                    continue;

                // Read IFD entries
                int numEntries = littleEndian ? (exif[tiffStart + ifdOffset] | (exif[tiffStart + ifdOffset + 1] << 8))
                                              : ((exif[tiffStart + ifdOffset] << 8) | exif[tiffStart + ifdOffset + 1]);

                for (int i = 0; i < numEntries; i++) {
                    size_t entryPos = tiffStart + ifdOffset + 2 + i * 12;
                    if (entryPos + 12 > exif.size())
                        break;

                    int tag = littleEndian ? (exif[entryPos] | (exif[entryPos + 1] << 8))
                                           : ((exif[entryPos] << 8) | exif[entryPos + 1]);

                    if (tag == 0x0112) {
                        // Found orientation tag
                        int value = littleEndian ? (exif[entryPos + 8] | (exif[entryPos + 9] << 8))
                                                 : ((exif[entryPos + 8] << 8) | exif[entryPos + 9]);

                        if (value >= 1 && value <= 8) {
                            return value;
                        }
                    }
                }
                return 1;
            } else if (byte2 >= 0xD0 && byte2 <= 0xD9) {
                // Other JPEG marker, skip
                continue;
            } else if (byte2 == 0xDA) {
                // Start of scan - stop looking
                break;
            }
        }
    } catch (...) {
        // Ignore errors
    }
    return 1;
}

// Extract EXIF DateTime tag (0x0132 for DateTime, or 0x9003 for DateTimeOriginal)
// Returns datetime string in format "YYYY:MM:DD HH:MM:SS" or empty string if not found
static std::string getExifDateTime(const std::vector<uint8_t> &data) {
    try {
        if (data.size() < 4)
            return "";

        if (data[0] != 0xFF || data[1] != 0xD8) {
            return "";
        }

        for (size_t i = 2; i + 1 < data.size(); i++) {
            if (data[i] != 0xFF)
                continue;

            uint8_t byte2 = data[i + 1];
            if (byte2 == 0xE1) {
                if (i + 4 > data.size())
                    break;
                unsigned char len_h = data[i + 2];
                unsigned char len_l = data[i + 3];

                int length = (len_h << 8) | len_l;
                if (length < 10 || i + 4 + length > data.size())
                    continue;

                size_t exifStart = i + 4;
                std::vector<uint8_t> exif(data.begin() + exifStart, data.begin() + exifStart + length);

                bool littleEndian = false;
                size_t tiffStart = 0;

                if (exif.size() > 8) {
                    if (exif[6] == 'I' && exif[7] == 'I') {
                        littleEndian = true;
                        tiffStart = 6;
                    } else if (exif[6] == 'M' && exif[7] == 'M') {
                        littleEndian = false;
                        tiffStart = 6;
                    }
                }

                if (tiffStart == 0 || tiffStart + 8 > exif.size())
                    continue;

                auto readShort = [&](size_t pos) -> uint16_t {
                    if (pos + 1 >= exif.size())
                        return 0;
                    return littleEndian ? static_cast<uint16_t>(exif[pos] | (exif[pos + 1] << 8))
                                        : static_cast<uint16_t>((exif[pos] << 8) | exif[pos + 1]);
                };

                auto readLong = [&](size_t pos) -> uint32_t {
                    if (pos + 3 >= exif.size())
                        return 0;
                    return littleEndian ? static_cast<uint32_t>(exif[pos] | (exif[pos + 1] << 8) |
                                                                (exif[pos + 2] << 16) | (exif[pos + 3] << 24))
                                        : static_cast<uint32_t>((exif[pos] << 24) | (exif[pos + 1] << 16) |
                                                                (exif[pos + 2] << 8) | exif[pos + 3]);
                };

                auto readDateTimeAt = [&](uint32_t offset) -> std::string {
                    size_t stringPos = tiffStart + offset;
                    if (stringPos + 19 < exif.size()) {
                        std::string dateTime;
                        for (int j = 0; j < 19; j++) {
                            dateTime += static_cast<char>(exif[stringPos + j]);
                        }
                        if (dateTime.find(':') != std::string::npos) {
                            return dateTime;
                        }
                    }
                    return "";
                };

                uint32_t ifdOffset = readLong(tiffStart + 4);

                if (tiffStart + ifdOffset + 2 > exif.size())
                    continue;

                uint16_t numEntries = readShort(tiffStart + ifdOffset);
                uint32_t exifIfdOffset = 0;

                auto scanIFD = [&](uint32_t baseOffset) -> std::string {
                    if (tiffStart + baseOffset + 2 > exif.size())
                        return "";
                    uint16_t entries = readShort(tiffStart + baseOffset);
                    for (uint16_t i = 0; i < entries; i++) {
                        size_t entryPos = tiffStart + baseOffset + 2 + i * 12;
                        if (entryPos + 12 > exif.size())
                            break;

                        uint16_t tag = readShort(entryPos);
                        uint16_t valueType = readShort(entryPos + 2);
                        uint32_t count = readLong(entryPos + 4);
                        uint32_t valueOffset = readLong(entryPos + 8);

                        // DateTime tags: 0x0132 or 0x9003 (DateTimeOriginal)
                        if ((tag == 0x0132 || tag == 0x9003) && valueType == 2 && count >= 19) {
                            std::string dt = readDateTimeAt(valueOffset);
                            if (!dt.empty()) {
                                return dt;
                            }
                        }
                    }
                    return "";
                };

                for (uint16_t i = 0; i < numEntries; i++) {
                    size_t entryPos = tiffStart + ifdOffset + 2 + i * 12;
                    if (entryPos + 12 > exif.size())
                        break;

                    uint16_t tag = readShort(entryPos);
                    uint16_t valueType = readShort(entryPos + 2);
                    uint32_t count = readLong(entryPos + 4);
                    uint32_t valueOffset = readLong(entryPos + 8);

                    if ((tag == 0x0132 || tag == 0x9003) && valueType == 2 && count >= 19) {
                        std::string dt = readDateTimeAt(valueOffset);
                        if (!dt.empty()) {
                            return dt;
                        }
                    }

                    // ExifIFD pointer
                    if (tag == 0x8769 && valueType == 4 && count == 1) {
                        exifIfdOffset = valueOffset;
                    }
                }

                if (exifIfdOffset != 0) {
                    std::string dt = scanIFD(exifIfdOffset);
                    if (!dt.empty()) {
                        return dt;
                    }
                }
            }
        }
    } catch (...) {
        // Ignore errors
    }
    return "";
}

// Extract XMP metadata packet from JPEG data
// Looks for <x:xmpmeta> ... </x:xmpmeta> tags
static std::string extractXmpPacket(const std::vector<uint8_t> &data) {
    try {
        std::string dataStr(reinterpret_cast<const char *>(data.data()), data.size());
        const std::string startTag = "<x:xmpmeta";
        const std::string endTag = "</x:xmpmeta>";

        size_t start = dataStr.find(startTag);
        if (start == std::string::npos)
            return "";
        size_t end = dataStr.find(endTag, start);
        if (end == std::string::npos)
            return "";
        end += endTag.size();
        return dataStr.substr(start, end - start);
    } catch (...) {
        return "";
    }
}

// Extract value from XMP metadata string
// Supports both tag-based values (<tag>value</tag>) and attribute values (tag="value")
// tags: vector of tag names to search for (in order of preference)
// Returns empty string if no matching tag found
static std::string extractXmpValue(const std::string &xmp, const std::vector<std::string> &tags) {
    for (const auto &tag : tags) {
        std::string openTag = "<" + tag;
        size_t pos = xmp.find(openTag);
        if (pos != std::string::npos) {
            size_t gt = xmp.find('>', pos);
            if (gt != std::string::npos) {
                std::string closeTag = "</" + tag + ">";
                size_t end = xmp.find(closeTag, gt + 1);
                if (end != std::string::npos) {
                    return xmp.substr(gt + 1, end - gt - 1);
                }
            }
        }

        std::string attr = tag + "=\"";
        pos = xmp.find(attr);
        if (pos != std::string::npos) {
            size_t start = pos + attr.size();
            size_t end = xmp.find('"', start);
            if (end != std::string::npos) {
                return xmp.substr(start, end - start);
            }
        }
    }
    return "";
}

static std::string extractIptcValue(const std::vector<uint8_t> &data, uint8_t record, uint8_t dataset);

static std::string extractXmpOrIptcValue(const std::string &xmp, const std::vector<std::string> &tags,
                                         const std::vector<uint8_t> &data, uint8_t record, uint8_t dataset) {
    std::string value = extractXmpValue(xmp, tags);
    if (!value.empty()) {
        return value;
    }
    return extractIptcValue(data, record, dataset);
}

static std::vector<std::string> extractXmpKeywordArray(const std::string &xmp, const std::vector<std::string> &tags) {
    std::vector<std::string> result;
    const std::regex liRegex(R"(<rdf:li[^>]*>(.*?)</rdf:li>)");

    for (const auto &tag : tags) {
        std::string openTag = "<" + tag;
        size_t pos = xmp.find(openTag);
        if (pos == std::string::npos) {
            continue;
        }

        size_t gt = xmp.find('>', pos);
        if (gt == std::string::npos) {
            continue;
        }

        std::string closeTag = "</" + tag + ">";
        size_t end = xmp.find(closeTag, gt + 1);
        if (end == std::string::npos) {
            continue;
        }

        std::string inner = xmp.substr(gt + 1, end - gt - 1);

        for (std::sregex_iterator it(inner.begin(), inner.end(), liRegex); it != std::sregex_iterator(); ++it) {
            if (it->size() < 2) {
                continue;
            }
            std::string value = trimWhitespace((*it)[1].str());
            if (!value.empty()) {
                result.push_back(value);
            }
        }

        if (!result.empty()) {
            return result;
        }

        std::string fallback = trimWhitespace(inner);
        if (!fallback.empty()) {
            result.push_back(fallback);
            return result;
        }
    }

    return result;
}

// Extract IPTC metadata value
// IPTC records structure: 0x1C (record marker) + record number + dataset number + length (2 bytes) + data
// Commonly used IPTC tags:
//   Record 2, Dataset 5: Title
//   Record 2, Dataset 15: Category
//   Record 2, Dataset 20: Supplemental Categories
//   Record 2, Dataset 25: Keywords
//   Record 2, Dataset 105: Caption/Description
static std::string extractIptcValue(const std::vector<uint8_t> &data, uint8_t record, uint8_t dataset) {
    try {
        if (data.size() < 5)
            return "";

        for (size_t i = 0; i + 4 < data.size(); i++) {
            if (data[i] == 0x1C && data[i + 1] == record && data[i + 2] == dataset) {
                uint16_t len = static_cast<uint16_t>((data[i + 3] << 8) | data[i + 4]);
                size_t start = i + 5;
                if (start + len <= data.size()) {
                    // Validate that extracted data is valid text
                    bool isValidText = true;
                    for (size_t j = 0; j < len; j++) {
                        uint8_t byte = data[start + j];
                        // Allow printable ASCII (32-126) and common UTF-8 extended bytes
                        // Reject control characters and binary data
                        if (byte < 32 && byte != 9 && byte != 10 && byte != 13) {
                            // Control characters (except tab, newline, carriage return)
                            isValidText = false;
                            break;
                        }
                    }
                    if (isValidText && len > 0) {
                        return std::string(reinterpret_cast<const char *>(&data[start]), len);
                    }
                }
            }
        }
    } catch (...) {
        return "";
    }
    return "";
}

static std::vector<std::string> extractIptcValues(const std::vector<uint8_t> &data, uint8_t record, uint8_t dataset) {
    std::vector<std::string> results;

    try {
        if (data.size() < 5)
            return results;

        for (size_t i = 0; i + 4 < data.size(); i++) {
            if (data[i] != 0x1C || data[i + 1] != record || data[i + 2] != dataset) {
                continue;
            }

            uint16_t len = static_cast<uint16_t>((data[i + 3] << 8) | data[i + 4]);
            size_t start = i + 5;
            if (start + len > data.size()) {
                continue;
            }

            bool isValidText = true;
            for (size_t j = 0; j < len; j++) {
                uint8_t byte = data[start + j];
                if (byte < 32 && byte != 9 && byte != 10 && byte != 13) {
                    isValidText = false;
                    break;
                }
            }

            if (isValidText && len > 0) {
                results.emplace_back(reinterpret_cast<const char *>(&data[start]), len);
            }
        }
    } catch (...) {
        return results;
    }

    return results;
}

static bool extractGpsFromExif(const std::vector<uint8_t> &data, double &outLat, double &outLon) {
    outLat = 0.0;
    outLon = 0.0;

    try {
        if (data.size() < 4)
            return false;
        if (data[0] != 0xFF || data[1] != 0xD8)
            return false;

        for (size_t i = 2; i + 4 < data.size(); ++i) {
            if (data[i] != 0xFF)
                continue;
            uint8_t marker = data[i + 1];
            if (marker != 0xE1)
                continue;

            uint16_t length = static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
            if (length < 10 || i + 4 + length > data.size())
                continue;

            size_t exifStart = i + 4;
            std::vector<uint8_t> exif(data.begin() + exifStart, data.begin() + exifStart + length);

            bool littleEndian = false;
            size_t tiffStart = 0;

            if (exif.size() > 8) {
                if (exif[6] == 'I' && exif[7] == 'I') {
                    littleEndian = true;
                    tiffStart = 6;
                } else if (exif[6] == 'M' && exif[7] == 'M') {
                    littleEndian = false;
                    tiffStart = 6;
                }
            }

            if (tiffStart == 0 || tiffStart + 8 > exif.size())
                return false;

            auto readShort = [&](size_t pos) -> uint16_t {
                if (pos + 1 >= exif.size())
                    return 0;
                return littleEndian ? static_cast<uint16_t>(exif[pos] | (exif[pos + 1] << 8))
                                    : static_cast<uint16_t>((exif[pos] << 8) | exif[pos + 1]);
            };

            auto readLong = [&](size_t pos) -> uint32_t {
                if (pos + 3 >= exif.size())
                    return 0;
                return littleEndian ? static_cast<uint32_t>(exif[pos] | (exif[pos + 1] << 8) | (exif[pos + 2] << 16) |
                                                            (exif[pos + 3] << 24))
                                    : static_cast<uint32_t>((exif[pos] << 24) | (exif[pos + 1] << 16) |
                                                            (exif[pos + 2] << 8) | exif[pos + 3]);
            };

            auto readAscii = [&](uint32_t count, uint32_t valueOffset) -> std::string {
                std::string result;
                if (count == 0)
                    return result;
                result.reserve(count);

                if (count <= 4) {
                    for (uint32_t idx = 0; idx < count; ++idx) {
                        uint8_t byte = littleEndian ? static_cast<uint8_t>((valueOffset >> (8 * idx)) & 0xFF)
                                                    : static_cast<uint8_t>((valueOffset >> (8 * (3 - idx))) & 0xFF);
                        if (byte == 0)
                            break;
                        result.push_back(static_cast<char>(byte));
                    }
                    return result;
                }

                size_t pos = tiffStart + valueOffset;
                if (pos + count > exif.size())
                    return "";
                for (uint32_t idx = 0; idx < count; ++idx) {
                    char c = static_cast<char>(exif[pos + idx]);
                    if (c == '\0')
                        break;
                    result.push_back(c);
                }
                return result;
            };

            auto readRational = [&](uint32_t valueOffset) -> double {
                size_t pos = tiffStart + valueOffset;
                if (pos + 7 >= exif.size())
                    return 0.0;
                uint32_t num = readLong(pos);
                uint32_t den = readLong(pos + 4);
                if (den == 0)
                    return 0.0;
                return static_cast<double>(num) / static_cast<double>(den);
            };

            auto readRationalArray = [&](uint32_t valueOffset, uint32_t count, std::vector<double> &out) -> bool {
                if (count < 3)
                    return false;
                size_t pos = tiffStart + valueOffset;
                if (pos + 8 * count > exif.size())
                    return false;
                out.clear();
                for (uint32_t idx = 0; idx < 3; ++idx) {
                    out.push_back(readRational(valueOffset + idx * 8));
                }
                return true;
            };

            uint32_t ifdOffset = readLong(tiffStart + 4);
            if (tiffStart + ifdOffset + 2 > exif.size())
                return false;

            uint16_t numEntries = readShort(tiffStart + ifdOffset);
            uint32_t gpsOffset = 0;

            for (uint16_t entryIdx = 0; entryIdx < numEntries; ++entryIdx) {
                size_t entryPos = tiffStart + ifdOffset + 2 + entryIdx * 12;
                if (entryPos + 12 > exif.size())
                    break;

                uint16_t tag = readShort(entryPos);
                uint16_t valueType = readShort(entryPos + 2);
                uint32_t count = readLong(entryPos + 4);
                uint32_t valueOffset = readLong(entryPos + 8);

                if (tag == 0x8825 && valueType == 4 && count == 1) {
                    gpsOffset = valueOffset;
                    break;
                }
            }

            if (gpsOffset == 0)
                return false;

            size_t gpsBase = tiffStart + gpsOffset;
            if (gpsBase + 2 > exif.size())
                return false;

            uint16_t gpsEntries = readShort(gpsBase);
            std::string latRef;
            std::string lonRef;
            std::vector<double> latParts;
            std::vector<double> lonParts;

            for (uint16_t entryIdx = 0; entryIdx < gpsEntries; ++entryIdx) {
                size_t entryPos = gpsBase + 2 + entryIdx * 12;
                if (entryPos + 12 > exif.size())
                    break;

                uint16_t tag = readShort(entryPos);
                uint16_t valueType = readShort(entryPos + 2);
                uint32_t count = readLong(entryPos + 4);
                uint32_t valueOffset = readLong(entryPos + 8);

                if (tag == 0x0001 && valueType == 2) {
                    latRef = readAscii(count, valueOffset);
                } else if (tag == 0x0002 && valueType == 5) {
                    readRationalArray(valueOffset, count, latParts);
                } else if (tag == 0x0003 && valueType == 2) {
                    lonRef = readAscii(count, valueOffset);
                } else if (tag == 0x0004 && valueType == 5) {
                    readRationalArray(valueOffset, count, lonParts);
                }
            }

            if (latParts.size() >= 3 && lonParts.size() >= 3) {
                outLat = latParts[0] + (latParts[1] / 60.0) + (latParts[2] / 3600.0);
                outLon = lonParts[0] + (lonParts[1] / 60.0) + (lonParts[2] / 3600.0);

                if (!latRef.empty() && (latRef[0] == 'S' || latRef[0] == 's')) {
                    outLat = -outLat;
                }
                if (!lonRef.empty() && (lonRef[0] == 'W' || lonRef[0] == 'w')) {
                    outLon = -outLon;
                }
                return true;
            }

            return false;
        }
    } catch (...) {
        return false;
    }

    return false;
}

// Extract image metadata from files (batch processing)
// Returns map<imagePath, metadata json> matching extractExiftoolData format
std::map<fs::path, json> extractImageMetadata(const std::vector<fs::path> &imagePaths) {
    std::map<fs::path, json> results;

    auto enrichWithDefaults = [&](json &meta) {
        try {
            json wrapped = json::array();
            wrapped.push_back(meta);
            wrapped = enrichAndValidateJsonWithSchemaYaml(EXIFTOOL_RESPONSE_SCHEMA_YAML, wrapped);
            if (wrapped.is_array() && !wrapped.empty() && wrapped[0].is_object()) {
                meta = wrapped[0];
            }
        } catch (...) {
            // Leave as-is if schema validation fails.
        }
    };

    for (const auto &imagePath : imagePaths) {
        try {
            if (!isPoorMansSupportedExtension(imagePath)) {
                json meta = json::object();
                meta["SourceFile"] = imagePath.string();
                enrichWithDefaults(meta);
                results[imagePath] = meta;
                continue;
            }

            std::ifstream file(imagePath, std::ios::binary);
            if (!file)
                continue;

            const size_t maxReadSize = 256 * 1024;
            std::vector<uint8_t> fileData;

            file.seekg(0, std::ios::end);
            std::streamsize fileSize = file.tellg();
            file.seekg(0, std::ios::beg);

            size_t readSize = std::min(static_cast<size_t>(fileSize), maxReadSize);
            fileData.resize(readSize);
            file.read(reinterpret_cast<char *>(fileData.data()), readSize);
            file.close();

            json meta = json::object();
            meta["SourceFile"] = imagePath.string();

            meta["DateTimeOriginal"] = getExifDateTime(fileData);

            // Extract XMP packet for string fields
            std::string xmp = extractXmpPacket(fileData);

            meta["Country"] = extractXmpOrIptcValue(xmp,
                                                    {"photoshop:Country", "Iptc4xmpCore:CountryName",
                                                     "Iptc4xmpExt:LocationShownCountryName", "xmp:Country"},
                                                    fileData, 0x02, 0x65);

            meta["State"] = extractXmpOrIptcValue(xmp,
                                                  {"photoshop:State", "Iptc4xmpCore:ProvinceState",
                                                   "Iptc4xmpExt:LocationShownProvinceState", "xmp:State"},
                                                  fileData, 0x02, 0x5F);

            meta["City"] = extractXmpOrIptcValue(
                xmp, {"photoshop:City", "Iptc4xmpCore:City", "Iptc4xmpExt:LocationShownCity", "xmp:City"}, fileData,
                0x02, 0x5A);

            meta["Location"] =
                extractXmpOrIptcValue(xmp,
                                      {"Iptc4xmpCore:Location", "photoshop:Location", "photoshop:Sublocation",
                                       "Iptc4xmpExt:LocationShownLocation", "xmp:Location"},
                                      fileData, 0x02, 0x5C);

            meta["Description"] =
                extractXmpOrIptcValue(xmp,
                                      {"dc:description", "photoshop:Caption", "Iptc4xmpCore:Description",
                                       "Iptc4xmpExt:Description", "xmp:Description"},
                                      fileData, 0x02, 0x69);

            // Extract Keywords (as array)
            std::vector<std::string> xmpKeywords =
                extractXmpKeywordArray(xmp, {"photoshop:Keywords", "pdf:Keywords", "xmp:Keywords",
                                             "Iptc4xmpCore:Keywords", "dc:subject", "xmp:Subject"});
            if (!xmpKeywords.empty()) {
                json keywordArray = json::array();
                for (const auto &kw : xmpKeywords) {
                    keywordArray.push_back(kw);
                }
                meta["Keywords"] = keywordArray;
            } else {
                std::vector<std::string> iptcKeywords = extractIptcValues(fileData, 0x02, 0x19);
                if (!iptcKeywords.empty()) {
                    json keywordArray = json::array();
                    for (const auto &kw : iptcKeywords) {
                        for (const auto &splitKw : splitKeywords(kw)) {
                            keywordArray.push_back(splitKw);
                        }
                    }
                    meta["Keywords"] = keywordArray;
                }
            }

            // Extract GPS coordinates from EXIF
            double lat;
            double lon;
            if (extractGpsFromExif(fileData, lat, lon)) {
                meta["GPSLatitude"] = lat;
                meta["GPSLongitude"] = lon;
            }

            // Extract Orientation
            int orientation = getExifOrientation(imagePath.string());
            meta["Orientation"] = orientation;

            for (auto it = meta.begin(); it != meta.end();) {
                if (it.value().is_string() && it.value().get<std::string>().empty()) {
                    it = meta.erase(it);
                    continue;
                }
                ++it;
            }

            enrichWithDefaults(meta);

            results[imagePath] = meta;

        } catch (...) {
            // Skip files that fail to process
        }
    }

    return results;
}
