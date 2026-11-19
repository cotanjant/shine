#include "platform/windows/dxgi_capture.hpp"

#include "platform/windows/win_raii.hpp"
#include "shine/core/recorder.hpp"

#include <d3d11_4.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace shine::platform::win {

namespace {

constexpr std::uint32_t kMaxCursorPatchSize = 256U;

struct EnumeratedOutput {
    std::uint32_t index{};
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    core::DisplayInfo info;
    RECT desktopBounds{};
};

[[nodiscard]] core::Result<std::vector<EnumeratedOutput>> enumerate_outputs_internal() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    const HRESULT factoryHr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factoryHr)) {
        return core::Result<std::vector<EnumeratedOutput>>::make_err(
            core::ErrorCode::CaptureFailed,
            "CreateDXGIFactory1 failed: " + hr_to_string(factoryHr));
    }

    std::vector<EnumeratedOutput> results;
    std::uint32_t publicIdx = 0;

    for (UINT adapterIdx = 0;; ++adapterIdx) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adHr = factory->EnumAdapters1(adapterIdx, &adapter);
        if (adHr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adHr)) {
            continue;
        }

        for (UINT outputIdx = 0;; ++outputIdx) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            const HRESULT outHr = adapter->EnumOutputs(outputIdx, &output);
            if (outHr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(outHr)) {
                continue;
            }

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) {
                continue;
            }

            MONITORINFOEXW monInfo{};
            monInfo.cbSize = sizeof(monInfo);
            const bool hasMonInfo = ::GetMonitorInfoW(desc.Monitor, &monInfo) != FALSE;

            DEVMODEW devMode{};
            devMode.dmSize = sizeof(devMode);
            const bool hasMode = ::EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &devMode) != FALSE;

            std::uint32_t w = static_cast<std::uint32_t>(
                std::max<LONG>(0, desc.DesktopCoordinates.right - desc.DesktopCoordinates.left));
            std::uint32_t h = static_cast<std::uint32_t>(
                std::max<LONG>(0, desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top));
            std::uint32_t hz = 60U;

            if (hasMode) {
                if (devMode.dmPelsWidth > 0 && devMode.dmPelsHeight > 0) {
                    w = devMode.dmPelsWidth;
                    h = devMode.dmPelsHeight;
                }
                if (devMode.dmDisplayFrequency > 0) {
                    hz = devMode.dmDisplayFrequency;
                }
            }

            std::string name = wide_to_utf8(desc.DeviceName);
            if (name.empty()) {
                name = "Display " + std::to_string(publicIdx);
            }

            EnumeratedOutput item{};
            item.index = publicIdx;
            item.adapter = adapter;
            item.output = output;
            item.desktopBounds = desc.DesktopCoordinates;
            item.info = core::DisplayInfo{
                publicIdx,
                w,
                h,
                hz,
                hasMonInfo && ((monInfo.dwFlags & MONITORINFOF_PRIMARY) != 0),
                std::move(name)
            };
            results.push_back(std::move(item));
            ++publicIdx;
        }
    }

    return core::Result<std::vector<EnumeratedOutput>>::make_ok(std::move(results));
}

