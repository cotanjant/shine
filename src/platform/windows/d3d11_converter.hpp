#pragma once

#include "shine/core/result.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace shine::platform::win {

class D3D11BgraToNv12Converter {
public:
    D3D11BgraToNv12Converter() = default;
    ~D3D11BgraToNv12Converter();

    D3D11BgraToNv12Converter(const D3D11BgraToNv12Converter&) = delete;
    D3D11BgraToNv12Converter& operator=(const D3D11BgraToNv12Converter&) = delete;
    D3D11BgraToNv12Converter(D3D11BgraToNv12Converter&&) = delete;
    D3D11BgraToNv12Converter& operator=(D3D11BgraToNv12Converter&&) = delete;

    [[nodiscard]] core::Result<void> init(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* srcBgraTex,
        std::uint32_t srcWidth,
        std::uint32_t srcHeight,
        std::uint32_t dstWidth,
        std::uint32_t dstHeight);

    [[nodiscard]] core::Result<void> convert(ID3D11VideoProcessorOutputView* dstView);

    [[nodiscard]] ID3D11VideoDevice* video_device() const noexcept { return videoDevice_.Get(); }
    [[nodiscard]] ID3D11VideoProcessorEnumerator* enumerator() const noexcept { return enumerator_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView_;
};

} // namespace shine::platform::win
