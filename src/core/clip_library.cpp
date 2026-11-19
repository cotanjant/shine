#include "shine/core/clip_library.hpp"

#include "shine/core/mp4_validator.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace shine::core {

namespace {

[[nodiscard]] std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

[[nodiscard]] std::int64_t to_epoch_seconds(std::filesystem::file_time_type ftime) noexcept {
    const auto nowFile = std::filesystem::file_time_type::clock::now();
    const auto nowSys = std::chrono::system_clock::now();
    const auto sysTime = std::chrono::time_point_cast<std::chrono::seconds>(
        ftime - nowFile + nowSys);
    return static_cast<std::int64_t>(sysTime.time_since_epoch().count());
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

[[nodiscard]] std::string build_display_title(std::string_view stem, bool isClip, std::int64_t epochSec) {
    std::time_t tt = static_cast<std::time_t>(epochSec);
    std::tm tmLocal{};
#ifdef _WIN32
    localtime_s(&tmLocal, &tt);
#else
    localtime_r(&tt, &tmLocal);
#endif
    if (stem.rfind("shine_", 0) == 0) {
        std::ostringstream oss;
        oss << (isClip ? "Clip · " : "Recording · ")
            << std::put_time(&tmLocal, "%b %d %H:%M");
        return oss.str();
    }
    return std::string(stem);
}

} // namespace

std::vector<ClipEntry> scan_clips(const std::filesystem::path& directory) {
    std::vector<ClipEntry> entries;
    std::error_code ec;
    if (directory.empty() || !std::filesystem::exists(directory, ec) || ec) {
        return entries;
    }

    for (const auto& item : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!item.is_regular_file(ec) || ec) {
            continue;
        }
        const auto ext = to_lower(path_to_utf8(item.path().extension()));
        if (ext != ".mp4") {
            continue;
        }

        const Mp4Report report = inspect_mp4(item.path());
        if (!report.valid) {
            continue;
        }

        ClipEntry entry{};
        entry.path = item.path();
        entry.fileName = path_to_utf8(item.path().filename());
        entry.fileSizeBytes = report.fileSizeBytes;
        const auto ftime = item.last_write_time(ec);
        entry.modifiedEpochSec = ec ? 0 : to_epoch_seconds(ftime);
        entry.durationSeconds = report.durationSeconds;
        entry.videoFrames = report.videoFrameCount;
        entry.codecFourCC = report.videoCodecFourCC;
        entry.hasAudio = report.hasAudioTrack;
        entry.isClip = (entry.fileName.rfind("shine_clip_", 0) == 0);
        entry.displayTitle = build_display_title(
            path_to_utf8(item.path().stem()), entry.isClip, entry.modifiedEpochSec);

        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const ClipEntry& a, const ClipEntry& b) {
        if (a.modifiedEpochSec != b.modifiedEpochSec) {
            return a.modifiedEpochSec > b.modifiedEpochSec;
        }
        return a.fileName > b.fileName;
    });

    return entries;
}

Result<void> delete_clip(const std::filesystem::path& path) {
    if (path.empty()) {
        return Result<void>::make_err(ErrorCode::InvalidArgument, "Clip path must not be empty");
    }
    const auto ext = to_lower(path_to_utf8(path.extension()));
    if (ext != ".mp4") {
        return Result<void>::make_err(ErrorCode::InvalidArgument, "Only .mp4 clips may be deleted");
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return Result<void>::make_err(ErrorCode::IoError, "Clip file does not exist");
    }

    if (!std::filesystem::remove(path, ec) || ec) {
        return Result<void>::make_err(
            ErrorCode::IoError, "Failed to delete clip: " + ec.message());
    }
    return Result<void>::make_ok();
}

std::filesystem::path generate_clip_filename(const std::filesystem::path& directory) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmLocal{};
#ifdef _WIN32
    localtime_s(&tmLocal, &tt);
#else
    localtime_r(&tt, &tmLocal);
#endif

    std::ostringstream oss;
    oss << "shine_clip_" << std::put_time(&tmLocal, "%Y%m%d_%H%M%S") << ".mp4";
    const std::filesystem::path baseDir = directory.empty() ? default_recordings_dir() : directory;
    return baseDir / oss.str();
}

} // namespace shine::core
