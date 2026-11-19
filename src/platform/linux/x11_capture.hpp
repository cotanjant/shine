#pragma once

#include "platform/linux/linux_raii.hpp"
#include "shine/core/result.hpp"

#include <cstdint>
#include <vector>

namespace shine::platform::linux_impl {

class X11Capture {
public:
    X11Capture() = default;
    ~X11Capture() = default;

    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    [[nodiscard]] core::Result<void> init(std::uint32_t displayIndex, bool captureCursor);
    [[nodiscard]] core::Result<void> capture_frame();

    [[nodiscard]] const std::uint8_t* bgra_data() const noexcept { return bgraBuffer_.data(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t stride_bytes() const noexcept { return width_ * 4U; }

private:
    void copy_ximage_to_bgra(const XImage* img);
    void overlay_cursor();

    XDisplayPtr display_;
    Window root_{0};
    int originX_{0};
    int originY_{0};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    bool captureCursor_{true};
    bool hasXFixes_{false};
    bool useShm_{false};

    XShmBuffer shm_;
    std::vector<std::uint8_t> bgraBuffer_;
};

} // namespace shine::platform::linux_impl
