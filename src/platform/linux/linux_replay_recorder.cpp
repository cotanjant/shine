#include "platform/linux/linux_replay_recorder.hpp"

#include "platform/linux/linux_audio.hpp"
#include "platform/linux/linux_raii.hpp"
#include "platform/linux/x11_capture.hpp"
#include "shine/core/clip_library.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <vector>

namespace shine::platform::linux_impl {

namespace {

struct VaapiNode {
    std::string drmNode;
    std::string vendor;
};

[[nodiscard]] bool vaapi_has_h264_enc(VADisplay vaDpy, VAProfile profile) noexcept {
    int n = ::vaMaxNumEntrypoints(vaDpy);
    if (n <= 0) {
        return false;
    }
    std::vector<VAEntrypoint> eps(static_cast<std::size_t>(n));
    if (::vaQueryConfigEntrypoints(vaDpy, profile, eps.data(), &n) != VA_STATUS_SUCCESS) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (eps[static_cast<std::size_t>(i)] == VAEntrypointEncSlice ||
            eps[static_cast<std::size_t>(i)] == VAEntrypointEncSliceLP) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<VaapiNode> probe_vaapi_node() {
    for (int idx = 128; idx <= 135; ++idx) {
        const std::string node = "/dev/dri/renderD" + std::to_string(idx);
        VaDisplayGuard va;
        if (!va.open(node.c_str())) {
            continue;
        }
        if (vaapi_has_h264_enc(va.get(), VAProfileH264High) ||
            vaapi_has_h264_enc(va.get(), VAProfileH264Main)) {
            const char* vendor = ::vaQueryVendorString(va.get());
            return VaapiNode{node, vendor != nullptr ? std::string(vendor) : "VA-API"};
        }
    }
    return std::nullopt;
}

[[nodiscard]] core::Result<void> save_clip_mp4_linux(
    const core::ClipSnapshot& snap,
    AVCodecID videoCodecId,
    const std::filesystem::path& outputPath) {
    if (snap.videoPackets.empty() || snap.width < 2U || snap.height < 2U) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Replay buffer contains no video frames yet");
    }

    std::error_code ec;
    const auto parent = outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    AVFormatContext* rawFmt = nullptr;
    int err = ::avformat_alloc_output_context2(&rawFmt, nullptr, "mp4", outputPath.c_str());
    if (err < 0 || rawFmt == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "avformat_alloc_output_context2 failed: " + av_error_string(err));
    }
    AvFormatOutPtr fmtCtx(rawFmt);

    AVStream* vStream = ::avformat_new_stream(fmtCtx.get(), nullptr);
    if (vStream == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "Failed to create clip video stream");
    }
    vStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vStream->codecpar->codec_id = videoCodecId;
    vStream->codecpar->width = static_cast<int>(snap.width);
    vStream->codecpar->height = static_cast<int>(snap.height);
    vStream->codecpar->format = AV_PIX_FMT_YUV420P;
    vStream->time_base = AVRational{1, 1000000};
    vStream->avg_frame_rate = AVRational{static_cast<int>(snap.fps), 1};

