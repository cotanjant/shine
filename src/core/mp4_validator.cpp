#include "shine/core/mp4_validator.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>

namespace shine::core {

namespace {

[[nodiscard]] std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

[[nodiscard]] std::uint64_t read_be64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(read_be32(p)) << 32U) |
           static_cast<std::uint64_t>(read_be32(p + 4));
}

struct BoxHeader {
    std::uint64_t totalSize{0};
    std::uint64_t headerSize{0};
    std::array<char, 4> type{};
};

[[nodiscard]] bool read_box_header(
    std::ifstream& in,
    std::uint64_t offset,
    std::uint64_t parentEnd,
    BoxHeader& out) {
    if (offset + 8 > parentEnd) {
        return false;
    }

    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) {
        return false;
    }

    std::array<std::uint8_t, 8> buf{};
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (in.gcount() != 8) {
        return false;
    }

    const std::uint32_t size32 = read_be32(buf.data());
    out.type = {
        static_cast<char>(buf[4]),
        static_cast<char>(buf[5]),
        static_cast<char>(buf[6]),
        static_cast<char>(buf[7])
    };

    if (size32 == 1U) {
        if (offset + 16 > parentEnd) {
            return false;
        }
        std::array<std::uint8_t, 8> ext{};
        in.read(reinterpret_cast<char*>(ext.data()), static_cast<std::streamsize>(ext.size()));
        if (in.gcount() != 8) {
            return false;
        }
        out.totalSize = read_be64(ext.data());
        out.headerSize = 16;
    } else if (size32 == 0U) {
        out.totalSize = parentEnd - offset;
        out.headerSize = 8;
    } else {
        out.totalSize = size32;
        out.headerSize = 8;
    }

    if (out.totalSize < out.headerSize || offset + out.totalSize > parentEnd) {
        return false;
    }
    return true;
}

[[nodiscard]] bool type_eq(const std::array<char, 4>& type, const char (&lit)[5]) noexcept {
    return type[0] == lit[0] && type[1] == lit[1] && type[2] == lit[2] && type[3] == lit[3];
}

struct TrackParseState {
    bool isVideoTrack{false};
    bool isAudioTrack{false};
    std::uint32_t sampleCount{0};
    std::string codecFourCC;
};

