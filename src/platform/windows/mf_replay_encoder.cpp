#include "platform/windows/mf_replay_encoder.hpp"

#include "platform/windows/mf_encoder.hpp"
#include "platform/windows/win_raii.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <wrl/implements.h>

#include <initguid.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>

namespace shine::platform::win {

namespace {

[[nodiscard]] std::vector<std::uint8_t> extract_sps_pps_annex_b(
    const std::vector<std::uint8_t>& data,
    bool& outHasIdr) {
    outHasIdr = false;
    std::vector<std::size_t> starts;
    std::vector<std::size_t> scLens;

    for (std::size_t i = 0; i + 3U < data.size(); ++i) {
        if (data[i] == 0x00U && data[i + 1U] == 0x00U) {
            if (i + 4U < data.size() && data[i + 2U] == 0x00U && data[i + 3U] == 0x01U) {
                starts.push_back(i);
                scLens.push_back(4U);
                i += 3U;
            } else if (data[i + 2U] == 0x01U) {
                starts.push_back(i);
                scLens.push_back(3U);
                i += 2U;
            }
        }
    }

    std::vector<std::uint8_t> spsPps;
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::size_t nalStart = starts[k] + scLens[k];
        if (nalStart >= data.size()) {
            continue;
        }
        const std::size_t nalEnd = (k + 1U < starts.size()) ? starts[k + 1U] : data.size();
        const std::uint8_t nalType = data[nalStart] & 0x1FU;
        if (nalType == 5U) {
            outHasIdr = true;
        }
        if (nalType == 7U || nalType == 8U) {
            spsPps.insert(
                spsPps.end(),
                data.begin() + static_cast<std::ptrdiff_t>(starts[k]),
                data.begin() + static_cast<std::ptrdiff_t>(nalEnd));
        }
    }
    return spsPps;
}

void read_seq_header_from_type(IMFMediaType* mediaType, std::vector<std::uint8_t>& outCfg) {
    if (mediaType == nullptr) {
        return;
    }
    UINT32 blobSize = 0;
    if (SUCCEEDED(mediaType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blobSize)) && blobSize > 0) {
        std::vector<std::uint8_t> buf(blobSize);
        if (SUCCEEDED(mediaType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, buf.data(), blobSize, &blobSize))) {
            buf.resize(blobSize);
            outCfg = std::move(buf);
        }
    }
}

void configure_codec_api(IMFTransform* mft, std::uint32_t fps, std::uint32_t bitrateKbps) {
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&codecApi))) || !codecApi) {
        return;
    }

    VARIANT vRateCtrl{};
    vRateCtrl.vt = VT_UI4;
    vRateCtrl.ulVal = eAVEncCommonRateControlMode_CBR;
    (void)codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &vRateCtrl);

    VARIANT vBitrate{};
    vBitrate.vt = VT_UI4;
    vBitrate.ulVal = bitrateKbps * 1000U;
    (void)codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &vBitrate);

    VARIANT vGop{};
    vGop.vt = VT_UI4;
    vGop.ulVal = std::max<std::uint32_t>(1U, fps);
    (void)codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &vGop);

    VARIANT vLowLat{};
    vLowLat.vt = VT_BOOL;
    vLowLat.boolVal = VARIANT_TRUE;
    (void)codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &vLowLat);
}

} // namespace

MfH264StreamEncoder::~MfH264StreamEncoder() {
    shutdown();
}

core::Result<void> MfH264StreamEncoder::init(
    ID3D11Device* d3dDevice,
    D3D11BgraToNv12Converter* converter,
    std::uint32_t captureWidth,
    std::uint32_t captureHeight,
    std::uint32_t fps,
    std::uint32_t bitrateKbps,
    bool preferGpu,
    std::function<void(core::EncodedVideoPacket)> onPacket,
    std::function<void(std::vector<std::uint8_t>)> onCodecConfig) {
    shutdown();

    captureWidth_ = captureWidth;
    captureHeight_ = captureHeight;
    encodeWidth_ = captureWidth & ~1U;
    encodeHeight_ = captureHeight & ~1U;
    fps_ = fps;
    bitrateKbps_ = bitrateKbps;
    onPacket_ = std::move(onPacket);
    onCodecConfig_ = std::move(onCodecConfig);
    codecConfig_.clear();
    framesEncoded_.store(0, std::memory_order_release);

    if (encodeWidth_ < 2U || encodeHeight_ < 2U) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Capture dimensions are too small");
    }

    if (preferGpu && d3dDevice != nullptr && converter != nullptr) {
        auto hwRes = configure_mft(d3dDevice, converter, true);
        if (hwRes) {
            return hwRes;
        }
        shutdown();
    }

    return configure_mft(d3dDevice, nullptr, false);
}

