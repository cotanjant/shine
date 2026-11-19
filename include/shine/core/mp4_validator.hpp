#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace shine::core {

struct Mp4Report {
    bool valid{false};
    std::uint64_t fileSizeBytes{0};
    bool hasFtyp{false};
    bool hasMoov{false};
    bool hasMdat{false};
    std::uint64_t mdatPayloadBytes{0};
    std::uint32_t timescale{0};
    std::uint64_t durationUnits{0};
    double durationSeconds{0.0};
    std::uint32_t videoFrameCount{0};
    std::string videoCodecFourCC;
    std::string diagnostic;
};

[[nodiscard]] Mp4Report inspect_mp4(const std::filesystem::path& path);

} // namespace shine::core