    if (!snap.codecConfig.empty()) {
        vStream->codecpar->extradata = static_cast<uint8_t*>(
            ::av_mallocz(snap.codecConfig.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (vStream->codecpar->extradata != nullptr) {
            std::memcpy(vStream->codecpar->extradata, snap.codecConfig.data(), snap.codecConfig.size());
            vStream->codecpar->extradata_size = static_cast<int>(snap.codecConfig.size());
        }
    }

    AVStream* aStream = nullptr;
    AvCodecCtxPtr aCodecCtx;
    AvFramePtr aFrame;
    if (!snap.audioChunks.empty()) {
        const AVCodec* aac = ::avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (aac != nullptr) {
            AvCodecCtxPtr cand(::avcodec_alloc_context3(aac));
            if (cand) {
                cand->sample_rate = 48000;
                cand->bit_rate = 128000;
                cand->sample_fmt = AV_SAMPLE_FMT_FLTP;
                cand->time_base = AVRational{1, 48000};
#if LIBAVCODEC_VERSION_MAJOR >= 60
                AVChannelLayout monoLayout = AV_CHANNEL_LAYOUT_MONO;
                ::av_channel_layout_copy(&cand->ch_layout, &monoLayout);
#else
                cand->channels = 1;
                cand->channel_layout = AV_CH_LAYOUT_MONO;
#endif
                if ((fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
                    cand->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                }
                if (::avcodec_open2(cand.get(), aac, nullptr) == 0) {
                    aStream = ::avformat_new_stream(fmtCtx.get(), nullptr);
                    if (aStream != nullptr &&
                        ::avcodec_parameters_from_context(aStream->codecpar, cand.get()) == 0) {
                        aStream->time_base = cand->time_base;
                        aCodecCtx = std::move(cand);
                        aFrame.reset(::av_frame_alloc());
                    } else {
                        aStream = nullptr;
                    }
                }
            }
        }
    }

    if ((fmtCtx->oformat->flags & AVFMT_NOFILE) == 0) {
        err = ::avio_open(&fmtCtx->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::IoError, "avio_open failed: " + av_error_string(err));
        }
    }

    AVDictionary* muxOpts = nullptr;
    ::av_dict_set(&muxOpts, "movflags", "+faststart", 0);
    err = ::avformat_write_header(fmtCtx.get(), &muxOpts);
    ::av_dict_free(&muxOpts);
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError, "avformat_write_header failed: " + av_error_string(err));
    }

    AvPacketPtr pkt(::av_packet_alloc());
    if (!pkt) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "av_packet_alloc failed");
    }

    std::int64_t lastVideoPts = -1;
    for (const auto& vp : snap.videoPackets) {
        if (vp.data.empty()) {
            continue;
        }
        err = ::av_new_packet(pkt.get(), static_cast<int>(vp.data.size()));
        if (err < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed, "av_new_packet failed");
        }
        std::memcpy(pkt->data, vp.data.data(), vp.data.size());

        std::int64_t pts = std::max<std::int64_t>(0, vp.ptsUs);
        if (pts <= lastVideoPts) {
            pts = lastVideoPts + 1;
        }
        lastVideoPts = pts;

        pkt->pts = pts;
        pkt->dts = pts;
        pkt->duration = std::max<std::int64_t>(1, vp.durationUs);
        pkt->stream_index = vStream->index;
        pkt->flags = vp.isKeyframe ? AV_PKT_FLAG_KEY : 0;

        ::av_packet_rescale_ts(pkt.get(), AVRational{1, 1000000}, vStream->time_base);
        err = ::av_interleaved_write_frame(fmtCtx.get(), pkt.get());
        ::av_packet_unref(pkt.get());
        if (err < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::IoError, "av_interleaved_write_frame(video) failed: " + av_error_string(err));
        }
    }

    if (aStream != nullptr && aCodecCtx && aFrame) {
        const int frameSamples = aCodecCtx->frame_size > 0 ? aCodecCtx->frame_size : 1024;
        std::vector<float> pendingF32;
        for (const auto& chunk : snap.audioChunks) {
            pendingF32.reserve(pendingF32.size() + chunk.samples.size());
            for (const std::int16_t s : chunk.samples) {
                pendingF32.push_back(static_cast<float>(s) / 32768.0f);
            }
        }

        const auto encode_audio_frame = [&](const float* samples, int count, std::int64_t ptsSamples) {
            ::av_frame_unref(aFrame.get());
            aFrame->nb_samples = count;
            aFrame->format = aCodecCtx->sample_fmt;
            aFrame->sample_rate = aCodecCtx->sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 60
            ::av_channel_layout_copy(&aFrame->ch_layout, &aCodecCtx->ch_layout);
#else
            aFrame->channels = aCodecCtx->channels;
            aFrame->channel_layout = aCodecCtx->channel_layout;
#endif
            if (::av_frame_get_buffer(aFrame.get(), 0) < 0) {
                return;
            }
            if (::av_frame_make_writable(aFrame.get()) < 0) {
                return;
            }
            std::memcpy(aFrame->data[0], samples, static_cast<std::size_t>(count) * sizeof(float));
            aFrame->pts = ptsSamples;

            if (::avcodec_send_frame(aCodecCtx.get(), aFrame.get()) >= 0) {
                while (::avcodec_receive_packet(aCodecCtx.get(), pkt.get()) == 0) {
                    ::av_packet_rescale_ts(pkt.get(), aCodecCtx->time_base, aStream->time_base);
                    pkt->stream_index = aStream->index;
                    (void)::av_interleaved_write_frame(fmtCtx.get(), pkt.get());
                    ::av_packet_unref(pkt.get());
                }
            }
        };

        std::size_t offset = 0;
        std::int64_t ptsSamples = 0;
        while (offset + static_cast<std::size_t>(frameSamples) <= pendingF32.size()) {
            encode_audio_frame(pendingF32.data() + offset, frameSamples, ptsSamples);
            offset += static_cast<std::size_t>(frameSamples);
            ptsSamples += frameSamples;
        }
        if (offset < pendingF32.size()) {
            std::vector<float> tail(static_cast<std::size_t>(frameSamples), 0.0f);
            std::memcpy(tail.data(), pendingF32.data() + offset,
                        (pendingF32.size() - offset) * sizeof(float));
            encode_audio_frame(tail.data(), frameSamples, ptsSamples);
        }

        if (::avcodec_send_frame(aCodecCtx.get(), nullptr) >= 0) {
            while (::avcodec_receive_packet(aCodecCtx.get(), pkt.get()) == 0) {
                ::av_packet_rescale_ts(pkt.get(), aCodecCtx->time_base, aStream->time_base);
                pkt->stream_index = aStream->index;
                (void)::av_interleaved_write_frame(fmtCtx.get(), pkt.get());
                ::av_packet_unref(pkt.get());
            }
        }
    }

    err = ::av_write_trailer(fmtCtx.get());
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError, "av_write_trailer failed: " + av_error_string(err));
    }

    return core::Result<void>::make_ok();
}

} // namespace