[[nodiscard]] std::uint8_t clamp_u8(int v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

void blend_cursor_patch(
    std::uint8_t* bgraDst,
    std::uint32_t dstPitch,
    std::uint32_t patchW,
    std::uint32_t patchH,
    std::uint32_t cursorStartX,
    std::uint32_t cursorStartY,
    const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shapeInfo,
    const std::vector<std::uint8_t>& shapeBytes) noexcept {
    if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        for (std::uint32_t y = 0; y < patchH; ++y) {
            const std::uint32_t sy = cursorStartY + y;
            if (sy >= shapeInfo.Height) {
                break;
            }
            const std::uint8_t* srcRow = shapeBytes.data() + static_cast<std::size_t>(sy) * shapeInfo.Pitch;
            std::uint8_t* dstRow = bgraDst + static_cast<std::size_t>(y) * dstPitch;

            for (std::uint32_t x = 0; x < patchW; ++x) {
                const std::uint32_t sx = cursorStartX + x;
                if (sx >= shapeInfo.Width) {
                    break;
                }
                const std::uint8_t* s = srcRow + static_cast<std::size_t>(sx) * 4U;
                std::uint8_t* d = dstRow + static_cast<std::size_t>(x) * 4U;

                const std::uint32_t a = s[3];
                if (a == 255U) {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                } else if (a > 0U) {
                    const std::uint32_t invA = 255U - a;
                    d[0] = clamp_u8(static_cast<int>(s[0] + (d[0] * invA) / 255U));
                    d[1] = clamp_u8(static_cast<int>(s[1] + (d[1] * invA) / 255U));
                    d[2] = clamp_u8(static_cast<int>(s[2] + (d[2] * invA) / 255U));
                }
            }
        }
    } else if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        for (std::uint32_t y = 0; y < patchH; ++y) {
            const std::uint32_t sy = cursorStartY + y;
            if (sy >= shapeInfo.Height) {
                break;
            }
            const std::uint8_t* srcRow = shapeBytes.data() + static_cast<std::size_t>(sy) * shapeInfo.Pitch;
            std::uint8_t* dstRow = bgraDst + static_cast<std::size_t>(y) * dstPitch;

            for (std::uint32_t x = 0; x < patchW; ++x) {
                const std::uint32_t sx = cursorStartX + x;
                if (sx >= shapeInfo.Width) {
                    break;
                }
                const std::uint8_t* s = srcRow + static_cast<std::size_t>(sx) * 4U;
                std::uint8_t* d = dstRow + static_cast<std::size_t>(x) * 4U;

                if (s[3] != 0U) {
                    d[0] ^= s[0];
                    d[1] ^= s[1];
                    d[2] ^= s[2];
                } else {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                }
            }
        }
    } else if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        const std::uint32_t maskH = shapeInfo.Height / 2U;
        for (std::uint32_t y = 0; y < patchH; ++y) {
            const std::uint32_t sy = cursorStartY + y;
            if (sy >= maskH) {
                break;
            }
            const std::uint8_t* andRow = shapeBytes.data() + static_cast<std::size_t>(sy) * shapeInfo.Pitch;
            const std::uint8_t* xorRow = shapeBytes.data() + static_cast<std::size_t>(sy + maskH) * shapeInfo.Pitch;
            std::uint8_t* dstRow = bgraDst + static_cast<std::size_t>(y) * dstPitch;

            for (std::uint32_t x = 0; x < patchW; ++x) {
                const std::uint32_t sx = cursorStartX + x;
                if (sx >= shapeInfo.Width) {
                    break;
                }
                const std::uint8_t maskBit = static_cast<std::uint8_t>(0x80U >> (sx & 7U));
                const bool andBit = (andRow[sx / 8U] & maskBit) != 0U;
                const bool xorBit = (xorRow[sx / 8U] & maskBit) != 0U;

                std::uint8_t* d = dstRow + static_cast<std::size_t>(x) * 4U;
                if (!andBit && !xorBit) {
                    d[0] = 0U;
                    d[1] = 0U;
                    d[2] = 0U;
                } else if (!andBit && xorBit) {
                    d[0] = 255U;
                    d[1] = 255U;
                    d[2] = 255U;
                } else if (andBit && xorBit) {
                    d[0] ^= 0xFFU;
                    d[1] ^= 0xFFU;
                    d[2] ^= 0xFFU;
                }
            }
        }
    }
}

} // namespace

DxgiCapture::~DxgiCapture() {
    duplication_.Reset();
    cursorStagingTex_.Reset();
    compositeTex_.Reset();
    desktopCleanTex_.Reset();
    context_.Reset();
    device_.Reset();
    output1_.Reset();
    adapter_.Reset();
}

