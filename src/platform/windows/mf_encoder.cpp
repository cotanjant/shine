#include "platform/windows/mf_encoder.hpp"

#include "platform/windows/win_raii.hpp"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <wrl/implements.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

namespace shine::platform::win {

namespace {

constexpr std::size_t kNv12PoolSize = 6;
constexpr auto kPoolWaitTimeout = std::chrono::milliseconds(250);

struct HardwareMftInfo {
    bool found{false};
    std::string friendlyName;
};

[[nodiscard]] HardwareMftInfo query_hardware_h264_mft() {
    HardwareMftInfo info{};
    MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    const HRESULT hr = ::MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &outType,
        &activates,
        &count);

    if (SUCCEEDED(hr) && activates != nullptr && count > 0) {
        info.found = true;
        WCHAR* friendly = nullptr;
        UINT32 friendlyLen = 0;
        if (SUCCEEDED(activates[0]->GetAllocatedString(
                MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen)) &&
            friendly != nullptr) {
            info.friendlyName = wide_to_utf8(friendly);
            ::CoTaskMemFree(friendly);
        }
    }

    if (activates != nullptr) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i] != nullptr) {
                activates[i]->Release();
            }
        }
        ::CoTaskMemFree(activates);
    }
    return info;
}

class SlotToken
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IUnknown> {
public:
    explicit SlotToken(std::size_t index) noexcept : index_(index) {}
    [[nodiscard]] std::size_t index() const noexcept { return index_; }

private:
    std::size_t index_{0};
};

} // namespace

struct PoolState : public std::enable_shared_from_this<PoolState> {
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
        Microsoft::WRL::ComPtr<IUnknown> token;
    };

    std::vector<Slot> slots;
    std::deque<std::size_t> freeIndices;
    std::mutex mutex;
    std::condition_variable cv;
    Microsoft::WRL::ComPtr<IMFAsyncCallback> returnCallback;
    bool shuttingDown{false};
    std::uint64_t poolHits{0};
    std::uint64_t overflowAllocations{0};

    void return_slot(std::size_t idx) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (shuttingDown || idx >= slots.size()) {
                return;
            }
            freeIndices.push_back(idx);
        }
        cv.notify_one();
    }
};