core::Result<void> MfH264StreamEncoder::configure_mft(
    ID3D11Device* d3dDevice,
    D3D11BgraToNv12Converter* converter,
    bool tryHardware) {
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    const UINT32 enumFlags = tryHardware
        ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER)
        : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER);

    HRESULT hr = ::MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER, enumFlags, nullptr, &outInfo, &activates, &count);

    std::string chosenName;
    if (SUCCEEDED(hr) && activates != nullptr && count > 0) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i] == nullptr) {
                continue;
            }
            Microsoft::WRL::ComPtr<IMFTransform> candidate;
            if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&candidate))) && candidate) {
                WCHAR* friendly = nullptr;
                UINT32 friendlyLen = 0;
                if (SUCCEEDED(activates[i]->GetAllocatedString(
                        MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen)) &&
                    friendly != nullptr) {
                    chosenName = wide_to_utf8(friendly);
                    ::CoTaskMemFree(friendly);
                }
                mft_ = std::move(candidate);
                break;
            }
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

    if (!mft_ && !tryHardware) {
        hr = ::CoCreateInstance(
            CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
        if (SUCCEEDED(hr)) {
            chosenName = "Microsoft H264 Software MFT";
        }
    }

    if (!mft_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            tryHardware ? "No hardware H.264 encoder MFT available" : "No software H.264 MFT available");
    }

    isAsyncMft_ = false;
    bool hasHwAttr = false;
    Microsoft::WRL::ComPtr<IMFAttributes> mftAttrs;
    if (SUCCEEDED(mft_->GetAttributes(&mftAttrs)) && mftAttrs) {
        UINT32 asyncVal = 0;
        if (SUCCEEDED(mftAttrs->GetUINT32(MF_TRANSFORM_ASYNC, &asyncVal)) && asyncVal != 0U) {
            isAsyncMft_ = true;
            hr = mftAttrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            if (FAILED(hr)) {
                return core::Result<void>::make_err(
                    core::ErrorCode::EncoderInitFailed,
                    "Failed to unlock async MFT: " + hr_to_string(hr));
            }
        }

        WCHAR* hwUrl = nullptr;
        UINT32 hwLen = 0;
        if (SUCCEEDED(mftAttrs->GetAllocatedString(
                MFT_ENUM_HARDWARE_URL_Attribute, &hwUrl, &hwLen)) &&
            hwUrl != nullptr) {
            hasHwAttr = true;
            ::CoTaskMemFree(hwUrl);
        }
    }

    if (tryHardware && d3dDevice != nullptr && converter != nullptr) {
        UINT resetToken = 0;
        hr = ::MFCreateDXGIDeviceManager(&resetToken, &deviceManager_);
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

        hr = mft_->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(deviceManager_.Get()));
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed,
                "MFT_MESSAGE_SET_D3D_MANAGER failed: " + hr_to_string(hr));
        }

        auto poolRes = create_nv12_pool(
            d3dDevice,
            converter->video_device(),
            converter->enumerator(),
            encodeWidth_,
            encodeHeight_);
        if (!poolRes) {
            return core::Result<void>::make_err(poolRes.error());
        }
        surfacePool_ = poolRes.take();
        usingGpuPipeline_ = true;
        gpuAccelerated_ = hasHwAttr || tryHardware;
    } else {
        if (d3dDevice == nullptr) {
            return core::Result<void>::make_err(
                core::ErrorCode::InvalidArgument, "Null D3D11 device");
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
        hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &swStagingTex_);
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed,
                "CreateTexture2D(swStagingTex_) failed: " + hr_to_string(hr));
        }
        swNv12Buffer_.resize(static_cast<std::size_t>(encodeWidth_) * encodeHeight_ * 3U / 2U);
        usingGpuPipeline_ = false;
        gpuAccelerated_ = false;
    }

    configure_codec_api(mft_.Get(), fps_, bitrateKbps_);

    const UINT32 profiles[] = {
        eAVEncH264VProfile_Main,
        eAVEncH264VProfile_High,
        eAVEncH264VProfile_Base
    };

    HRESULT lastNegHr = E_FAIL;
    bool negotiated = false;
    for (const UINT32 profile : profiles) {
        Microsoft::WRL::ComPtr<IMFMediaType> outType;
        hr = ::MFCreateMediaType(&outType);
        if (FAILED(hr)) {
            lastNegHr = hr;
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

        hr = mft_->SetOutputType(0, outType.Get(), 0);
        if (FAILED(hr)) {
            lastNegHr = hr;
            continue;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> inType;
        hr = ::MFCreateMediaType(&inType);
        if (FAILED(hr)) {
            lastNegHr = hr;
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

        hr = mft_->SetInputType(0, inType.Get(), 0);
        if (FAILED(hr)) {
            lastNegHr = hr;
            continue;
        }

        negotiated = true;
        break;
    }

    if (!negotiated) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "H.264 MFT type negotiation failed: " + hr_to_string(lastNegHr));
    }

    Microsoft::WRL::ComPtr<IMFMediaType> curOutType;
    if (SUCCEEDED(mft_->GetOutputCurrentType(0, &curOutType)) && curOutType) {
        read_seq_header_from_type(curOutType.Get(), codecConfig_);
        if (!codecConfig_.empty() && onCodecConfig_) {
            onCodecConfig_(codecConfig_);
        }
    }

    MFT_OUTPUT_STREAM_INFO osi{};
    hr = mft_->GetOutputStreamInfo(0, &osi);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "IMFTransform::GetOutputStreamInfo failed: " + hr_to_string(hr));
    }
    mftProvidesOutputSamples_ =
        (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0U;
    outputBufferSize_ = osi.cbSize > 0U ? osi.cbSize : (encodeWidth_ * encodeHeight_);

    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "MFT_MESSAGE_NOTIFY_BEGIN_STREAMING failed: " + hr_to_string(hr));
    }

    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "MFT_MESSAGE_NOTIFY_START_OF_STREAM failed: " + hr_to_string(hr));
    }

    if (isAsyncMft_) {
        hr = mft_.As(&eventGen_);
        if (FAILED(hr) || !eventGen_) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed,
                "Async MFT missing IMFMediaEventGenerator: " + hr_to_string(hr));
        }
        needInputCount_.store(0, std::memory_order_release);
        stopEventPump_.store(false, std::memory_order_release);
        eventThread_ = std::thread(&MfH264StreamEncoder::async_event_loop, this);
    }

    encoderName_ = !chosenName.empty()
                       ? std::move(chosenName)
                       : (gpuAccelerated_ ? "Media Foundation Hardware H.264"
                                          : "Media Foundation Software H.264");
    return core::Result<void>::make_ok();
}

