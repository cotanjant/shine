#include "platform/linux/vaapi_encoder.hpp"

extern "C" {
#include <libavutil/opt.h>
}

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace shine::platform::linux_impl {

namespace {

struct VaapiProbeResult {
    std::string drmNode;
    std::string vendor;
};

[[nodiscard]] bool profile_has_encode_entrypoint(VADisplay vaDpy, VAProfile profile) noexcept {
    int numEntrypoints = ::vaMaxNumEntrypoints(vaDpy);
    if (numEntrypoints <= 0) {
        return false;
    }
    std::vector<VAEntrypoint> entrypoints(static_cast<std::size_t>(numEntrypoints));
    if (::vaQueryConfigEntrypoints(vaDpy, profile, entrypoints.data(), &numEntrypoints) != VA_STATUS_SUCCESS) {
        return false;
    }
    for (int i = 0; i < numEntrypoints; ++i) {
        if (entrypoints[static_cast<std::size_t>(i)] == VAEntrypointEncSlice ||
            entrypoints[static_cast<std::size_t>(i)] == VAEntrypointEncSliceLP) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<VaapiProbeResult> probe_vaapi_h264_device() {
    for (int idx = 128; idx <= 135; ++idx) {
        const std::string node = "/dev/dri/renderD" + std::to_string(idx);
        VaDisplayGuard va;
        if (!va.open(node.c_str())) {
            continue;
        }
        if (profile_has_encode_entrypoint(va.get(), VAProfileH264High) ||
            profile_has_encode_entrypoint(va.get(), VAProfileH264Main)) {
            const char* vendorPtr = ::vaQueryVendorString(va.get());
            return VaapiProbeResult{
                node,
                vendorPtr != nullptr ? std::string(vendorPtr) : "VA-API"
            };
        }
    }
    return std::nullopt;
}

} // namespace

VaapiH264Encoder::~VaapiH264Encoder() {
    (void)finalize();
    swsCtx_.reset();
    pkt_.reset();
    hwFrame_.reset();
    swFrame_.reset();
    codecCtx_.reset();
    hwDeviceCtx_.reset();
    fmtCtx_.reset();
}

core::Result<void> VaapiH264Encoder::init(
    const std::filesystem::path& outputPath,
    std::uint32_t captureWidth,
    std::uint32_t captureHeight,
    std::uint32_t fps,
    std::uint32_t bitrateKbps,
    bool preferGpu) {
    captureWidth_ = captureWidth;
    captureHeight_ = captureHeight;
    encodeWidth_ = captureWidth & ~1U;
    encodeHeight_ = captureHeight & ~1U;
    fps_ = fps;
    bitrateKbps_ = bitrateKbps;
    finalized_ = false;
    headerWritten_ = false;
    framesWritten_ = 0;

    if (encodeWidth_ < 2U || encodeHeight_ < 2U) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Capture dimensions are too small");
    }

    std::error_code ec;
    const auto parent = outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    if (preferGpu) {
        if (const auto probe = probe_vaapi_h264_device(); probe.has_value()) {
            auto vaRes = init_vaapi(outputPath, probe->drmNode, probe->vendor);
            if (vaRes) {
                return vaRes;
            }
            swsCtx_.reset();
            pkt_.reset();
            hwFrame_.reset();
            swFrame_.reset();
            codecCtx_.reset();
            hwDeviceCtx_.reset();
            fmtCtx_.reset();
        }
    }

    return init_software(outputPath);
}

core::Result<void> VaapiH264Encoder::open_muxer(const std::filesystem::path& outputPath) {
    AVFormatContext* rawFmt = nullptr;
    const int fmtErr = ::avformat_alloc_output_context2(
        &rawFmt, nullptr, "mp4", outputPath.c_str());
    if (fmtErr < 0 || rawFmt == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "avformat_alloc_output_context2 failed: " + av_error_string(fmtErr));
    }
    fmtCtx_.reset(rawFmt);

    videoStream_ = ::avformat_new_stream(fmtCtx_.get(), nullptr);
    if (videoStream_ == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "avformat_new_stream failed");
    }
    videoStream_->time_base = AVRational{1, static_cast<int>(fps_)};
    videoStream_->avg_frame_rate = AVRational{static_cast<int>(fps_), 1};
    return core::Result<void>::make_ok();
}