namespace {

class PoolReturnCallback
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFAsyncCallback> {
public:
    explicit PoolReturnCallback(std::weak_ptr<PoolState> pool) noexcept
        : weakPool_(std::move(pool)) {}

    STDMETHODIMP GetParameters(DWORD* pdwFlags, DWORD* pdwQueue) override {
        if (pdwFlags == nullptr || pdwQueue == nullptr) {
            return E_POINTER;
        }
        *pdwFlags = 0;
        *pdwQueue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
        return S_OK;
    }

    STDMETHODIMP Invoke(IMFAsyncResult* pAsyncResult) override {
        if (pAsyncResult == nullptr) {
            return S_OK;
        }
        auto pool = weakPool_.lock();
        if (!pool) {
            return S_OK;
        }
        Microsoft::WRL::ComPtr<IUnknown> stateUnk;
        if (SUCCEEDED(pAsyncResult->GetState(&stateUnk)) && stateUnk) {
            const auto* token = static_cast<const SlotToken*>(stateUnk.Get());
            pool->return_slot(token->index());
        }
        return S_OK;
    }

private:
    std::weak_ptr<PoolState> weakPool_;
};

[[nodiscard]] core::Result<std::shared_ptr<PoolState>> create_nv12_pool(
    ID3D11Device* d3dDevice,
    ID3D11VideoDevice* videoDevice,
    ID3D11VideoProcessorEnumerator* enumerator,
    std::uint32_t width,
    std::uint32_t height) {
    auto pool = std::make_shared<PoolState>();
    pool->returnCallback = Microsoft::WRL::Make<PoolReturnCallback>(pool->weak_from_this());
    pool->slots.resize(kNv12PoolSize);

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_NV12;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
    ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovDesc.Texture2D.MipSlice = 0;

    for (std::size_t i = 0; i < kNv12PoolSize; ++i) {
        HRESULT hr = d3dDevice->CreateTexture2D(&texDesc, nullptr, &pool->slots[i].texture);
        if (FAILED(hr)) {
            return core::Result<std::shared_ptr<PoolState>>::make_err(
                core::ErrorCode::EncoderInitFailed,
                "CreateTexture2D(NV12 pool slot) failed: " + hr_to_string(hr));
        }

        hr = videoDevice->CreateVideoProcessorOutputView(
            pool->slots[i].texture.Get(),
            enumerator,
            &ovDesc,
            &pool->slots[i].outputView);
        if (FAILED(hr)) {
            return core::Result<std::shared_ptr<PoolState>>::make_err(
                core::ErrorCode::EncoderInitFailed,
                "CreateVideoProcessorOutputView failed: " + hr_to_string(hr));
        }

        pool->slots[i].token = Microsoft::WRL::Make<SlotToken>(i);
        pool->freeIndices.push_back(i);
    }

    return core::Result<std::shared_ptr<PoolState>>::make_ok(std::move(pool));
}

[[nodiscard]] core::Result<Microsoft::WRL::ComPtr<IMFSample>> create_dxgi_sample_from_texture(
    ID3D11Texture2D* texture,
    IMFAsyncCallback* returnCallback,
    IUnknown* slotToken) {
    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = ::MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D), texture, 0, FALSE, &mediaBuffer);
    if (FAILED(hr)) {
        return core::Result<Microsoft::WRL::ComPtr<IMFSample>>::make_err(
            core::ErrorCode::EncodeFailed,
            "MFCreateDXGISurfaceBuffer failed: " + hr_to_string(hr));
    }

    Microsoft::WRL::ComPtr<IMF2DBuffer> buffer2d;
    hr = mediaBuffer.As(&buffer2d);
    if (FAILED(hr)) {
        return core::Result<Microsoft::WRL::ComPtr<IMFSample>>::make_err(
            core::ErrorCode::EncodeFailed,
            "QueryInterface(IMF2DBuffer) failed: " + hr_to_string(hr));
    }

    DWORD contiguousLen = 0;
    hr = buffer2d->GetContiguousLength(&contiguousLen);
    if (FAILED(hr)) {
        return core::Result<Microsoft::WRL::ComPtr<IMFSample>>::make_err(
            core::ErrorCode::EncodeFailed,
            "IMF2DBuffer::GetContiguousLength failed: " + hr_to_string(hr));
    }

    hr = mediaBuffer->SetCurrentLength(contiguousLen);
    if (FAILED(hr)) {
        return core::Result<Microsoft::WRL::ComPtr<IMFSample>>::make_err(
            core::ErrorCode::EncodeFailed,
            "IMFMediaBuffer::SetCurrentLength failed: " + hr_to_string(hr));
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (returnCallback != nullptr && slotToken != nullptr) {
        Microsoft::WRL::ComPtr<IMFTrackedSample> tracked;
        if (SUCCEEDED(::MFCreateTrackedSample(&tracked)) && tracked) {
            if (SUCCEEDED(tracked->SetAllocator(returnCallback, slotToken))) {
                tracked.As(&sample);
            }
        }
    }

    if (!sample) {
        hr = ::MFCreateSample(&sample);
        if (FAILED(hr)) {
            return core::Result<Microsoft::WRL::ComPtr<IMFSample>>::make_err(
                core::ErrorCode::EncodeFailed,
                "MFCreateSample failed: " + hr_to_string(hr));
        }
    }

    hr = sample->AddBuffer(mediaBuffer.Get());
    if (FAILED(hr)) {
        return core::Result<Microsoft::WRL::ComPtr<IMFSample>>::make_err(
            core::ErrorCode::EncodeFailed,
            "IMFSample::AddBuffer failed: " + hr_to_string(hr));
    }

    return core::Result<Microsoft::WRL::ComPtr<IMFSample>>::make_ok(std::move(sample));
}