void MfH264StreamEncoder::maybe_capture_codec_config(const std::vector<std::uint8_t>& annexB) {
    if (!codecConfig_.empty()) {
        return;
    }
    bool unusedIdr = false;
    std::vector<std::uint8_t> extracted = extract_sps_pps_annex_b(annexB, unusedIdr);
    if (!extracted.empty()) {
        codecConfig_ = std::move(extracted);
        if (onCodecConfig_) {
            onCodecConfig_(codecConfig_);
        }
    }
}

void MfH264StreamEncoder::drain_one_output() {
    MFT_OUTPUT_DATA_BUFFER outBuf{};
    Microsoft::WRL::ComPtr<IMFSample> preAllocSample;

    if (!mftProvidesOutputSamples_) {
        if (FAILED(::MFCreateSample(&preAllocSample)) || !preAllocSample) {
            return;
        }
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        if (FAILED(::MFCreateMemoryBuffer(outputBufferSize_, &buf)) || !buf) {
            return;
        }
        preAllocSample->AddBuffer(buf.Get());
        outBuf.pSample = preAllocSample.Get();
    }

    DWORD status = 0;
    const HRESULT hr = mft_->ProcessOutput(0, 1, &outBuf, &status);

    Microsoft::WRL::ComPtr<IMFCollection> eventsGuard;
    if (outBuf.pEvents != nullptr) {
        eventsGuard.Attach(outBuf.pEvents);
    }

    Microsoft::WRL::ComPtr<IMFSample> outSample;
    if (mftProvidesOutputSamples_ && outBuf.pSample != nullptr) {
        outSample.Attach(outBuf.pSample);
    } else if (!mftProvidesOutputSamples_ && outBuf.pSample == preAllocSample.Get()) {
        outSample = preAllocSample;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        Microsoft::WRL::ComPtr<IMFMediaType> availOut;
        if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &availOut)) && availOut) {
            (void)mft_->SetOutputType(0, availOut.Get(), 0);
            read_seq_header_from_type(availOut.Get(), codecConfig_);
            if (!codecConfig_.empty() && onCodecConfig_) {
                onCodecConfig_(codecConfig_);
            }
        }
        return;
    }

    if (FAILED(hr) || !outSample) {
        return;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> contigBuf;
    if (FAILED(outSample->ConvertToContiguousBuffer(&contigBuf)) || !contigBuf) {
        return;
    }

    BYTE* dataPtr = nullptr;
    DWORD dataLen = 0;
    if (FAILED(contigBuf->Lock(&dataPtr, nullptr, &dataLen)) || dataPtr == nullptr) {
        return;
    }

    std::vector<std::uint8_t> packetBytes(dataPtr, dataPtr + dataLen);
    contigBuf->Unlock();

    if (packetBytes.empty()) {
        return;
    }

    bool hasIdr = false;
    (void)extract_sps_pps_annex_b(packetBytes, hasIdr);
    maybe_capture_codec_config(packetBytes);

    UINT32 cleanPoint = 0;
    (void)outSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);

    LONGLONG pts100Ns = 0;
    LONGLONG dur100Ns = 0;
    (void)outSample->GetSampleTime(&pts100Ns);
    (void)outSample->GetSampleDuration(&dur100Ns);
    if (dur100Ns <= 0) {
        dur100Ns = std::max<LONGLONG>(1, 10'000'000LL / fps_);
    }

    core::EncodedVideoPacket pkt{};
    pkt.data = std::move(packetBytes);
    pkt.ptsUs = std::max<std::int64_t>(0, pts100Ns / 10LL);
    pkt.durationUs = std::max<std::int64_t>(1, dur100Ns / 10LL);
    pkt.isKeyframe = (cleanPoint != 0U) || hasIdr;

    if (onPacket_) {
        onPacket_(std::move(pkt));
    }
    framesEncoded_.fetch_add(1, std::memory_order_relaxed);
}