core::Result<void> DxgiCapture::init(std::uint32_t displayIndex, bool captureCursor) {
    captureCursor_ = captureCursor;
    hasInitialFrame_ = false;
    pointerVisible_ = false;
    hasPointerShape_ = false;

    auto outputsRes = enumerate_outputs_internal();
    if (!outputsRes) {
        return core::Result<void>::make_err(outputsRes.error());
    }

    const auto& outputs = outputsRes.value();
    if (outputs.empty()) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed, "No desktop-attached DXGI outputs found");
    }
    if (displayIndex >= outputs.size()) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument,
            "Display index " + std::to_string(displayIndex) + " out of range");
    }

    const auto& chosen = outputs[displayIndex];
    adapter_ = chosen.adapter;
    desktopBounds_ = chosen.desktopBounds;

    const HRESULT qiHr = chosen.output.As(&output1_);
    if (FAILED(qiHr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed,
            "QueryInterface(IDXGIOutput1) failed: " + hr_to_string(qiHr));
    }

    constexpr D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    D3D_FEATURE_LEVEL chosenLevel{};
    HRESULT devHr = ::D3D11CreateDevice(
        adapter_.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        kLevels,
        static_cast<UINT>(std::size(kLevels)),
        D3D11_SDK_VERSION,
        &device_,
        &chosenLevel,
        &context_);

    if (FAILED(devHr)) {
        devHr = ::D3D11CreateDevice(
            adapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            kLevels,
            static_cast<UINT>(std::size(kLevels)),
            D3D11_SDK_VERSION,
            &device_,
            &chosenLevel,
            &context_);
    }

    if (FAILED(devHr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed,
            "D3D11CreateDevice failed: " + hr_to_string(devHr));
    }

    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    auto dupRes = recreate_duplication();
    if (!dupRes) {
        return dupRes;
    }

    return create_duplication_textures();
}

core::Result<void> DxgiCapture::recreate_duplication() {
    duplication_.Reset();
    const HRESULT hr = output1_->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed,
            "IDXGIOutput1::DuplicateOutput failed: " + hr_to_string(hr));
    }

    DXGI_OUTDUPL_DESC duplDesc{};
    duplication_->GetDesc(&duplDesc);
    width_ = duplDesc.ModeDesc.Width;
    height_ = duplDesc.ModeDesc.Height;

    if (width_ == 0 || height_ == 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed, "DXGI duplication returned zero dimensions");
    }

    return core::Result<void>::make_ok();
}

core::Result<void> DxgiCapture::create_duplication_textures() {
    D3D11_TEXTURE2D_DESC gpuDesc{};
    gpuDesc.Width = width_;
    gpuDesc.Height = height_;
    gpuDesc.MipLevels = 1;
    gpuDesc.ArraySize = 1;
    gpuDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    gpuDesc.SampleDesc.Count = 1;
    gpuDesc.SampleDesc.Quality = 0;
    gpuDesc.Usage = D3D11_USAGE_DEFAULT;
    gpuDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    gpuDesc.CPUAccessFlags = 0;
    gpuDesc.MiscFlags = 0;

    HRESULT hr = device_->CreateTexture2D(&gpuDesc, nullptr, &desktopCleanTex_);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed,
            "CreateTexture2D(desktopCleanTex_) failed: " + hr_to_string(hr));
    }

    hr = device_->CreateTexture2D(&gpuDesc, nullptr, &compositeTex_);
    if (FAILED(hr)) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed,
            "CreateTexture2D(compositeTex_) failed: " + hr_to_string(hr));
    }

    if (captureCursor_) {
        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = kMaxCursorPatchSize;
        stagingDesc.Height = kMaxCursorPatchSize;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device_->CreateTexture2D(&stagingDesc, nullptr, &cursorStagingTex_);
        if (FAILED(hr)) {
            cursorStagingTex_.Reset();
        }
    }

    return core::Result<void>::make_ok();
}