LinuxReplayRecorder::~LinuxReplayRecorder() {
    (void)stop();
}

core::Result<void> LinuxReplayRecorder::start(const core::ReplayConfig& config) {
    auto validRes = config.validate();
    if (!validRes) {
        return validRes;
    }

    if (running_.load(std::memory_order_acquire)) {
        return core::Result<void>::make_err(
            core::ErrorCode::AlreadyRunning, "Replay buffer is already running");
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    defaultClipSeconds_ = config.clipLengthSeconds;
    outputDir_ = config.outputDirectory.empty()
                     ? core::default_recordings_dir()
                     : config.outputDirectory;

    ringBuffer_.clear();
    ringBuffer_.configure(config.clipLengthSeconds);

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = core::RecordingStats{};
        stats_.state = core::RecorderState::Idle;
        stats_.replayActive = true;
        stats_.memoryUsageMb = core::current_process_memory_mb();
    }

    stopRequested_.store(false, std::memory_order_release);
    std::promise<core::Result<void>> initPromise;
    auto initFuture = initPromise.get_future();

    workerThread_ = std::thread(
        &LinuxReplayRecorder::worker_main, this, config, std::move(initPromise));

    core::Result<void> initRes = initFuture.get();
    if (!initRes) {
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        running_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.replayActive = false;
        return initRes;
    }

    return core::Result<void>::make_ok();
}

core::Result<void> LinuxReplayRecorder::stop() {
    if (!running_.load(std::memory_order_acquire) && !workerThread_.joinable()) {
        return core::Result<void>::make_ok();
    }

    stopRequested_.store(true, std::memory_order_release);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    running_.store(false, std::memory_order_release);
    ringBuffer_.clear();

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.replayActive = false;
    stats_.replayBufferedSec = 0U;
    stats_.voiceActive = false;
    stats_.memoryUsageMb = core::current_process_memory_mb();
    return core::Result<void>::make_ok();
}