void MfH264StreamEncoder::drain_sync_outputs() {
    while (true) {
        MFT_OUTPUT_DATA_BUFFER outBuf{};
        Microsoft::WRL::ComPtr<IMFSample> preAllocSample;

        if (!mftProvidesOutputSamples_) {
            if (FAILED(::MFCreateSample(&preAllocSample)) || !preAllocSample) {
                break;
            }
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            if (FAILED(::MFCreateMemoryBuffer(outputBufferSize_, &buf)) || !buf) {
                break;
            }
            preAllocSample->AddBuffer(buf.Get());
            outBuf.pSample = preAllocSample.Get();
        }

        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &outBuf, &status);

        Microsoft::WRL::ComPtr<IMFCollection> eventsGuard;
        if (outBuf.pEvents != nullptr) {
            eventsGuard.Attach(outBuf.pEvents);
        }

        Microsoft::WRL::ComPtr<IMFSample> outSample;
        if (mftProvidesOutputSamples_ && outBuf.pSample != nullptr) {
            outSample.Attach(outBuf.pSample);
        } else if (!mftProvidesOutputSamples_ && outBuf.pSample == preAllocSample.Get()) {
            outSample = preAllocSample;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            Microsoft::WRL::ComPtr<IMFMediaType> availOut;
            if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &availOut)) && availOut) {
                (void)mft_->SetOutputType(0, availOut.Get(), 0);
                read_seq_header_from_type(availOut.Get(), codecConfig_);
                if (!codecConfig_.empty() && onCodecConfig_) {
                    onCodecConfig_(codecConfig_);
                }
            }
            continue;
        }
        if (FAILED(hr) || !outSample) {
            break;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> contigBuf;
        if (FAILED(outSample->ConvertToContiguousBuffer(&contigBuf)) || !contigBuf) {
            continue;
        }
        BYTE* dataPtr = nullptr;
        DWORD dataLen = 0;
        if (FAILED(contigBuf->Lock(&dataPtr, nullptr, &dataLen)) || dataPtr == nullptr) {
            continue;
        }
        std::vector<std::uint8_t> packetBytes(dataPtr, dataPtr + dataLen);
        contigBuf->Unlock();

        if (packetBytes.empty()) {
            continue;
        }

        bool hasIdr = false;
        (void)extract_sps_pps_annex_b(packetBytes, hasIdr);
        maybe_capture_codec_config(packetBytes);

        UINT32 cleanPoint = 0;
        (void)outSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);

        LONGLONG pts100Ns = 0;
        LONGLONG dur100Ns = 0;
        (void)outSample->GetSampleTime(&pts100Ns);
        (void)outSample->GetSampleDuration(&dur100Ns);
        if (dur100Ns <= 0) {
            dur100Ns = std::max<LONGLONG>(1, 10'000'000LL / fps_);
        }

        core::EncodedVideoPacket pkt{};
        pkt.data = std::move(packetBytes);
        pkt.ptsUs = std::max<std::int64_t>(0, pts100Ns / 10LL);
        pkt.durationUs = std::max<std::int64_t>(1, dur100Ns / 10LL);
        pkt.isKeyframe = (cleanPoint != 0U) || hasIdr;

        if (onPacket_) {
            onPacket_(std::move(pkt));
        }
        framesEncoded_.fetch_add(1, std::memory_order_relaxed);
    }
}