core::Result<void> VaapiH264Encoder::init_vaapi(
    const std::filesystem::path& outputPath,
    const std::string& drmNode,
    const std::string& vendor) {
    const AVCodec* codec = ::avcodec_find_encoder_by_name("h264_vaapi");
    if (codec == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported, "h264_vaapi encoder not available in libavcodec");
    }

    auto muxRes = open_muxer(outputPath);
    if (!muxRes) {
        return muxRes;
    }

    AVBufferRef* rawHwDev = nullptr;
    int err = ::av_hwdevice_ctx_create(
        &rawHwDev, AV_HWDEVICE_TYPE_VAAPI, drmNode.c_str(), nullptr, 0);
    if (err < 0 || rawHwDev == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "av_hwdevice_ctx_create(VAAPI) failed: " + av_error_string(err));
    }
    hwDeviceCtx_.reset(rawHwDev);

    codecCtx_.reset(::avcodec_alloc_context3(codec));
    if (!codecCtx_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "avcodec_alloc_context3(h264_vaapi) failed");
    }

    codecCtx_->width = static_cast<int>(encodeWidth_);
    codecCtx_->height = static_cast<int>(encodeHeight_);
    codecCtx_->time_base = AVRational{1, static_cast<int>(fps_)};
    codecCtx_->framerate = AVRational{static_cast<int>(fps_), 1};
    codecCtx_->sample_aspect_ratio = AVRational{1, 1};
    codecCtx_->pix_fmt = AV_PIX_FMT_VAAPI;
    codecCtx_->bit_rate = static_cast<int64_t>(bitrateKbps_) * 1000;
    codecCtx_->gop_size = static_cast<int>(fps_ * 2U);
    codecCtx_->max_b_frames = 0;

    if ((fmtCtx_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AvBufferRefPtr hwFramesRef(::av_hwframe_ctx_alloc(hwDeviceCtx_.get()));
    if (!hwFramesRef) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "av_hwframe_ctx_alloc failed");
    }

    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(hwFramesRef->data);
    framesCtx->format = AV_PIX_FMT_VAAPI;
    framesCtx->sw_format = AV_PIX_FMT_NV12;
    framesCtx->width = static_cast<int>(encodeWidth_);
    framesCtx->height = static_cast<int>(encodeHeight_);
    framesCtx->initial_pool_size = 6;

    err = ::av_hwframe_ctx_init(hwFramesRef.get());
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "av_hwframe_ctx_init failed: " + av_error_string(err));
    }

    codecCtx_->hw_frames_ctx = ::av_buffer_ref(hwFramesRef.get());

    err = ::avcodec_open2(codecCtx_.get(), codec, nullptr);
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "avcodec_open2(h264_vaapi) failed: " + av_error_string(err));
    }

    err = ::avcodec_parameters_from_context(videoStream_->codecpar, codecCtx_.get());
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "avcodec_parameters_from_context failed: " + av_error_string(err));
    }

    if ((fmtCtx_->oformat->flags & AVFMT_NOFILE) == 0) {
        err = ::avio_open(&fmtCtx_->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::IoError,
                "avio_open failed: " + av_error_string(err));
        }
    }

    AVDictionary* muxOpts = nullptr;
    ::av_dict_set(&muxOpts, "movflags", "+faststart", 0);
    err = ::avformat_write_header(fmtCtx_.get(), &muxOpts);
    ::av_dict_free(&muxOpts);
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "avformat_write_header failed: " + av_error_string(err));
    }
    headerWritten_ = true;

    swFrame_.reset(::av_frame_alloc());
    hwFrame_.reset(::av_frame_alloc());
    pkt_.reset(::av_packet_alloc());
    if (!swFrame_ || !hwFrame_ || !pkt_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "Failed to allocate FFmpeg frame/packet");
    }

    swFrame_->format = AV_PIX_FMT_NV12;
    swFrame_->width = static_cast<int>(encodeWidth_);
    swFrame_->height = static_cast<int>(encodeHeight_);
    err = ::av_frame_get_buffer(swFrame_.get(), 32);
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "av_frame_get_buffer(NV12) failed: " + av_error_string(err));
    }

    swsCtx_.reset(::sws_getContext(
        static_cast<int>(captureWidth_),
        static_cast<int>(captureHeight_),
        AV_PIX_FMT_BGRA,
        static_cast<int>(encodeWidth_),
        static_cast<int>(encodeHeight_),
        AV_PIX_FMT_NV12,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr));
    if (!swsCtx_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "sws_getContext(BGRA->NV12) failed");
    }

    gpuAccelerated_ = true;
    encoderName_ = "VA-API H.264 (" + vendor + ")";
    return core::Result<void>::make_ok();
}

