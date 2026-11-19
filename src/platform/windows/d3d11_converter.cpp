#include "platform/windows/d3d11_converter.hpp"

#include "platform/windows/win_raii.hpp"

namespace shine::platform::win {

D3D11BgraToNv12Converter::~D3D11BgraToNv12Converter() {
    inputView_.Reset();
    processor_.Reset();
    enumerator_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    d3dContext_.Reset();
}

core::Result<void> D3D11BgraToNv12Converter::init(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* srcBgraTex,
    std::uint32_t srcWidth,
    std::uint32_t srcHeight,
    std::uint32_t dstWidth,
    std::uint32_t dstHeight) {
    if (device == nullptr || context == nullptr || srcBgraTex == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Null D3D11 pointer passed to converter");
    }

    d3dContext_ = context;

    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&videoDevice_));
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "ID3D11VideoDevice unsupported: " + hr_to_string(hr));
    }

    hr = context->QueryInterface(IID_PPV_ARGS(&videoContext_));
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "ID3D11VideoContext unsupported: " + hr_to_string(hr));
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = srcWidth;
    contentDesc.InputHeight = srcHeight;
    contentDesc.OutputWidth = dstWidth;
    contentDesc.OutputHeight = dstHeight;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, &enumerator_);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "CreateVideoProcessorEnumerator failed: " + hr_to_string(hr));
    }

    UINT inSupport = 0;
    hr = enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &inSupport);
    if (FAILED(hr) || (inSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0U) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "Video processor does not support B8G8R8A8_UNORM input");
    }

    UINT outSupport = 0;
    hr = enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outSupport);
    if (FAILED(hr) || (outSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0U) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "Video processor does not support NV12 output");
    }

    hr = videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "CreateVideoProcessor failed: " + hr_to_string(hr));
    }

    videoContext_->VideoProcessorSetStreamFrameFormat(
        processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE srcColor{};
    srcColor.Usage = 0;
    srcColor.RGB_Range = 0; // Full 0-255
    srcColor.YCbCr_Matrix = 1; // BT.709
    srcColor.YCbCr_xvYCC = 0;
    srcColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &srcColor);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE dstColor{};
    dstColor.Usage = 0;
    dstColor.RGB_Range = 0;
    dstColor.YCbCr_Matrix = 1; // BT.709
    dstColor.YCbCr_xvYCC = 0;
    dstColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetOutputColorSpace(processor_.Get(), &dstColor);

    const RECT srcRect{0, 0, static_cast<LONG>(dstWidth), static_cast<LONG>(dstHeight)};
    const RECT dstRect{0, 0, static_cast<LONG>(dstWidth), static_cast<LONG>(dstHeight)};
    videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &srcRect);
    videoContext_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dstRect);
    videoContext_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &dstRect);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inViewDesc{};
    inViewDesc.FourCC = 0;
    inViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inViewDesc.Texture2D.MipSlice = 0;
    inViewDesc.Texture2D.ArraySlice = 0;

    hr = videoDevice_->CreateVideoProcessorInputView(
        srcBgraTex, enumerator_.Get(), &inViewDesc, &inputView_);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported,
            "CreateVideoProcessorInputView failed: " + hr_to_string(hr));
    }

    return core::Result<void>::make_ok();
}

core::Result<void> D3D11BgraToNv12Converter::convert(ID3D11VideoProcessorOutputView* dstView) {
    if (dstView == nullptr || !videoContext_ || !processor_ || !inputView_) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed, "Converter not initialized");
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.ppPastSurfaces = nullptr;
    stream.pInputSurface = inputView_.Get();
    stream.ppFutureSurfaces = nullptr;

    const HRESULT hr = videoContext_->VideoProcessorBlt(
        processor_.Get(), dstView, 0, 1, &stream);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::EncodeFailed,
            "VideoProcessorBlt failed: " + hr_to_string(hr));
    }

    d3dContext_->Flush();
    return core::Result<void>::make_ok();
}

} // namespace shine::platform::win
