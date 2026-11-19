#pragma once

#include "shine/core/result.hpp"
#include "shine/core/types.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace shine::platform::win {

enum class AcquireStatus {
    NewFrame,
    Timeout,
    AccessLost,
    Error
};

struct AcquireResult {
    AcquireStatus status{AcquireStatus::Error};
    std::string errorMessage;
};

class DxgiCapture {
public:
    DxgiCapture() = default;
    ~DxgiCapture();

    DxgiCapture(const DxgiCapture&) = delete;
    DxgiCapture& operator=(const DxgiCapture&) = delete;
    DxgiCapture(DxgiCapture&&) = delete;
    DxgiCapture& operator=(DxgiCapture&&) = delete;

    [[nodiscard]] core::Result<void> init(std::uint32_t displayIndex, bool captureCursor);
    [[nodiscard]] AcquireResult acquire_frame(std::uint32_t timeoutMs);
    [[nodiscard]] core::Result<void> recreate_duplication();
    [[nodiscard]] core::Result<void> copy_to_staging(ID3D11Texture2D* stagingTex) const;

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return context_.Get(); }
    [[nodiscard]] ID3D11Texture2D* composite_texture() const noexcept { return compositeTex_.Get(); }

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool has_frame() const noexcept { return hasInitialFrame_; }

private:
    [[nodiscard]] core::Result<void> create_duplication_textures();
    [[nodiscard]] bool seed_initial_frame_gdi();
    void update_pointer_state(const DXGI_OUTDUPL_FRAME_INFO& info);
    void build_composite_frame();

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output1_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopCleanTex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> compositeTex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cursorStagingTex_;

    std::uint32_t width_{0};
    std::uint32_t height_{0};
    RECT desktopBounds_{};
    bool captureCursor_{true};
    bool hasInitialFrame_{false};

    bool pointerVisible_{false};
    POINT pointerPos_{};
    bool hasPointerShape_{false};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShapeInfo_{};
    std::vector<std::uint8_t> pointerShapeBuffer_;
    std::vector<std::uint8_t> cursorPatchBuffer_;
};

} // namespace shine::platform::win