bool DxgiCapture::seed_initial_frame_gdi() {
    HDC screenDc = ::GetDC(nullptr);
    if (screenDc == nullptr) {
        return false;
    }

    HDC memDc = ::CreateCompatibleDC(screenDc);
    if (memDc == nullptr) {
        ::ReleaseDC(nullptr, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width_);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height_);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        ::DeleteDC(memDc);
        ::ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ oldObj = ::SelectObject(memDc, dib);
    const int srcW = std::max<int>(1, desktopBounds_.right - desktopBounds_.left);
    const int srcH = std::max<int>(1, desktopBounds_.bottom - desktopBounds_.top);

    bool ok = false;
    if (srcW == static_cast<int>(width_) && srcH == static_cast<int>(height_)) {
        ok = ::BitBlt(
                 memDc,
                 0,
                 0,
                 static_cast<int>(width_),
                 static_cast<int>(height_),
                 screenDc,
                 desktopBounds_.left,
                 desktopBounds_.top,
                 SRCCOPY | CAPTUREBLT) != FALSE;
    } else {
        ::SetStretchBltMode(memDc, HALFTONE);
        ok = ::StretchBlt(
                 memDc,
                 0,
                 0,
                 static_cast<int>(width_),
                 static_cast<int>(height_),
                 screenDc,
                 desktopBounds_.left,
                 desktopBounds_.top,
                 srcW,
                 srcH,
                 SRCCOPY | CAPTUREBLT) != FALSE;
    }

    if (ok) {
        auto* px = static_cast<std::uint8_t*>(bits);
        const std::size_t totalPixels = static_cast<std::size_t>(width_) * height_;
        for (std::size_t i = 0; i < totalPixels; ++i) {
            px[i * 4U + 3U] = 0xFFU;
        }
        context_->UpdateSubresource(desktopCleanTex_.Get(), 0, nullptr, bits, width_ * 4U, 0);
        hasInitialFrame_ = true;
    }

    ::SelectObject(memDc, oldObj);
    ::DeleteObject(dib);
    ::DeleteDC(memDc);
    ::ReleaseDC(nullptr, screenDc);
    return ok;
}

void DxgiCapture::update_pointer_state(const DXGI_OUTDUPL_FRAME_INFO& info) {
    if (!captureCursor_) {
        return;
    }

    if (info.LastMouseUpdateTime.QuadPart != 0) {
        pointerVisible_ = (info.PointerPosition.Visible != FALSE);
        pointerPos_ = info.PointerPosition.Position;
    }

    if (info.PointerShapeBufferSize > 0) {
        if (pointerShapeBuffer_.size() < info.PointerShapeBufferSize) {
            pointerShapeBuffer_.resize(info.PointerShapeBufferSize);
        }
        UINT required = 0;
        const HRESULT hr = duplication_->GetFramePointerShape(
            static_cast<UINT>(pointerShapeBuffer_.size()),
            pointerShapeBuffer_.data(),
            &required,
            &pointerShapeInfo_);
        if (SUCCEEDED(hr)) {
            hasPointerShape_ = true;
        }
    }
}