void MfH264StreamEncoder::async_event_loop() {
    while (!stopEventPump_.load(std::memory_order_acquire)) {
        Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
        const HRESULT hr = eventGen_->GetEvent(0, &ev);
        if (FAILED(hr) || !ev) {
            break;
        }

        MediaEventType evType = MEUnknown;
        if (FAILED(ev->GetType(&evType))) {
            continue;
        }

        if (evType == METransformNeedInput) {
            {
                std::lock_guard<std::mutex> lock(inputCvMutex_);
                needInputCount_.fetch_add(1, std::memory_order_acq_rel);
            }
            inputCv_.notify_one();
        } else if (evType == METransformHaveOutput) {
            std::lock_guard<std::mutex> mftLock(mftMutex_);
            if (!stopEventPump_.load(std::memory_order_acquire) && mft_) {
                drain_one_output();
            }
        } else if (evType == METransformDrainComplete) {
            break;
        }
    }
}

core::Result<void> MfH264StreamEncoder::submit_sample(IMFSample* sample) {
    if (!mft_ || sample == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed, "Stream encoder not initialized");
    }

    if (isAsyncMft_) {
        {
            std::unique_lock<std::mutex> lock(inputCvMutex_);
            inputCv_.wait_for(lock, std::chrono::milliseconds(200), [&]() {
                return needInputCount_.load(std::memory_order_acquire) > 0 ||
                       stopEventPump_.load(std::memory_order_acquire);
            });
        }
        if (stopEventPump_.load(std::memory_order_acquire)) {
            return core::Result<void>::make_ok();
        }
        int expected = needInputCount_.load(std::memory_order_acquire);
        while (expected > 0 &&
               !needInputCount_.compare_exchange_weak(expected, expected - 1, std::memory_order_acq_rel)) {
        }
        if (expected <= 0) {
            return core::Result<void>::make_ok();
        }

        std::lock_guard<std::mutex> mftLock(mftMutex_);
        if (stopEventPump_.load(std::memory_order_acquire) || !mft_) {
            return core::Result<void>::make_ok();
        }
        const HRESULT hr = mft_->ProcessInput(0, sample, 0);
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncodeFailed,
                "Async MFT ProcessInput failed: " + hr_to_string(hr));
        }
        return core::Result<void>::make_ok();
    }

    std::lock_guard<std::mutex> mftLock(mftMutex_);
    const HRESULT hr = mft_->ProcessInput(0, sample, 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "Sync MFT ProcessInput failed: " + hr_to_string(hr));
    }
    drain_sync_outputs();
    return core::Result<void>::make_ok();
}