void bgra_to_nv12_bt709(
    const std::uint8_t* bgra,
    std::uint32_t bgraPitch,
    std::uint32_t width,
    std::uint32_t height,
    std::uint8_t* nv12Out) noexcept {
    std::uint8_t* yPlane = nv12Out;
    std::uint8_t* uvPlane = nv12Out + static_cast<std::size_t>(width) * height;

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * bgraPitch;
        std::uint8_t* yRow = yPlane + static_cast<std::size_t>(y) * width;

        for (std::uint32_t x = 0; x < width; ++x) {
            const int b = row[x * 4U + 0U];
            const int g = row[x * 4U + 1U];
            const int r = row[x * 4U + 2U];

            // BT.709 limited-range Y = ((47*R + 157*G + 16*B + 128) >> 8) + 16
            const int luma = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
            yRow[x] = static_cast<std::uint8_t>(std::clamp(luma, 16, 235));
        }

        if ((y & 1U) == 0U && y + 1U < height) {
            const std::uint8_t* rowNext = bgra + static_cast<std::size_t>(y + 1U) * bgraPitch;
            std::uint8_t* uvRow = uvPlane + static_cast<std::size_t>(y / 2U) * width;

            for (std::uint32_t x = 0; x < width; x += 2U) {
                const int b = (row[x * 4U + 0U] + row[(x + 1U) * 4U + 0U] +
                               rowNext[x * 4U + 0U] + rowNext[(x + 1U) * 4U + 0U] + 2) >> 2;
                const int g = (row[x * 4U + 1U] + row[(x + 1U) * 4U + 1U] +
                               rowNext[x * 4U + 1U] + rowNext[(x + 1U) * 4U + 1U] + 2) >> 2;
                const int r = (row[x * 4U + 2U] + row[(x + 1U) * 4U + 2U] +
                               rowNext[x * 4U + 2U] + rowNext[(x + 1U) * 4U + 2U] + 2) >> 2;

                // BT.709 limited-range Cb/Cr
                const int cb = ((-26 * r - 86 * g + 112 * b + 128) >> 8) + 128;
                const int cr = ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
                uvRow[x] = static_cast<std::uint8_t>(std::clamp(cb, 16, 240));
                uvRow[x + 1U] = static_cast<std::uint8_t>(std::clamp(cr, 16, 240));
            }
        }
    }
}

} // namespace

MfH264Encoder::~MfH264Encoder() {
    (void)finalize();

    if (surfacePool_) {
        std::lock_guard<std::mutex> lock(surfacePool_->mutex);
        surfacePool_->shuttingDown = true;
    }

    sinkWriter_.Reset();
    deviceManager_.Reset();
    swStagingTex_.Reset();

    if (surfacePool_) {
        surfacePool_->returnCallback.Reset();
        surfacePool_->slots.clear();
        surfacePool_.reset();
    }
}

core::Result<void> MfH264Encoder::init(
    const std::filesystem::path& outputPath,
    ID3D11Device* d3dDevice,
    D3D11BgraToNv12Converter* converter,
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

    if (preferGpu && d3dDevice != nullptr && converter != nullptr) {
        auto gpuRes = init_gpu_pipeline(outputPath, d3dDevice, *converter);
        if (gpuRes) {
            return gpuRes;
        }
        // Clean partial state before software fallback
        sinkWriter_.Reset();
        deviceManager_.Reset();
        surfacePool_.reset();
    }

    return init_software_pipeline(outputPath, d3dDevice);
}

