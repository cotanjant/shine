#include "platform/linux/x11_capture.hpp"

#include "shine/core/recorder.hpp"

#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace shine::platform::linux_impl {

namespace {

struct MonitorGeom {
    int x{0};
    int y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t refreshHz{60};
    bool isPrimary{false};
    std::string name;
};

[[nodiscard]] core::Result<std::vector<MonitorGeom>> query_monitors(Display* dpy) {
    if (dpy == nullptr) {
        return core::Result<std::vector<MonitorGeom>>::make_err(
            core::ErrorCode::CaptureFailed, "Failed to open X11 display");
    }

    const int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    std::vector<MonitorGeom> out;

    int rrEvent = 0;
    int rrError = 0;
    if (::XRRQueryExtension(dpy, &rrEvent, &rrError) != 0) {
        int count = 0;
        XRRMonitorInfo* monitors = ::XRRGetMonitors(dpy, root, True, &count);
        if (monitors != nullptr) {
            for (int i = 0; i < count; ++i) {
                if (monitors[i].width <= 0 || monitors[i].height <= 0) {
                    continue;
                }
                MonitorGeom g{};
                g.x = monitors[i].x;
                g.y = monitors[i].y;
                g.width = static_cast<std::uint32_t>(monitors[i].width);
                g.height = static_cast<std::uint32_t>(monitors[i].height);
                g.refreshHz = 60;
                g.isPrimary = (monitors[i].primary != 0);
                if (monitors[i].name != kX11None) {
                    char* atomName = ::XGetAtomName(dpy, monitors[i].name);
                    if (atomName != nullptr) {
                        g.name = atomName;
                        ::XFree(atomName);
                    }
                }
                if (g.name.empty()) {
                    g.name = "Display " + std::to_string(out.size());
                }
                out.push_back(std::move(g));
            }
            ::XRRFreeMonitors(monitors);
        }
    }

    if (out.empty()) {
        MonitorGeom fallback{};
        fallback.x = 0;
        fallback.y = 0;
        fallback.width = static_cast<std::uint32_t>(DisplayWidth(dpy, screen));
        fallback.height = static_cast<std::uint32_t>(DisplayHeight(dpy, screen));
        fallback.refreshHz = 60;
        fallback.isPrimary = true;
        fallback.name = "Display 0";
        if (fallback.width > 0 && fallback.height > 0) {
            out.push_back(std::move(fallback));
        }
    }

    return core::Result<std::vector<MonitorGeom>>::make_ok(std::move(out));
}

} // namespace

core::Result<void> X11Capture::init(std::uint32_t displayIndex, bool captureCursor) {
    captureCursor_ = captureCursor;
    display_.reset(::XOpenDisplay(nullptr));
    if (!display_) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed, "XOpenDisplay failed (is DISPLAY set?)");
    }

    auto monRes = query_monitors(display_.get());
    if (!monRes) {
        return core::Result<void>::make_err(monRes.error());
    }
    const auto& monitors = monRes.value();
    if (monitors.empty()) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed, "No active X11 displays found");
    }
    if (displayIndex >= monitors.size()) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument,
            "Display index " + std::to_string(displayIndex) + " out of range");
    }

    const int screen = DefaultScreen(display_.get());
    root_ = RootWindow(display_.get(), screen);

    originX_ = monitors[displayIndex].x;
    originY_ = monitors[displayIndex].y;
    width_ = monitors[displayIndex].width;
    height_ = monitors[displayIndex].height;

    bgraBuffer_.resize(static_cast<std::size_t>(width_) * height_ * 4U);

    int xfEvent = 0;
    int xfError = 0;
    hasXFixes_ = captureCursor_ && (::XFixesQueryExtension(display_.get(), &xfEvent, &xfError) != 0);

    useShm_ = false;
    if (::XShmQueryExtension(display_.get()) != 0) {
        Visual* visual = DefaultVisual(display_.get(), screen);
        const auto depth = static_cast<unsigned int>(DefaultDepth(display_.get(), screen));
        useShm_ = shm_.create(display_.get(), visual, depth,
                              static_cast<int>(width_), static_cast<int>(height_));
    }

    return core::Result<void>::make_ok();
}

void X11Capture::copy_ximage_to_bgra(const XImage* img) {
    if (img == nullptr || img->data == nullptr) {
        return;
    }

    const std::uint32_t dstPitch = width_ * 4U;
    if (img->bits_per_pixel == 32) {
        for (std::uint32_t y = 0; y < height_; ++y) {
            const auto* src = reinterpret_cast<const std::uint8_t*>(img->data) +
                              static_cast<std::size_t>(y) * static_cast<std::size_t>(img->bytes_per_line);
            std::uint8_t* dst = bgraBuffer_.data() + static_cast<std::size_t>(y) * dstPitch;
            std::memcpy(dst, src, dstPitch);
        }
    } else if (img->bits_per_pixel == 24) {
        for (std::uint32_t y = 0; y < height_; ++y) {
            const auto* src = reinterpret_cast<const std::uint8_t*>(img->data) +
                              static_cast<std::size_t>(y) * static_cast<std::size_t>(img->bytes_per_line);
            std::uint8_t* dst = bgraBuffer_.data() + static_cast<std::size_t>(y) * dstPitch;
            for (std::uint32_t x = 0; x < width_; ++x) {
                dst[x * 4U + 0U] = src[x * 3U + 0U];
                dst[x * 4U + 1U] = src[x * 3U + 1U];
                dst[x * 4U + 2U] = src[x * 3U + 2U];
                dst[x * 4U + 3U] = 0xFFU;
            }
        }
    }
}