core::Result<std::filesystem::path> LinuxReplayRecorder::save_clip(
    const std::filesystem::path& outputPath,
    std::uint32_t seconds) {
    if (!running_.load(std::memory_order_acquire)) {
        return core::Result<std::filesystem::path>::make_err(
            core::ErrorCode::NotRunning, "Replay buffer is not running");
    }

    std::lock_guard<std::mutex> saveLock(saveMutex_);
    const std::uint32_t targetSec = seconds > 0U ? seconds : defaultClipSeconds_;
    const core::ClipSnapshot snap = ringBuffer_.snapshot_last(targetSec);
    if (snap.videoPackets.empty()) {
        return core::Result<std::filesystem::path>::make_err(
            core::ErrorCode::NotRunning, "Replay buffer does not have a complete GOP yet");
    }

    const std::filesystem::path dest =
        outputPath.empty() ? core::generate_clip_filename(outputDir_) : outputPath;

    auto muxRes = save_clip_mp4_linux(snap, videoCodecId_, dest);
    if (!muxRes) {
        return core::Result<std::filesystem::path>::make_err(muxRes.error());
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.lastClipPath = dest;
        ++stats_.clipsSavedCount;
    }
    return core::Result<std::filesystem::path>::make_ok(dest);
}

core::RecordingStats LinuxReplayRecorder::stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    core::RecordingStats copy = stats_;
    copy.replayBufferedSec = ringBuffer_.buffered_seconds();
    copy.memoryUsageMb = core::current_process_memory_mb();
    return copy;
}