core::Result<void> MfH264Encoder::configure_sink_writer(
    const std::filesystem::path& outputPath,
    bool enableHardware) {
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    HRESULT hr = ::MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "MFCreateAttributes failed: " + hr_to_string(hr));
    }

    if (enableHardware && deviceManager_) {
        attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, deviceManager_.Get());
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    } else {
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    }
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE);

    const std::wstring wideOut = outputPath.wstring();
    const UINT32 profiles[] = {
        eAVEncH264VProfile_High,
        eAVEncH264VProfile_Main,
        eAVEncH264VProfile_Base
    };

    HRESULT lastNegotiationHr = E_FAIL;
    for (const UINT32 profile : profiles) {
        Microsoft::WRL::ComPtr<IMFSinkWriter> candidateWriter;
        hr = ::MFCreateSinkWriterFromURL(wideOut.c_str(), nullptr, attrs.Get(), &candidateWriter);
        if (FAILED(hr)) {
            lastNegotiationHr = hr;
            break;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> outType;
        hr = ::MFCreateMediaType(&outType);
        if (FAILED(hr)) {
            lastNegotiationHr = hr;
            continue;
        }
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps_ * 1000U);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ::MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, encodeWidth_, encodeHeight_);
        ::MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fps_, 1);
        ::MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, profile);

        DWORD streamIdx = 0;
        hr = candidateWriter->AddStream(outType.Get(), &streamIdx);
        if (FAILED(hr)) {
            lastNegotiationHr = hr;
            continue;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> inType;
        hr = ::MFCreateMediaType(&inType);
        if (FAILED(hr)) {
            lastNegotiationHr = hr;
            continue;
        }
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ::MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, encodeWidth_, encodeHeight_);
        ::MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps_, 1);
        ::MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        inType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        inType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

        hr = candidateWriter->SetInputMediaType(streamIdx, inType.Get(), nullptr);
        if (FAILED(hr)) {
            lastNegotiationHr = hr;
            continue;
        }

        hr = candidateWriter->BeginWriting();
        if (FAILED(hr)) {
            lastNegotiationHr = hr;
            continue;
        }

        sinkWriter_ = std::move(candidateWriter);
        videoStreamIndex_ = streamIdx;
        return core::Result<void>::make_ok();
    }

    return core::Result<void>::make_err(
        core::ErrorCode::EncoderInitFailed,
        "Media Foundation H.264 stream negotiation failed: " + hr_to_string(lastNegotiationHr));
}

core::Result<void> MfH264Encoder::init_gpu_pipeline(
    const std::filesystem::path& outputPath,
    ID3D11Device* d3dDevice,
    D3D11BgraToNv12Converter& converter) {
    UINT resetToken = 0;
    HRESULT hr = ::MFCreateDXGIDeviceManager(&resetToken, &deviceManager_);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "MFCreateDXGIDeviceManager failed: " + hr_to_string(hr));
    }

    hr = deviceManager_->ResetDevice(d3dDevice, resetToken);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "IMFDXGIDeviceManager::ResetDevice failed: " + hr_to_string(hr));
    }

    auto poolRes = create_nv12_pool(
        d3dDevice,
        converter.video_device(),
        converter.enumerator(),
        encodeWidth_,
        encodeHeight_);
    if (!poolRes) {
        return core::Result<void>::make_err(poolRes.error());
    }
    surfacePool_ = poolRes.take();

    auto cfgRes = configure_sink_writer(outputPath, true);
    if (!cfgRes) {
        return cfgRes;
    }

    usingGpuPipeline_ = true;

    const HardwareMftInfo hwInfo = query_hardware_h264_mft();
    bool confirmedHw = false;
    std::string detectedName;

    Microsoft::WRL::ComPtr<IMFTransform> transform;
    if (SUCCEEDED(sinkWriter_->GetServiceForStream(
            videoStreamIndex_, GUID_NULL, IID_PPV_ARGS(&transform))) &&
        transform) {
        Microsoft::WRL::ComPtr<IMFAttributes> mftAttrs;
        if (SUCCEEDED(transform->GetAttributes(&mftAttrs)) && mftAttrs) {
            WCHAR* hwUrl = nullptr;
            UINT32 hwLen = 0;
            if (SUCCEEDED(mftAttrs->GetAllocatedString(
                    MFT_ENUM_HARDWARE_URL_Attribute, &hwUrl, &hwLen)) &&
                hwUrl != nullptr) {
                confirmedHw = true;
                ::CoTaskMemFree(hwUrl);
            }

            WCHAR* friendly = nullptr;
            UINT32 friendlyLen = 0;
            if (SUCCEEDED(mftAttrs->GetAllocatedString(
                    MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen)) &&
                friendly != nullptr) {
                detectedName = wide_to_utf8(friendly);
                ::CoTaskMemFree(friendly);
            }
        }
    }

    if (!confirmedHw && hwInfo.found) {
        // When SinkWriter wraps an async hardware MFT directly on the DXGI device manager,
        // GetServiceForStream may omit MFT_ENUM_HARDWARE_URL_Attribute while still running on the HW MFT.
        confirmedHw = true;
    }

    gpuAccelerated_ = confirmedHw;
    if (!detectedName.empty()) {
        encoderName_ = std::move(detectedName);
    } else if (!hwInfo.friendlyName.empty()) {
        encoderName_ = hwInfo.friendlyName;
    } else if (gpuAccelerated_) {
        encoderName_ = "Media Foundation Hardware H.264";
    } else {
        encoderName_ = "Media Foundation D3D11 + H.264";
    }

    return core::Result<void>::make_ok();
}

