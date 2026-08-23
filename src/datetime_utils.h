#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace datetime_utils {

std::optional<std::int64_t> exifTakenEpoch(const std::string &dateTimeOriginal, const std::string &offsetTimeOriginal);

std::optional<std::int64_t> getTakenEpochFromMetadata(const nlohmann::json &metadata);

} // namespace datetime_utils