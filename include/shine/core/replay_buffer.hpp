#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace shine::core {

struct EncodedVideoPacket {
    std::vector<std::uint8_t> data;
    std::int64_t ptsUs{0};
    std::int64_t durationUs{0};
    bool isKeyframe{false};
};

struct AudioPcmChunk {
    std::vector<std::int16_t> samples;
    std::int64_t ptsUs{0};
    std::uint32_t sampleRate{48000};
    std::uint16_t channels{1};
};

struct ClipSnapshot {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t fps{60};
    std::uint32_t bitrateKbps{8000};
    std::vector<std::uint8_t> codecConfig;
    std::vector<EncodedVideoPacket> videoPackets;
    std::vector<AudioPcmChunk> audioChunks;
};

class ReplayRingBuffer {
public:
    ReplayRingBuffer() = default;

    void configure(
        std::uint32_t maxDurationSeconds,
        std::size_t maxBytes = 256ULL * 1024ULL * 1024ULL);

    void set_video_format(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fps,
        std::uint32_t bitrateKbps,
        std::vector<std::uint8_t> codecConfig);

    void push_video(EncodedVideoPacket pkt);
    void push_audio(AudioPcmChunk chunk);

    [[nodiscard]] ClipSnapshot snapshot_last(std::uint32_t seconds) const;
    [[nodiscard]] std::uint32_t buffered_seconds() const noexcept;
    [[nodiscard]] std::size_t memory_bytes() const noexcept;
    void clear();

private:
    void trim_locked();

    mutable std::mutex mutex_;
    std::uint32_t maxDurationSeconds_{30};
    std::size_t maxBytes_{256ULL * 1024ULL * 1024ULL};

    std::uint32_t width_{0};
    std::uint32_t height_{0};
    std::uint32_t fps_{60};
    std::uint32_t bitrateKbps_{8000};
    std::vector<std::uint8_t> codecConfig_;

    std::deque<EncodedVideoPacket> videoPackets_;
    std::deque<AudioPcmChunk> audioChunks_;
    std::size_t totalVideoBytes_{0};
    std::size_t totalAudioBytes_{0};
};

} // namespace shine::core