core::Result<void> MfH264Encoder::init_software_pipeline(
    const std::filesystem::path& outputPath,
    ID3D11Device* d3dDevice) {
    if (d3dDevice == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Null ID3D11Device in software fallback");
    }

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = captureWidth_;
    stagingDesc.Height = captureHeight_;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    const HRESULT hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &swStagingTex_);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "CreateTexture2D(swStagingTex_) failed: " + hr_to_string(hr));
    }

    swNv12Buffer_.resize(static_cast<std::size_t>(encodeWidth_) * encodeHeight_ * 3U / 2U);

    auto cfgRes = configure_sink_writer(outputPath, false);
    if (!cfgRes) {
        return cfgRes;
    }

    usingGpuPipeline_ = false;
    gpuAccelerated_ = false;
    encoderName_ = "Media Foundation Software H.264";
    return core::Result<void>::make_ok();
}

core::Result<void> MfH264Encoder::encode_gpu_frame(
    D3D11BgraToNv12Converter& converter,
    std::int64_t sampleTime100Ns,
    std::int64_t sampleDuration100Ns) {
    if (!usingGpuPipeline_ || !sinkWriter_ || !surfacePool_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed, "GPU encoder pipeline is not initialized");
    }

    std::optional<std::size_t> slotIdx;
    {
        std::unique_lock<std::mutex> lock(surfacePool_->mutex);
        if (surfacePool_->freeIndices.empty()) {
            surfacePool_->cv.wait_for(lock, kPoolWaitTimeout, [&]() {
                return !surfacePool_->freeIndices.empty() || surfacePool_->shuttingDown;
            });
        }
        if (surfacePool_->shuttingDown) {
            return core::Result<void>::make_ok();
        }
        if (!surfacePool_->freeIndices.empty()) {
            slotIdx = surfacePool_->freeIndices.front();
            surfacePool_->freeIndices.pop_front();
            ++surfacePool_->poolHits;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTex;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> targetOv;
    IMFAsyncCallback* callback = nullptr;
    IUnknown* token = nullptr;

    // Fallback temporary texture if the async MFT holds all pooled slots simultaneously
    Microsoft::WRL::ComPtr<ID3D11Texture2D> overflowTex;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> overflowOv;

    if (slotIdx.has_value()) {
        const auto& slot = surfacePool_->slots[*slotIdx];
        targetTex = slot.texture;
        targetOv = slot.outputView;
        callback = surfacePool_->returnCallback.Get();
        token = slot.token.Get();
    } else {
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        surfacePool_->slots[0].texture->GetDevice(&dev);

        D3D11_TEXTURE2D_DESC texDesc{};
        surfacePool_->slots[0].texture->GetDesc(&texDesc);
        HRESULT hr = dev->CreateTexture2D(&texDesc, nullptr, &overflowTex);
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "Overflow NV12 texture allocation failed: " + hr_to_string(hr));
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
        ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = converter.video_device()->CreateVideoProcessorOutputView(
            overflowTex.Get(), converter.enumerator(), &ovDesc, &overflowOv);
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "Overflow NV12 output view failed: " + hr_to_string(hr));
        }
        targetTex = overflowTex;
        targetOv = overflowOv;
        ++surfacePool_->overflowAllocations;
    }

    auto convRes = converter.convert(targetOv.Get());
    if (!convRes) {
        if (slotIdx.has_value()) {
            surfacePool_->return_slot(*slotIdx);
        }
        return convRes;
    }

    auto sampleRes = create_dxgi_sample_from_texture(targetTex.Get(), callback, token);
    if (!sampleRes) {
        if (slotIdx.has_value()) {
            surfacePool_->return_slot(*slotIdx);
        }
        return core::Result<void>::make_err(sampleRes.error());
    }

    Microsoft::WRL::ComPtr<IMFSample> sample = sampleRes.take();
    sample->SetSampleTime(sampleTime100Ns);
    sample->SetSampleDuration(sampleDuration100Ns);

    const HRESULT hr = sinkWriter_->WriteSample(videoStreamIndex_, sample.Get());
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "IMFSinkWriter::WriteSample (GPU) failed: " + hr_to_string(hr));
    }

    ++framesWritten_;
    return core::Result<void>::make_ok();
}