void X11Capture::overlay_cursor() {
    if (!hasXFixes_) {
        return;
    }
    XFixesCursorImage* cursor = ::XFixesGetCursorImage(display_.get());
    if (cursor == nullptr) {
        return;
    }

    const int curX = static_cast<int>(cursor->x) - static_cast<int>(cursor->xhot) - originX_;
    const int curY = static_cast<int>(cursor->y) - static_cast<int>(cursor->yhot) - originY_;

    for (int cy = 0; cy < static_cast<int>(cursor->height); ++cy) {
        const int dy = curY + cy;
        if (dy < 0 || dy >= static_cast<int>(height_)) {
            continue;
        }
        std::uint8_t* dstRow = bgraBuffer_.data() + static_cast<std::size_t>(dy) * width_ * 4U;

        for (int cx = 0; cx < static_cast<int>(cursor->width); ++cx) {
            const int dx = curX + cx;
            if (dx < 0 || dx >= static_cast<int>(width_)) {
                continue;
            }
            const auto px = static_cast<std::uint32_t>(
                cursor->pixels[static_cast<std::size_t>(cy) * cursor->width + static_cast<std::size_t>(cx)]);
            const std::uint32_t a = (px >> 24U) & 0xFFU;
            if (a == 0U) {
                continue;
            }
            const std::uint32_t r = (px >> 16U) & 0xFFU;
            const std::uint32_t g = (px >> 8U) & 0xFFU;
            const std::uint32_t b = px & 0xFFU;

            std::uint8_t* dst = dstRow + static_cast<std::size_t>(dx) * 4U;
            if (a == 255U) {
                dst[0] = static_cast<std::uint8_t>(b);
                dst[1] = static_cast<std::uint8_t>(g);
                dst[2] = static_cast<std::uint8_t>(r);
            } else {
                const std::uint32_t invA = 255U - a;
                dst[0] = static_cast<std::uint8_t>(std::min<std::uint32_t>(255U, b + (dst[0] * invA) / 255U));
                dst[1] = static_cast<std::uint8_t>(std::min<std::uint32_t>(255U, g + (dst[1] * invA) / 255U));
                dst[2] = static_cast<std::uint8_t>(std::min<std::uint32_t>(255U, r + (dst[2] * invA) / 255U));
            }
        }
    }

    ::XFree(cursor);
}

core::Result<void> X11Capture::capture_frame() {
    if (!display_) {
        return core::Result<void>::make_err(core::ErrorCode::CaptureFailed, "X11 display closed");
    }

    if (useShm_ && shm_.valid()) {
        if (::XShmGetImage(display_.get(), root_, shm_.image(), originX_, originY_, AllPlanes) != 0) {
            copy_ximage_to_bgra(shm_.image());
            overlay_cursor();
            return core::Result<void>::make_ok();
        }
    }

    XImagePtr img(::XGetImage(
        display_.get(),
        root_,
        originX_,
        originY_,
        width_,
        height_,
        AllPlanes,
        ZPixmap));
    if (!img) {
        return core::Result<void>::make_err(
            core::ErrorCode::CaptureFailed, "XGetImage failed");
    }

    copy_ximage_to_bgra(img.get());
    overlay_cursor();
    return core::Result<void>::make_ok();
}

} // namespace shine::platform::linux_impl

namespace shine::core {

Result<std::vector<DisplayInfo>> enumerate_displays() {
    platform::linux_impl::XDisplayPtr dpy(::XOpenDisplay(nullptr));
    if (!dpy) {
        return Result<std::vector<DisplayInfo>>::make_err(
            ErrorCode::CaptureFailed, "Failed to open X11 display");
    }

    auto monRes = platform::linux_impl::query_monitors(dpy.get());
    if (!monRes) {
        return Result<std::vector<DisplayInfo>>::make_err(monRes.error());
    }

    std::vector<DisplayInfo> displays;
    const auto& mons = monRes.value();
    displays.reserve(mons.size());
    for (std::size_t i = 0; i < mons.size(); ++i) {
        DisplayInfo info{};
        info.index = static_cast<std::uint32_t>(i);
        info.width = mons[i].width;
        info.height = mons[i].height;
        info.refreshHz = mons[i].refreshHz;
        info.isPrimary = mons[i].isPrimary;
        info.name = mons[i].name;
        displays.push_back(std::move(info));
    }

    return Result<std::vector<DisplayInfo>>::make_ok(std::move(displays));
}

} // namespace shine::core