bool LinuxReplayRecorder::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void LinuxReplayRecorder::worker_main(
    core::ReplayConfig config,
    std::promise<core::Result<void>> initPromise) {
    X11Capture capture;
    auto capRes = capture.init(config.displayIndex, config.captureCursor);
    if (!capRes) {
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.replayActive = false;
            stats_.lastError = capRes.error().message;
        }
        initPromise.set_value(std::move(capRes));
        return;
    }

    const std::uint32_t encodeW = capture.width() & ~1U;
    const std::uint32_t encodeH = capture.height() & ~1U;
    if (encodeW < 2U || encodeH < 2U) {
        auto err = core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Capture dimensions are too small");
        initPromise.set_value(std::move(err));
        return;
    }

    AvBufferRefPtr hwDeviceCtx;
    AvCodecCtxPtr codecCtx;
    AvFramePtr swFrame;
    AvFramePtr hwFrame;
    SwsCtxPtr swsCtx;
    bool gpuAccelerated = false;
    std::string encoderName;

    if (config.preferGpu) {
        if (const auto node = probe_vaapi_node(); node.has_value()) {
            const AVCodec* vaCodec = ::avcodec_find_encoder_by_name("h264_vaapi");
            AVBufferRef* rawDev = nullptr;
            if (vaCodec != nullptr &&
                ::av_hwdevice_ctx_create(
                    &rawDev, AV_HWDEVICE_TYPE_VAAPI, node->drmNode.c_str(), nullptr, 0) >= 0 &&
                rawDev != nullptr) {
                hwDeviceCtx.reset(rawDev);
                AvCodecCtxPtr ctx(::avcodec_alloc_context3(vaCodec));
                if (ctx) {
                    ctx->width = static_cast<int>(encodeW);
                    ctx->height = static_cast<int>(encodeH);
                    ctx->time_base = AVRational{1, static_cast<int>(config.fps)};
                    ctx->framerate = AVRational{static_cast<int>(config.fps), 1};
                    ctx->sample_aspect_ratio = AVRational{1, 1};
                    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
                    ctx->bit_rate = static_cast<int64_t>(config.bitrateKbps) * 1000;
                    ctx->gop_size = static_cast<int>(config.fps);
                    ctx->max_b_frames = 0;
                    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                    AvBufferRefPtr framesRef(::av_hwframe_ctx_alloc(hwDeviceCtx.get()));
                    if (framesRef) {
                        auto* fc = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
                        fc->format = AV_PIX_FMT_VAAPI;
                        fc->sw_format = AV_PIX_FMT_NV12;
                        fc->width = static_cast<int>(encodeW);
                        fc->height = static_cast<int>(encodeH);
                        fc->initial_pool_size = 6;
                        if (::av_hwframe_ctx_init(framesRef.get()) >= 0) {
                            ctx->hw_frames_ctx = ::av_buffer_ref(framesRef.get());
                            if (::avcodec_open2(ctx.get(), vaCodec, nullptr) >= 0) {
                                codecCtx = std::move(ctx);
                                gpuAccelerated = true;
                                encoderName = "VA-API H.264 (" + node->vendor + ")";
                            }
                        }
                    }
                }
            }
        }
    }

    if (!codecCtx) {
        hwDeviceCtx.reset();
        const AVCodec* swCodec = ::avcodec_find_encoder_by_name("libx264");
        if (swCodec == nullptr) {
            swCodec = ::avcodec_find_encoder_by_name("libopenh264");
        }
        if (swCodec == nullptr) {
            swCodec = ::avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        if (swCodec == nullptr) {
            swCodec = ::avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        }
        if (swCodec == nullptr) {
            initPromise.set_value(core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed, "No suitable video encoder found"));
            return;
        }

        codecCtx.reset(::avcodec_alloc_context3(swCodec));
        if (!codecCtx) {
            initPromise.set_value(core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed, "avcodec_alloc_context3 failed"));
            return;
        }
        codecCtx->width = static_cast<int>(encodeW);
        codecCtx->height = static_cast<int>(encodeH);
        codecCtx->time_base = AVRational{1, static_cast<int>(config.fps)};
        codecCtx->framerate = AVRational{static_cast<int>(config.fps), 1};
        codecCtx->sample_aspect_ratio = AVRational{1, 1};
        codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        codecCtx->bit_rate = static_cast<int64_t>(config.bitrateKbps) * 1000;
        codecCtx->gop_size = static_cast<int>(config.fps);
        codecCtx->max_b_frames = 0;
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (std::string_view(swCodec->name) == "libx264") {
            ::av_opt_set(codecCtx->priv_data, "preset", "veryfast", 0);
            ::av_opt_set(codecCtx->priv_data, "tune", "zerolatency", 0);
        }

        const int openErr = ::avcodec_open2(codecCtx.get(), swCodec, nullptr);
        if (openErr < 0) {
            initPromise.set_value(core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed,
                "avcodec_open2 failed: " + av_error_string(openErr)));
            return;
        }
        encoderName = std::string("FFmpeg ") + swCodec->name;
    }

    videoCodecId_ = codecCtx->codec_id;

    swFrame.reset(::av_frame_alloc());
    hwFrame.reset(::av_frame_alloc());
    AvPacketPtr pkt(::av_packet_alloc());
    if (!swFrame || !hwFrame || !pkt) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "Failed to allocate FFmpeg frame/packet"));
        return;
    }

    const AVPixelFormat swPixFmt = gpuAccelerated ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    swFrame->format = swPixFmt;
    swFrame->width = static_cast<int>(encodeW);
    swFrame->height = static_cast<int>(encodeH);
    if (::av_frame_get_buffer(swFrame.get(), 32) < 0) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "av_frame_get_buffer failed"));
        return;
    }

    swsCtx.reset(::sws_getContext(
        static_cast<int>(capture.width()),
        static_cast<int>(capture.height()),
        AV_PIX_FMT_BGRA,
        static_cast<int>(encodeW),
        static_cast<int>(encodeH),
        swPixFmt,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr));
    if (!swsCtx) {
        initPromise.set_value(core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "sws_getContext failed"));
        return;
    }

    std::vector<std::uint8_t> extradata;
    if (codecCtx->extradata != nullptr && codecCtx->extradata_size > 0) {
        extradata.assign(codecCtx->extradata, codecCtx->extradata + codecCtx->extradata_size);
    }
    ringBuffer_.set_video_format(encodeW, encodeH, config.fps, config.bitrateKbps, std::move(extradata));

    const auto startTp = std::chrono::steady_clock::now();

    AlsaMicCapture micCapture;
    bool micRunning = false;
    if (config.voiceEnabled) {
        auto micRes = micCapture.start(
            [this](core::AudioPcmChunk chunk) {
                ringBuffer_.push_audio(std::move(chunk));
            },
            startTp,
            config.voicePttEnabled,
            config.voicePttHotkey);
        micRunning = micRes.ok();
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.replayActive = true;
        stats_.voiceActive = micRunning;
        stats_.gpuAccelerated = gpuAccelerated;
        stats_.gpuBackend = gpuAccelerated ? core::GpuBackend::LinuxVaApi : core::GpuBackend::None;
        stats_.encoderName = encoderName;
        stats_.memoryUsageMb = core::current_process_memory_mb();
    }

    running_.store(true, std::memory_order_release);
    initPromise.set_value(core::Result<void>::make_ok());

    const auto frameInterval = std::chrono::nanoseconds(1'000'000'000LL / config.fps);
    auto nextTick = startTp;
    std::int64_t ptsIndex = 0;
    std::uint64_t encodedCount = 0;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            std::this_thread::sleep_until(nextTick);
            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }
            now = std::chrono::steady_clock::now();
        }

        auto capFrame = capture.capture_frame();
        if (!capFrame) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.lastError = capFrame.error().message;
            break;
        }

        if (::av_frame_make_writable(swFrame.get()) < 0) {
            break;
        }
        const uint8_t* const srcSlice[1] = {capture.bgra_data()};
        const int srcStride[1] = {static_cast<int>(capture.stride_bytes())};
        ::sws_scale(
            swsCtx.get(),
            srcSlice,
            srcStride,
            0,
            static_cast<int>(capture.height()),
            swFrame->data,
            swFrame->linesize);

        AVFrame* submitFrame = swFrame.get();
        if (gpuAccelerated) {
            ::av_frame_unref(hwFrame.get());
            if (::av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame.get(), 0) < 0 ||
                ::av_hwframe_transfer_data(hwFrame.get(), swFrame.get(), 0) < 0) {
                break;
            }
            submitFrame = hwFrame.get();
        }

        const auto elapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - startTp).count();
        std::int64_t targetPts =
            (elapsedNs * static_cast<std::int64_t>(config.fps)) / 1'000'000'000LL;
        if (targetPts < ptsIndex) {
            targetPts = ptsIndex;
        }
        submitFrame->pts = targetPts;
        ptsIndex = targetPts + 1;

        if (::avcodec_send_frame(codecCtx.get(), submitFrame) >= 0) {
            while (::avcodec_receive_packet(codecCtx.get(), pkt.get()) == 0) {
                core::EncodedVideoPacket ep{};
                ep.data.assign(pkt->data, pkt->data + pkt->size);
                ep.ptsUs = ::av_rescale_q(pkt->pts, codecCtx->time_base, AVRational{1, 1000000});
                ep.durationUs = std::max<std::int64_t>(
                    1,
                    ::av_rescale_q(
                        pkt->duration > 0 ? pkt->duration : 1,
                        codecCtx->time_base,
                        AVRational{1, 1000000}));
                ep.isKeyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                ::av_packet_unref(pkt.get());

                ringBuffer_.push_video(std::move(ep));
                ++encodedCount;
            }
        }

        nextTick += frameInterval;
        if (nextTick + frameInterval * 4 < now) {
            nextTick = now + frameInterval;
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.framesEncoded = encodedCount;
            stats_.replayBufferedSec = ringBuffer_.buffered_seconds();
            stats_.memoryUsageMb = core::current_process_memory_mb();
        }
    }

    micCapture.stop();
    running_.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.replayActive = false;
    stats_.voiceActive = false;
    stats_.replayBufferedSec = 0U;
    stats_.memoryUsageMb = core::current_process_memory_mb();
}

} // namespace shine::platform::linux_impl

namespace shine::core {

std::unique_ptr<IReplayRecorder> create_replay_recorder() {
    return std::make_unique<platform::linux_impl::LinuxReplayRecorder>();
}

} // namespace shine::core
