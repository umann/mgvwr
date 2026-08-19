#include "datetime_utils.h"

#include <chrono>

namespace datetime_utils {
namespace {

constexpr std::int64_t kMinutesPerHour = 60;

bool parseExifDateTime(const std::string &value, std::chrono::sys_seconds &outInstant) {
    if (value.size() < 19 || value[4] != ':' || value[7] != ':' || value[10] != ' ' || value[13] != ':' ||
        value[16] != ':') {
        return false;
    }

    try {
        const int year = std::stoi(value.substr(0, 4));
        const unsigned month = static_cast<unsigned>(std::stoul(value.substr(5, 2)));
        const unsigned day = static_cast<unsigned>(std::stoul(value.substr(8, 2)));
        const int hour = std::stoi(value.substr(11, 2));
        const int minute = std::stoi(value.substr(14, 2));
        const int second = std::stoi(value.substr(17, 2));

        const std::chrono::year_month_day ymd{
            std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
        if (!ymd.ok()) {
            return false;
        }

        outInstant = std::chrono::sys_days{ymd} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
                     std::chrono::seconds{second};
        return true;
    } catch (...) {
        return false;
    }
}

std::int64_t parseExifOffsetMinutes(const std::string &value) {
    if (value.size() < 6 || (value[0] != '+' && value[0] != '-') || value[3] != ':') {
        return 0;
    }

    try {
        const std::int64_t hours = std::stoll(value.substr(1, 2));
        const std::int64_t minutes = std::stoll(value.substr(4, 2));
        const std::int64_t totalMinutes = hours * kMinutesPerHour + minutes;
        return value[0] == '-' ? -totalMinutes : totalMinutes;
    } catch (...) {
        return 0;
    }
}

} // namespace

std::optional<std::int64_t> exifTakenEpoch(const std::string &dateTimeOriginal,
                                           const std::string &offsetTimeOriginal) {
    if (dateTimeOriginal.empty() || dateTimeOriginal == "0000:00:00 00:00:00") {
        return std::nullopt;
    }

    std::chrono::sys_seconds localInstant{};
    if (!parseExifDateTime(dateTimeOriginal, localInstant)) {
        return std::nullopt;
    }

    const auto offset = std::chrono::minutes{parseExifOffsetMinutes(offsetTimeOriginal)};
    const auto takenInstant = localInstant - offset;
    return takenInstant.time_since_epoch().count();
}

std::optional<std::int64_t> getTakenEpochFromMetadata(const nlohmann::json &metadata) {
    if (!metadata.is_object()) {
        return std::nullopt;
    }

    if (!metadata.contains("DateTimeOriginal") || !metadata["DateTimeOriginal"].is_string()) {
        return std::nullopt;
    }

    const std::string dateTimeOriginal = metadata["DateTimeOriginal"].get<std::string>();
    std::string offsetTimeOriginal = "+00:00";
    if (metadata.contains("OffsetTimeOriginal") && metadata["OffsetTimeOriginal"].is_string()) {
        const std::string candidate = metadata["OffsetTimeOriginal"].get<std::string>();
        if (!candidate.empty()) {
            offsetTimeOriginal = candidate;
        }
    }

    return exifTakenEpoch(dateTimeOriginal, offsetTimeOriginal);
}

} // namespace datetime_utils