core::Result<void> VaapiH264Encoder::init_software(const std::filesystem::path& outputPath) {
    const AVCodec* codec = ::avcodec_find_encoder_by_name("libx264");
    if (codec == nullptr) {
        codec = ::avcodec_find_encoder_by_name("libopenh264");
    }
    if (codec == nullptr) {
        codec = ::avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (codec == nullptr) {
        codec = ::avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    }
    if (codec == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "No suitable video encoder found in libavcodec");
    }

    auto muxRes = open_muxer(outputPath);
    if (!muxRes) {
        return muxRes;
    }

    codecCtx_.reset(::avcodec_alloc_context3(codec));
    if (!codecCtx_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "avcodec_alloc_context3 failed");
    }

    codecCtx_->width = static_cast<int>(encodeWidth_);
    codecCtx_->height = static_cast<int>(encodeHeight_);
    codecCtx_->time_base = AVRational{1, static_cast<int>(fps_)};
    codecCtx_->framerate = AVRational{static_cast<int>(fps_), 1};
    codecCtx_->sample_aspect_ratio = AVRational{1, 1};
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx_->bit_rate = static_cast<int64_t>(bitrateKbps_) * 1000;
    codecCtx_->gop_size = static_cast<int>(fps_ * 2U);
    codecCtx_->max_b_frames = 0;

    if ((fmtCtx_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (std::string_view(codec->name) == "libx264") {
        ::av_opt_set(codecCtx_->priv_data, "preset", "veryfast", 0);
        ::av_opt_set(codecCtx_->priv_data, "tune", "zerolatency", 0);
    }

    int err = ::avcodec_open2(codecCtx_.get(), codec, nullptr);
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "avcodec_open2 failed: " + av_error_string(err));
    }

    err = ::avcodec_parameters_from_context(videoStream_->codecpar, codecCtx_.get());
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "avcodec_parameters_from_context failed: " + av_error_string(err));
    }

    if ((fmtCtx_->oformat->flags & AVFMT_NOFILE) == 0) {
        err = ::avio_open(&fmtCtx_->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::IoError,
                "avio_open failed: " + av_error_string(err));
        }
    }

    AVDictionary* muxOpts = nullptr;
    ::av_dict_set(&muxOpts, "movflags", "+faststart", 0);
    err = ::avformat_write_header(fmtCtx_.get(), &muxOpts);
    ::av_dict_free(&muxOpts);
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "avformat_write_header failed: " + av_error_string(err));
    }
    headerWritten_ = true;

    swFrame_.reset(::av_frame_alloc());
    pkt_.reset(::av_packet_alloc());
    if (!swFrame_ || !pkt_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "Failed to allocate FFmpeg frame/packet");
    }

    swFrame_->format = AV_PIX_FMT_YUV420P;
    swFrame_->width = static_cast<int>(encodeWidth_);
    swFrame_->height = static_cast<int>(encodeHeight_);
    err = ::av_frame_get_buffer(swFrame_.get(), 32);
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "av_frame_get_buffer(YUV420P) failed: " + av_error_string(err));
    }

    swsCtx_.reset(::sws_getContext(
        static_cast<int>(captureWidth_),
        static_cast<int>(captureHeight_),
        AV_PIX_FMT_BGRA,
        static_cast<int>(encodeWidth_),
        static_cast<int>(encodeHeight_),
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr));
    if (!swsCtx_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed, "sws_getContext(BGRA->YUV420P) failed");
    }

    gpuAccelerated_ = false;
    encoderName_ = std::string("FFmpeg ") + codec->name;
    return core::Result<void>::make_ok();
}