core::Result<void> MfH264StreamEncoder::submit_gpu_frame(
    D3D11BgraToNv12Converter& converter,
    std::int64_t sampleTime100Ns,
    std::int64_t sampleDuration100Ns) {
    if (!usingGpuPipeline_ || !surfacePool_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed, "GPU stream encoder is not active");
    }

    auto sampleRes = acquire_converted_nv12_sample(
        *surfacePool_, converter, sampleTime100Ns, sampleDuration100Ns);
    if (!sampleRes) {
        return core::Result<void>::make_err(sampleRes.error());
    }
    Microsoft::WRL::ComPtr<IMFSample> sample = sampleRes.take();
    if (!sample) {
        return core::Result<void>::make_ok();
    }
    return submit_sample(sample.Get());
}

core::Result<void> MfH264StreamEncoder::submit_cpu_frame(
    const DxgiCapture& capture,
    std::int64_t sampleTime100Ns,
    std::int64_t sampleDuration100Ns) {
    if (usingGpuPipeline_ || !swStagingTex_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed, "CPU stream encoder is not active");
    }

    auto sampleRes = create_cpu_nv12_sample(
        capture,
        swStagingTex_.Get(),
        encodeWidth_,
        encodeHeight_,
        swNv12Buffer_,
        sampleTime100Ns,
        sampleDuration100Ns);
    if (!sampleRes) {
        return core::Result<void>::make_err(sampleRes.error());
    }
    Microsoft::WRL::ComPtr<IMFSample> sample = sampleRes.take();
    return submit_sample(sample.Get());
}