void DxgiCapture::build_composite_frame() {
    context_->CopyResource(compositeTex_.Get(), desktopCleanTex_.Get());

    if (!captureCursor_ || !pointerVisible_ || !hasPointerShape_ || !cursorStagingTex_) {
        return;
    }

    const std::uint32_t shapeW = pointerShapeInfo_.Width;
    const std::uint32_t shapeH =
        (pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
            ? (pointerShapeInfo_.Height / 2U)
            : pointerShapeInfo_.Height;

    if (shapeW == 0 || shapeH == 0) {
        return;
    }

    const LONG left = pointerPos_.x;
    const LONG top = pointerPos_.y;
    const LONG right = left + static_cast<LONG>(shapeW);
    const LONG bottom = top + static_cast<LONG>(shapeH);

    const LONG clipL = std::max<LONG>(0, left);
    const LONG clipT = std::max<LONG>(0, top);
    const LONG clipR = std::min<LONG>(static_cast<LONG>(width_), right);
    const LONG clipB = std::min<LONG>(static_cast<LONG>(height_), bottom);

    if (clipL >= clipR || clipT >= clipB) {
        return;
    }

    const std::uint32_t patchW = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(clipR - clipL), kMaxCursorPatchSize);
    const std::uint32_t patchH = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(clipB - clipT), kMaxCursorPatchSize);
    if (patchW == 0 || patchH == 0) {
        return;
    }

    const D3D11_BOX srcBox{
        static_cast<UINT>(clipL),
        static_cast<UINT>(clipT),
        0U,
        static_cast<UINT>(clipL) + patchW,
        static_cast<UINT>(clipT) + patchH,
        1U
    };
    context_->CopySubresourceRegion(
        cursorStagingTex_.Get(), 0, 0, 0, 0, desktopCleanTex_.Get(), 0, &srcBox);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(cursorStagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }

    const std::uint32_t tightPitch = patchW * 4U;
    cursorPatchBuffer_.resize(static_cast<std::size_t>(tightPitch) * patchH);
    for (std::uint32_t y = 0; y < patchH; ++y) {
        const auto* src = static_cast<const std::uint8_t*>(mapped.pData) +
                          static_cast<std::size_t>(y) * mapped.RowPitch;
        std::uint8_t* dst = cursorPatchBuffer_.data() + static_cast<std::size_t>(y) * tightPitch;
        std::memcpy(dst, src, tightPitch);
    }
    context_->Unmap(cursorStagingTex_.Get(), 0);

    const std::uint32_t cursorStartX = static_cast<std::uint32_t>(clipL - left);
    const std::uint32_t cursorStartY = static_cast<std::uint32_t>(clipT - top);

    blend_cursor_patch(
        cursorPatchBuffer_.data(),
        tightPitch,
        patchW,
        patchH,
        cursorStartX,
        cursorStartY,
        pointerShapeInfo_,
        pointerShapeBuffer_);

    const D3D11_BOX dstBox{
        static_cast<UINT>(clipL),
        static_cast<UINT>(clipT),
        0U,
        static_cast<UINT>(clipL) + patchW,
        static_cast<UINT>(clipT) + patchH,
        1U
    };
    context_->UpdateSubresource(
        compositeTex_.Get(), 0, &dstBox, cursorPatchBuffer_.data(), tightPitch, 0);
}

AcquireResult DxgiCapture::acquire_frame(std::uint32_t timeoutMs) {
    if (!duplication_) {
        return {AcquireStatus::Error, "DXGI duplication not initialized"};
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    const HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &frameInfo, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (!hasInitialFrame_) {
            (void)seed_initial_frame_gdi();
        }
        if (hasInitialFrame_) {
            build_composite_frame();
        }
        return {AcquireStatus::Timeout, {}};
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        return {AcquireStatus::AccessLost, "DXGI duplication access lost"};
    }

    if (FAILED(hr)) {
        return {AcquireStatus::Error, "AcquireNextFrame failed: " + hr_to_string(hr)};
    }

    ScopedFrameRelease releaseGuard(duplication_.Get());

    if (frameInfo.LastPresentTime.QuadPart != 0 || !hasInitialFrame_) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> acquiredTex;
        const HRESULT texHr = resource.As(&acquiredTex);
        if (FAILED(texHr)) {
            return {AcquireStatus::Error, "Acquired resource is not ID3D11Texture2D: " + hr_to_string(texHr)};
        }
        context_->CopyResource(desktopCleanTex_.Get(), acquiredTex.Get());
        hasInitialFrame_ = true;
    }

    update_pointer_state(frameInfo);
    if (hasInitialFrame_) {
        build_composite_frame();
    }

    return {AcquireStatus::NewFrame, {}};
}

core::Result<void> DxgiCapture::copy_to_staging(ID3D11Texture2D* stagingTex) const {
    if (!context_ || !compositeTex_ || stagingTex == nullptr) {
        return core::Result<void>::make_err(core::ErrorCode::CaptureFailed, "Invalid staging texture copy");
    }
    context_->CopyResource(stagingTex, compositeTex_.Get());
    return core::Result<void>::make_ok();
}

} // namespace shine::platform::win

namespace shine::core {

Result<std::vector<DisplayInfo>> enumerate_displays() {
    auto res = platform::win::enumerate_outputs_internal();
    if (!res) {
        return Result<std::vector<DisplayInfo>>::make_err(res.error());
    }
    std::vector<DisplayInfo> displays;
    displays.reserve(res.value().size());
    for (const auto& item : res.value()) {
        displays.push_back(item.info);
    }
    return Result<std::vector<DisplayInfo>>::make_ok(std::move(displays));
}

} // namespace shine::core