core::Result<void> VaapiH264Encoder::drain_packets() {
    while (true) {
        const int recvErr = ::avcodec_receive_packet(codecCtx_.get(), pkt_.get());
        if (recvErr == AVERROR(EAGAIN) || recvErr == AVERROR_EOF) {
            return core::Result<void>::make_ok();
        }
        if (recvErr < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "avcodec_receive_packet failed: " + av_error_string(recvErr));
        }

        ::av_packet_rescale_ts(pkt_.get(), codecCtx_->time_base, videoStream_->time_base);
        pkt_->stream_index = videoStream_->index;

        const int writeErr = ::av_interleaved_write_frame(fmtCtx_.get(), pkt_.get());
        ::av_packet_unref(pkt_.get());
        if (writeErr < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "av_interleaved_write_frame failed: " + av_error_string(writeErr));
        }
    }
}

core::Result<void> VaapiH264Encoder::encode_frame(
    const std::uint8_t* bgraData,
    std::uint32_t bgraStride,
    std::int64_t ptsIndex) {
    if (!codecCtx_ || !swFrame_ || !swsCtx_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed, "Encoder not initialized");
    }

    int err = ::av_frame_make_writable(swFrame_.get());
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "av_frame_make_writable failed: " + av_error_string(err));
    }

    const uint8_t* const srcSlice[1] = {bgraData};
    const int srcStride[1] = {static_cast<int>(bgraStride)};
    ::sws_scale(
        swsCtx_.get(),
        srcSlice,
        srcStride,
        0,
        static_cast<int>(captureHeight_),
        swFrame_->data,
        swFrame_->linesize);

    AVFrame* submitFrame = swFrame_.get();
    if (gpuAccelerated_) {
        ::av_frame_unref(hwFrame_.get());
        err = ::av_hwframe_get_buffer(codecCtx_->hw_frames_ctx, hwFrame_.get(), 0);
        if (err < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "av_hwframe_get_buffer failed: " + av_error_string(err));
        }
        err = ::av_hwframe_transfer_data(hwFrame_.get(), swFrame_.get(), 0);
        if (err < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "av_hwframe_transfer_data failed: " + av_error_string(err));
        }
        submitFrame = hwFrame_.get();
    }

    submitFrame->pts = ptsIndex;

    err = ::avcodec_send_frame(codecCtx_.get(), submitFrame);
    if (err < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "avcodec_send_frame failed: " + av_error_string(err));
    }

    auto drainRes = drain_packets();
    if (!drainRes) {
        return drainRes;
    }

    ++framesWritten_;
    return core::Result<void>::make_ok();
}

core::Result<void> VaapiH264Encoder::finalize() {
    if (finalized_) {
        return core::Result<void>::make_ok();
    }
    finalized_ = true;

    if (codecCtx_ && framesWritten_ > 0) {
        (void)::avcodec_send_frame(codecCtx_.get(), nullptr);
        (void)drain_packets();
    }

    if (fmtCtx_ && headerWritten_) {
        const int err = ::av_write_trailer(fmtCtx_.get());
        if (err < 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "av_write_trailer failed: " + av_error_string(err));
        }
    }

    return core::Result<void>::make_ok();
}

} // namespace shine::platform::linux_impl