core::Result<void> MfH264Encoder::encode_cpu_frame(
    const DxgiCapture& capture,
    std::int64_t sampleTime100Ns,
    std::int64_t sampleDuration100Ns) {
    if (usingGpuPipeline_ || !sinkWriter_ || !swStagingTex_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed, "Software encoder pipeline is not initialized");
    }

    auto copyRes = capture.copy_to_staging(swStagingTex_.Get());
    if (!copyRes) {
        return copyRes;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = capture.context()->Map(swStagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "Map(swStagingTex_) failed: " + hr_to_string(hr));
    }

    bgra_to_nv12_bt709(
        static_cast<const std::uint8_t*>(mapped.pData),
        mapped.RowPitch,
        encodeWidth_,
        encodeHeight_,
        swNv12Buffer_.data());

    capture.context()->Unmap(swStagingTex_.Get(), 0);

    const DWORD bufSize = static_cast<DWORD>(swNv12Buffer_.size());
    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    hr = ::MFCreateMemoryBuffer(bufSize, &mediaBuffer);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "MFCreateMemoryBuffer failed: " + hr_to_string(hr));
    }

    BYTE* dstPtr = nullptr;
    hr = mediaBuffer->Lock(&dstPtr, nullptr, nullptr);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "IMFMediaBuffer::Lock failed: " + hr_to_string(hr));
    }
    std::memcpy(dstPtr, swNv12Buffer_.data(), bufSize);
    mediaBuffer->Unlock();
    mediaBuffer->SetCurrentLength(bufSize);

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = ::MFCreateSample(&sample);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "MFCreateSample failed: " + hr_to_string(hr));
    }
    sample->AddBuffer(mediaBuffer.Get());
    sample->SetSampleTime(sampleTime100Ns);
    sample->SetSampleDuration(sampleDuration100Ns);

    hr = sinkWriter_->WriteSample(videoStreamIndex_, sample.Get());
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "IMFSinkWriter::WriteSample (CPU) failed: " + hr_to_string(hr));
    }

    ++framesWritten_;
    return core::Result<void>::make_ok();
}

core::Result<void> MfH264Encoder::finalize() {
    if (finalized_) {
        return core::Result<void>::make_ok();
    }
    finalized_ = true;

    if (surfacePool_) {
        std::lock_guard<std::mutex> lock(surfacePool_->mutex);
        surfacePool_->shuttingDown = true;
        surfacePool_->cv.notify_all();
    }

    if (sinkWriter_ && framesWritten_ > 0) {
        const HRESULT hr = sinkWriter_->Finalize();
        sinkWriter_.Reset();
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "IMFSinkWriter::Finalize failed: " + hr_to_string(hr));
        }
    } else {
        sinkWriter_.Reset();
    }

    return core::Result<void>::make_ok();
}

} // namespace shine::platform::win
