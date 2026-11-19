#include "shine/core/replay_buffer.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace shine::core {

void ReplayRingBuffer::configure(std::uint32_t maxDurationSeconds, std::size_t maxBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxDurationSeconds_ = std::clamp<std::uint32_t>(maxDurationSeconds, 5U, 300U);
    maxBytes_ = std::max<std::size_t>(maxBytes, 1024ULL * 1024ULL);
    trim_locked();
}

void ReplayRingBuffer::set_video_format(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fps,
    std::uint32_t bitrateKbps,
    std::vector<std::uint8_t> codecConfig) {
    std::lock_guard<std::mutex> lock(mutex_);
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrateKbps_ = bitrateKbps;
    codecConfig_ = std::move(codecConfig);
}

void ReplayRingBuffer::push_video(EncodedVideoPacket pkt) {
    std::lock_guard<std::mutex> lock(mutex_);
    totalVideoBytes_ += pkt.data.size();
    videoPackets_.push_back(std::move(pkt));
    trim_locked();
}

void ReplayRingBuffer::push_audio(AudioPcmChunk chunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    totalAudioBytes_ += chunk.samples.size() * sizeof(std::int16_t);
    audioChunks_.push_back(std::move(chunk));
    trim_locked();
}

void ReplayRingBuffer::trim_locked() {
    const std::int64_t maxSpanUs = static_cast<std::int64_t>(maxDurationSeconds_ + 2U) * 1'000'000LL;

    while (videoPackets_.size() >= 2U) {
        const std::int64_t spanUs = videoPackets_.back().ptsUs - videoPackets_.front().ptsUs;
        const std::size_t totalBytes = totalVideoBytes_ + totalAudioBytes_;
        if (spanUs <= maxSpanUs && totalBytes <= maxBytes_) {
            break;
        }

        // Advance to the next keyframe so the ring buffer always begins on a clean IDR frame.
        std::size_t nextKfIdx = 1U;
        while (nextKfIdx < videoPackets_.size() && !videoPackets_[nextKfIdx].isKeyframe) {
            ++nextKfIdx;
        }
        if (nextKfIdx >= videoPackets_.size()) {
            // Only one GOP currently in buffer; do not drop it unless over double hard limit.
            if (totalBytes <= maxBytes_ * 2U) {
                break;
            }
            nextKfIdx = 1U;
        }

        for (std::size_t i = 0; i < nextKfIdx; ++i) {
            totalVideoBytes_ -= videoPackets_.front().data.size();
            videoPackets_.pop_front();
        }
    }

    if (!videoPackets_.empty()) {
        const std::int64_t minAudioPtsUs = videoPackets_.front().ptsUs - 500'000LL;
        while (!audioChunks_.empty()) {
            const auto& front = audioChunks_.front();
            const std::int64_t rate = front.sampleRate > 0 ? static_cast<std::int64_t>(front.sampleRate) : 48000LL;
            const std::int64_t ch = front.channels > 0 ? static_cast<std::int64_t>(front.channels) : 1LL;
            const std::int64_t frames = static_cast<std::int64_t>(front.samples.size()) / ch;
            const std::int64_t chunkEndUs = front.ptsUs + (frames * 1'000'000LL) / rate;
            if (chunkEndUs >= minAudioPtsUs) {
                break;
            }
            totalAudioBytes_ -= front.samples.size() * sizeof(std::int16_t);
            audioChunks_.pop_front();
        }
    } else if (!audioChunks_.empty()) {
        const std::int64_t audioCutoffUs = audioChunks_.back().ptsUs - (maxSpanUs + 1'000'000LL);
        while (!audioChunks_.empty() && audioChunks_.front().ptsUs < audioCutoffUs) {
            totalAudioBytes_ -= audioChunks_.front().samples.size() * sizeof(std::int16_t);
            audioChunks_.pop_front();
        }
    }
}