void MfH264StreamEncoder::shutdown() {
    stopEventPump_.store(true, std::memory_order_release);
    inputCv_.notify_all();

    if (mft_) {
        {
            std::lock_guard<std::mutex> mftLock(mftMutex_);
            (void)mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            (void)mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        Microsoft::WRL::ComPtr<IMFShutdown> mfShut;
        if (SUCCEEDED(mft_.As(&mfShut)) && mfShut) {
            (void)mfShut->Shutdown();
        }
    }

    if (eventThread_.joinable()) {
        eventThread_.join();
    }

    eventGen_.Reset();
    mft_.Reset();
    deviceManager_.Reset();
    swStagingTex_.Reset();
    shutdown_nv12_pool(surfacePool_);
}

core::Result<void> save_clip_mp4_windows(
    const core::ClipSnapshot& snapshot,
    const std::filesystem::path& outputPath) {
    if (snapshot.videoPackets.empty() || snapshot.width < 2U || snapshot.height < 2U) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Replay buffer contains no video frames yet");
    }

    std::error_code ec;
    const auto parent = outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    const std::wstring wideOut = outputPath.wstring();
    const bool tryAudio = !snapshot.audioChunks.empty();

    const auto create_writer = [&](bool withAudio,
                                   Microsoft::WRL::ComPtr<IMFSinkWriter>& outWriter,
                                   DWORD& outVideoIdx,
                                   DWORD& outAudioIdx,
                                   bool& outAudioActive) -> core::Result<void> {
        outAudioActive = false;

        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        HRESULT hr = ::MFCreateAttributes(&attrs, 2);
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed, "MFCreateAttributes failed");
        }
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);

        Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
        hr = ::MFCreateSinkWriterFromURL(wideOut.c_str(), nullptr, attrs.Get(), &writer);
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::IoError,
                "MFCreateSinkWriterFromURL failed: " + hr_to_string(hr));
        }

        Microsoft::WRL::ComPtr<IMFMediaType> videoType;
        hr = ::MFCreateMediaType(&videoType);
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed, "MFCreateMediaType(H264) failed");
        }
        videoType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        videoType->SetUINT32(MF_MT_AVG_BITRATE, snapshot.bitrateKbps * 1000U);
        videoType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ::MFSetAttributeSize(videoType.Get(), MF_MT_FRAME_SIZE, snapshot.width, snapshot.height);
        ::MFSetAttributeRatio(videoType.Get(), MF_MT_FRAME_RATE, snapshot.fps, 1);
        ::MFSetAttributeRatio(videoType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (!snapshot.codecConfig.empty()) {
            videoType->SetBlob(
                MF_MT_MPEG_SEQUENCE_HEADER,
                snapshot.codecConfig.data(),
                static_cast<UINT32>(snapshot.codecConfig.size()));
        }

        DWORD vIdx = 0;
        hr = writer->AddStream(videoType.Get(), &vIdx);
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed,
                "SinkWriter AddStream(H264) failed: " + hr_to_string(hr));
        }
        hr = writer->SetInputMediaType(vIdx, videoType.Get(), nullptr);
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::EncoderInitFailed,
                "SinkWriter SetInputMediaType(H264 passthrough) failed: " + hr_to_string(hr));
        }

        DWORD aIdx = 0;
        if (withAudio) {
            const std::uint32_t sampleRate =
                snapshot.audioChunks[0].sampleRate > 0 ? snapshot.audioChunks[0].sampleRate : 48000U;
            const std::uint16_t channels =
                snapshot.audioChunks[0].channels > 0 ? snapshot.audioChunks[0].channels : 1U;

            Microsoft::WRL::ComPtr<IMFMediaType> aacOut;
            Microsoft::WRL::ComPtr<IMFMediaType> pcmIn;
            if (SUCCEEDED(::MFCreateMediaType(&aacOut)) &&
                SUCCEEDED(::MFCreateMediaType(&pcmIn))) {
                aacOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                aacOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                aacOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                aacOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
                aacOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                aacOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
                aacOut->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
                aacOut->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

                pcmIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pcmIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                pcmIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                pcmIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
                pcmIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                const UINT32 blockAlign = static_cast<UINT32>(channels * 2U);
                pcmIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
                pcmIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * blockAlign);

                if (SUCCEEDED(writer->AddStream(aacOut.Get(), &aIdx)) &&
                    SUCCEEDED(writer->SetInputMediaType(aIdx, pcmIn.Get(), nullptr))) {
                    outAudioActive = true;
                } else {
                    return core::Result<void>::make_err(
                        core::ErrorCode::NotSupported, "AAC audio negotiation failed");
                }
            }
        }

        hr = writer->BeginWriting();
        if (FAILED(hr)) {
            return core::Result<void>::make_err(
                core::ErrorCode::IoError,
                "SinkWriter BeginWriting failed: " + hr_to_string(hr));
        }

        outWriter = std::move(writer);
        outVideoIdx = vIdx;
        outAudioIdx = aIdx;
        return core::Result<void>::make_ok();
    };

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    DWORD videoIdx = 0;
    DWORD audioIdx = 0;
    bool audioActive = false;

    if (tryAudio) {
        auto audioWriterRes = create_writer(true, writer, videoIdx, audioIdx, audioActive);
        if (!audioWriterRes) {
            writer.Reset();
            std::filesystem::remove(outputPath, ec);
            auto vidOnlyRes = create_writer(false, writer, videoIdx, audioIdx, audioActive);
            if (!vidOnlyRes) {
                return vidOnlyRes;
            }
        }
    } else {
        auto vidOnlyRes = create_writer(false, writer, videoIdx, audioIdx, audioActive);
        if (!vidOnlyRes) {
            return vidOnlyRes;
        }
    }

    std::size_t vPos = 0;
    std::size_t aPos = 0;
    std::int64_t lastVideoPts100Ns = -1;
    std::int64_t lastAudioPts100Ns = -1;

    while (vPos < snapshot.videoPackets.size() ||
           (audioActive && aPos < snapshot.audioChunks.size())) {
        const bool chooseAudio =
            audioActive &&
            aPos < snapshot.audioChunks.size() &&
            (vPos >= snapshot.videoPackets.size() ||
             snapshot.audioChunks[aPos].ptsUs <= snapshot.videoPackets[vPos].ptsUs);

        if (!chooseAudio) {
            const auto& pkt = snapshot.videoPackets[vPos++];
            const DWORD byteLen = static_cast<DWORD>(pkt.data.size());
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            HRESULT hr = ::MFCreateMemoryBuffer(byteLen, &buf);
            if (FAILED(hr)) {
                return core::Result<void>::make_err(
                    core::ErrorCode::EncodeFailed, "MFCreateMemoryBuffer(video clip) failed");
            }
            BYTE* ptr = nullptr;
            if (FAILED(buf->Lock(&ptr, nullptr, nullptr))) {
                return core::Result<void>::make_err(
                    core::ErrorCode::EncodeFailed, "Buffer Lock failed");
            }
            std::memcpy(ptr, pkt.data.data(), byteLen);
            buf->Unlock();
            buf->SetCurrentLength(byteLen);

            Microsoft::WRL::ComPtr<IMFSample> sample;
            if (FAILED(::MFCreateSample(&sample)) || !sample) {
                return core::Result<void>::make_err(
                    core::ErrorCode::EncodeFailed, "MFCreateSample(video clip) failed");
            }
            sample->AddBuffer(buf.Get());

            std::int64_t ts100Ns = std::max<std::int64_t>(0, pkt.ptsUs * 10LL);
            if (ts100Ns <= lastVideoPts100Ns) {
                ts100Ns = lastVideoPts100Ns + 1;
            }
            lastVideoPts100Ns = ts100Ns;
            sample->SetSampleTime(ts100Ns);
            sample->SetSampleDuration(std::max<LONGLONG>(1, pkt.durationUs * 10LL));
            if (pkt.isKeyframe) {
                sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
            }

            hr = writer->WriteSample(videoIdx, sample.Get());
            if (FAILED(hr)) {
                return core::Result<void>::make_err(
                    core::ErrorCode::IoError,
                    "WriteSample(clip video) failed: " + hr_to_string(hr));
            }
        } else {
            const auto& chunk = snapshot.audioChunks[aPos++];
            if (chunk.samples.empty()) {
                continue;
            }
            const DWORD byteLen = static_cast<DWORD>(chunk.samples.size() * sizeof(std::int16_t));
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            HRESULT hr = ::MFCreateMemoryBuffer(byteLen, &buf);
            if (FAILED(hr)) {
                return core::Result<void>::make_err(
                    core::ErrorCode::EncodeFailed, "MFCreateMemoryBuffer(audio clip) failed");
            }
            BYTE* ptr = nullptr;
            if (FAILED(buf->Lock(&ptr, nullptr, nullptr))) {
                return core::Result<void>::make_err(
                    core::ErrorCode::EncodeFailed, "Audio Buffer Lock failed");
            }
            std::memcpy(ptr, chunk.samples.data(), byteLen);
            buf->Unlock();
            buf->SetCurrentLength(byteLen);

            Microsoft::WRL::ComPtr<IMFSample> sample;
            if (FAILED(::MFCreateSample(&sample)) || !sample) {
                return core::Result<void>::make_err(
                    core::ErrorCode::EncodeFailed, "MFCreateSample(audio clip) failed");
            }
            sample->AddBuffer(buf.Get());

            const std::uint32_t rate = chunk.sampleRate > 0 ? chunk.sampleRate : 48000U;
            const std::uint16_t ch = chunk.channels > 0 ? chunk.channels : 1U;
            const std::int64_t frames = static_cast<std::int64_t>(chunk.samples.size()) / ch;
            const std::int64_t dur100Ns = std::max<std::int64_t>(1, (frames * 10'000'000LL) / rate);

            std::int64_t ts100Ns = std::max<std::int64_t>(0, chunk.ptsUs * 10LL);
            if (ts100Ns <= lastAudioPts100Ns) {
                ts100Ns = lastAudioPts100Ns + 1;
            }
            lastAudioPts100Ns = ts100Ns;
            sample->SetSampleTime(ts100Ns);
            sample->SetSampleDuration(dur100Ns);

            hr = writer->WriteSample(audioIdx, sample.Get());
            if (FAILED(hr)) {
                return core::Result<void>::make_err(
                    core::ErrorCode::IoError,
                    "WriteSample(clip audio) failed: " + hr_to_string(hr));
            }
        }
    }

    const HRESULT finHr = writer->Finalize();
    writer.Reset();
    if (FAILED(finHr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "SinkWriter Finalize(clip) failed: " + hr_to_string(finHr));
    }

    return core::Result<void>::make_ok();
}

} // namespace shine::platform::win
