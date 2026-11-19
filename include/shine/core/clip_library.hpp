#pragma once

#include "shine/core/recorder.hpp"
#include "shine/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shine::core {

struct ClipEntry {
    std::filesystem::path path;
    std::string fileName;
    std::string displayTitle;
    std::uint64_t fileSizeBytes{0};
    std::int64_t modifiedEpochSec{0};
    double durationSeconds{0.0};
    std::uint32_t videoFrames{0};
    std::string codecFourCC;
    bool hasAudio{false};
    bool isClip{false};
};

[[nodiscard]] std::vector<ClipEntry> scan_clips(const std::filesystem::path& directory);
[[nodiscard]] Result<void> delete_clip(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path generate_clip_filename(
    const std::filesystem::path& directory = default_recordings_dir());

} // namespace shine::core