ClipSnapshot ReplayRingBuffer::snapshot_last(std::uint32_t seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    ClipSnapshot snap{};
    snap.width = width_;
    snap.height = height_;
    snap.fps = fps_;
    snap.bitrateKbps = bitrateKbps_;
    snap.codecConfig = codecConfig_;

    if (videoPackets_.empty()) {
        return snap;
    }

    const std::uint32_t clampedSec = std::clamp<std::uint32_t>(seconds, 1U, 300U);
    const std::int64_t newestPtsUs = videoPackets_.back().ptsUs;
    const std::int64_t targetStartUs = newestPtsUs - static_cast<std::int64_t>(clampedSec) * 1'000'000LL;

    std::optional<std::size_t> bestKf;
    std::optional<std::size_t> earliestKf;
    for (std::size_t i = 0; i < videoPackets_.size(); ++i) {
        if (!videoPackets_[i].isKeyframe) {
            continue;
        }
        if (!earliestKf.has_value()) {
            earliestKf = i;
        }
        if (videoPackets_[i].ptsUs <= targetStartUs) {
            bestKf = i;
        } else {
            break;
        }
    }

    const std::optional<std::size_t> startIdx = bestKf.has_value() ? bestKf : earliestKf;
    if (!startIdx.has_value()) {
        return snap;
    }

    const std::int64_t basePtsUs = videoPackets_[*startIdx].ptsUs;
    snap.videoPackets.reserve(videoPackets_.size() - *startIdx);
    for (std::size_t i = *startIdx; i < videoPackets_.size(); ++i) {
        EncodedVideoPacket pkt = videoPackets_[i];
        pkt.ptsUs = std::max<std::int64_t>(0, pkt.ptsUs - basePtsUs);
        snap.videoPackets.push_back(std::move(pkt));
    }

    for (const auto& chunk : audioChunks_) {
        const std::uint32_t rate = chunk.sampleRate > 0U ? chunk.sampleRate : 48000U;
        const std::uint16_t ch = chunk.channels > 0U ? chunk.channels : 1U;
        const std::size_t totalFrames = chunk.samples.size() / ch;
        const std::int64_t chunkDurUs =
            (static_cast<std::int64_t>(totalFrames) * 1'000'000LL) / static_cast<std::int64_t>(rate);
        const std::int64_t chunkEndUs = chunk.ptsUs + chunkDurUs;
        if (chunkEndUs <= basePtsUs) {
            continue;
        }

        AudioPcmChunk copy = chunk;
        if (copy.ptsUs < basePtsUs && totalFrames > 0U) {
            const std::int64_t trimUs = basePtsUs - copy.ptsUs;
            const std::size_t trimFrames = static_cast<std::size_t>(
                (trimUs * static_cast<std::int64_t>(rate)) / 1'000'000LL);
            const std::size_t trimSamples = std::min(trimFrames * ch, copy.samples.size());
            copy.samples.erase(copy.samples.begin(),
                               copy.samples.begin() + static_cast<std::ptrdiff_t>(trimSamples));
            copy.ptsUs = 0;
        } else {
            copy.ptsUs = std::max<std::int64_t>(0, copy.ptsUs - basePtsUs);
        }

        if (!copy.samples.empty()) {
            snap.audioChunks.push_back(std::move(copy));
        }
    }

    return snap;
}

std::uint32_t ReplayRingBuffer::buffered_seconds() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (videoPackets_.size() < 2U) {
        return 0U;
    }
    const std::int64_t spanUs = videoPackets_.back().ptsUs - videoPackets_.front().ptsUs;
    if (spanUs <= 0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(spanUs / 1'000'000LL);
}

std::size_t ReplayRingBuffer::memory_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalVideoBytes_ + totalAudioBytes_ + codecConfig_.size();
}

void ReplayRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    videoPackets_.clear();
    audioChunks_.clear();
    codecConfig_.clear();
    totalVideoBytes_ = 0;
    totalAudioBytes_ = 0;
}

} // namespace shine::core
