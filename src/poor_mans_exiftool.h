#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <filesystem>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// Extract image metadata from files (DateTimeOriginal, City, Location, Description, Keywords, GPS, Orientation)
// Reads files and extracts metadata using EXIF, XMP, and IPTC parsers
// Returns results in same format as extractExiftoolData: map<image_path, metadata json>
// Parameters: imagePaths - vector of image file paths to process
// Returns: map where key is image path and value is json object with metadata fields
std::map<fs::path, json> extractImageMetadata(const std::vector<fs::path>& imagePaths);