void parse_container(
    std::ifstream& in,
    std::uint64_t start,
    std::uint64_t end,
    int depth,
    Mp4Report& report,
    TrackParseState* currentTrack) {
    if (depth > 8) {
        return;
    }

    std::uint64_t cursor = start;
    while (cursor + 8 <= end) {
        BoxHeader box{};
        if (!read_box_header(in, cursor, end, box)) {
            break;
        }

        const std::uint64_t payloadOffset = cursor + box.headerSize;
        const std::uint64_t payloadEnd = cursor + box.totalSize;
        const std::uint64_t payloadSize = payloadEnd - payloadOffset;

        if (type_eq(box.type, "ftyp")) {
            report.hasFtyp = true;
        } else if (type_eq(box.type, "mdat")) {
            report.hasMdat = true;
            report.mdatPayloadBytes += payloadSize;
        } else if (type_eq(box.type, "moov")) {
            report.hasMoov = true;
            parse_container(in, payloadOffset, payloadEnd, depth + 1, report, nullptr);
        } else if (type_eq(box.type, "trak")) {
            TrackParseState trakState{};
            parse_container(in, payloadOffset, payloadEnd, depth + 1, report, &trakState);
            if (trakState.isAudioTrack) {
                report.hasAudioTrack = true;
                if (!trakState.codecFourCC.empty()) {
                    report.audioCodecFourCC = trakState.codecFourCC;
                }
            } else if (trakState.isVideoTrack || (report.videoCodecFourCC.empty() && !trakState.codecFourCC.empty())) {
                if (!trakState.codecFourCC.empty()) {
                    report.videoCodecFourCC = trakState.codecFourCC;
                }
                if (trakState.sampleCount > 0) {
                    report.videoFrameCount = trakState.sampleCount;
                }
            }
        } else if (type_eq(box.type, "mdia") ||
                   type_eq(box.type, "minf") ||
                   type_eq(box.type, "stbl")) {
            parse_container(in, payloadOffset, payloadEnd, depth + 1, report, currentTrack);
        } else if (type_eq(box.type, "mvhd")) {
            if (payloadSize >= 20) {
                std::array<std::uint8_t, 32> mvhd{};
                const auto toRead = static_cast<std::streamsize>(
                    payloadSize < mvhd.size() ? payloadSize : mvhd.size());
                in.seekg(static_cast<std::streamoff>(payloadOffset), std::ios::beg);
                in.read(reinterpret_cast<char*>(mvhd.data()), toRead);
                if (in.gcount() >= 20) {
                    const std::uint8_t version = mvhd[0];
                    if (version == 0U && in.gcount() >= 20) {
                        report.timescale = read_be32(mvhd.data() + 12);
                        report.durationUnits = read_be32(mvhd.data() + 16);
                    } else if (version == 1U && in.gcount() >= 32) {
                        report.timescale = read_be32(mvhd.data() + 20);
                        report.durationUnits = read_be64(mvhd.data() + 24);
                    }
                    if (report.timescale > 0U) {
                        report.durationSeconds =
                            static_cast<double>(report.durationUnits) /
                            static_cast<double>(report.timescale);
                    }
                }
            }
        } else if (type_eq(box.type, "hdlr") && currentTrack != nullptr) {
            if (payloadSize >= 12) {
                std::array<std::uint8_t, 12> hdlr{};
                in.seekg(static_cast<std::streamoff>(payloadOffset), std::ios::beg);
                in.read(reinterpret_cast<char*>(hdlr.data()), 12);
                if (in.gcount() == 12) {
                    if (hdlr[8] == 'v' && hdlr[9] == 'i' && hdlr[10] == 'd' && hdlr[11] == 'e') {
                        currentTrack->isVideoTrack = true;
                    } else if (hdlr[8] == 's' && hdlr[9] == 'o' && hdlr[10] == 'u' && hdlr[11] == 'n') {
                        currentTrack->isAudioTrack = true;
                    }
                }
            }
        } else if (type_eq(box.type, "stsd") && currentTrack != nullptr) {
            if (payloadSize >= 16) {
                std::array<std::uint8_t, 16> stsd{};
                in.seekg(static_cast<std::streamoff>(payloadOffset), std::ios::beg);
                in.read(reinterpret_cast<char*>(stsd.data()), 16);
                if (in.gcount() == 16) {
                    const std::uint32_t entryCount = read_be32(stsd.data() + 4);
                    if (entryCount >= 1U) {
                        currentTrack->codecFourCC.assign(
                            reinterpret_cast<const char*>(stsd.data() + 12), 4);
                    }
                }
            }
        } else if (type_eq(box.type, "stsz") && currentTrack != nullptr) {
            if (payloadSize >= 12) {
                std::array<std::uint8_t, 12> stsz{};
                in.seekg(static_cast<std::streamoff>(payloadOffset), std::ios::beg);
                in.read(reinterpret_cast<char*>(stsz.data()), 12);
                if (in.gcount() == 12) {
                    currentTrack->sampleCount = read_be32(stsz.data() + 8);
                }
            }
        }

        cursor += box.totalSize;
    }
}

} // namespace

Mp4Report inspect_mp4(const std::filesystem::path& path) {
    Mp4Report report{};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        report.diagnostic = "File does not exist";
        return report;
    }

    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize == 0) {
        report.diagnostic = "File is empty or unreadable";
        return report;
    }
    report.fileSizeBytes = fileSize;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        report.diagnostic = "Failed to open MP4 file";
        return report;
    }

    parse_container(in, 0, fileSize, 0, report, nullptr);

    if (!report.hasFtyp) {
        report.diagnostic = "Missing ftyp box";
        return report;
    }
    if (!report.hasMoov) {
        report.diagnostic = "Missing moov box";
        return report;
    }
    if (!report.hasMdat || report.mdatPayloadBytes == 0) {
        report.diagnostic = "Missing or empty mdat box";
        return report;
    }
    if (report.videoCodecFourCC.empty()) {
        report.diagnostic = "No video codec entry found in stsd";
        return report;
    }
    if (report.videoFrameCount == 0) {
        report.diagnostic = "Zero video samples in stsz";
        return report;
    }
    if (report.durationSeconds <= 0.0) {
        report.diagnostic = "Zero duration in mvhd";
        return report;
    }

    report.valid = true;
    report.diagnostic = "OK";
    return report;
}

} // namespace shine::core
