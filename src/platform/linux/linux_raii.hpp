#pragma once

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cstddef>
#include <memory>
#include <string>

namespace shine::platform::linux_impl {

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() noexcept { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] int get() const noexcept { return fd_; }

    int release() noexcept {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }

    void reset(int next = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = next;
    }

private:
    int fd_{-1};
};

struct XDisplayDeleter {
    void operator()(Display* d) const noexcept {
        if (d != nullptr) {
            ::XCloseDisplay(d);
        }
    }
};
using XDisplayPtr = std::unique_ptr<Display, XDisplayDeleter>;

struct XImageDeleter {
    void operator()(XImage* img) const noexcept {
        if (img != nullptr) {
            XDestroyImage(img);
        }
    }
};
using XImagePtr = std::unique_ptr<XImage, XImageDeleter>;

class XShmBuffer {
public:
    XShmBuffer() noexcept = default;
    ~XShmBuffer() noexcept { reset(); }

    XShmBuffer(const XShmBuffer&) = delete;
    XShmBuffer& operator=(const XShmBuffer&) = delete;

    [[nodiscard]] bool create(Display* dpy, Visual* visual, unsigned int depth, int w, int h) noexcept {
        reset();
        if (dpy == nullptr || w <= 0 || h <= 0) {
            return false;
        }
        display_ = dpy;
        image_ = ::XShmCreateImage(dpy, visual, depth, ZPixmap, nullptr, &shmInfo_,
                                   static_cast<unsigned int>(w), static_cast<unsigned int>(h));
        if (image_ == nullptr) {
            return false;
        }
        const std::size_t bytes = static_cast<std::size_t>(image_->bytes_per_line) *
                                  static_cast<std::size_t>(image_->height);
        shmInfo_.shmid = ::shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (shmInfo_.shmid < 0) {
            XDestroyImage(image_);
            image_ = nullptr;
            return false;
        }
        void* addr = ::shmat(shmInfo_.shmid, nullptr, 0);
        if (addr == reinterpret_cast<void*>(-1)) {
            ::shmctl(shmInfo_.shmid, IPC_RMID, nullptr);
            XDestroyImage(image_);
            image_ = nullptr;
            shmInfo_.shmid = -1;
            return false;
        }
        shmInfo_.shmaddr = static_cast<char*>(addr);
        image_->data = shmInfo_.shmaddr;
        shmInfo_.readOnly = False;

        if (::XShmAttach(dpy, &shmInfo_) == 0) {
            ::shmdt(shmInfo_.shmaddr);
            ::shmctl(shmInfo_.shmid, IPC_RMID, nullptr);
            XDestroyImage(image_);
            image_ = nullptr;
            shmInfo_.shmaddr = nullptr;
            shmInfo_.shmid = -1;
            return false;
        }
        ::XSync(dpy, False);
        ::shmctl(shmInfo_.shmid, IPC_RMID, nullptr);
        attached_ = true;
        return true;
    }

    void reset() noexcept {
        if (attached_ && display_ != nullptr) {
            ::XShmDetach(display_, &shmInfo_);
            ::XSync(display_, False);
            attached_ = false;
        }
        if (image_ != nullptr) {
            image_->data = nullptr;
            XDestroyImage(image_);
            image_ = nullptr;
        }
        if (shmInfo_.shmaddr != nullptr) {
            ::shmdt(shmInfo_.shmaddr);
            shmInfo_.shmaddr = nullptr;
        }
        shmInfo_.shmid = -1;
        display_ = nullptr;
    }

    [[nodiscard]] bool valid() const noexcept { return attached_ && image_ != nullptr; }
    [[nodiscard]] XImage* image() const noexcept { return image_; }
    [[nodiscard]] XShmSegmentInfo* shm_info() noexcept { return &shmInfo_; }

private:
    Display* display_{nullptr};
    XImage* image_{nullptr};
    XShmSegmentInfo shmInfo_{};
    bool attached_{false};
};

struct AvFormatOutDeleter {
    void operator()(AVFormatContext* ctx) const noexcept {
        if (ctx != nullptr) {
            if (ctx->oformat != nullptr && (ctx->oformat->flags & AVFMT_NOFILE) == 0 && ctx->pb != nullptr) {
                ::avio_closep(&ctx->pb);
            }
            ::avformat_free_context(ctx);
        }
    }
};
using AvFormatOutPtr = std::unique_ptr<AVFormatContext, AvFormatOutDeleter>;

struct AvCodecCtxDeleter {
    void operator()(AVCodecContext* ctx) const noexcept {
        if (ctx != nullptr) {
            ::avcodec_free_context(&ctx);
        }
    }
};
using AvCodecCtxPtr = std::unique_ptr<AVCodecContext, AvCodecCtxDeleter>;

struct AvFrameDeleter {
    void operator()(AVFrame* f) const noexcept {
        if (f != nullptr) {
            ::av_frame_free(&f);
        }
    }
};
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

struct AvPacketDeleter {
    void operator()(AVPacket* p) const noexcept {
        if (p != nullptr) {
            ::av_packet_free(&p);
        }
    }
};
using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;

struct AvBufferRefDeleter {
    void operator()(AVBufferRef* ref) const noexcept {
        if (ref != nullptr) {
            ::av_buffer_unref(&ref);
        }
    }
};
using AvBufferRefPtr = std::unique_ptr<AVBufferRef, AvBufferRefDeleter>;

struct SwsCtxDeleter {
    void operator()(SwsContext* ctx) const noexcept {
        if (ctx != nullptr) {
            ::sws_freeContext(ctx);
        }
    }
};
using SwsCtxPtr = std::unique_ptr<SwsContext, SwsCtxDeleter>;

class VaDisplayGuard {
public:
    VaDisplayGuard() noexcept = default;
    ~VaDisplayGuard() noexcept { reset(); }

    VaDisplayGuard(const VaDisplayGuard&) = delete;
    VaDisplayGuard& operator=(const VaDisplayGuard&) = delete;

    [[nodiscard]] bool open(const char* drmDevicePath) noexcept {
        reset();
        fd_.reset(::open(drmDevicePath, O_RDWR));
        if (!fd_.valid()) {
            return false;
        }
        vaDisplay_ = ::vaGetDisplayDRM(fd_.get());
        if (vaDisplay_ == nullptr) {
            fd_.reset();
            return false;
        }
        int major = 0;
        int minor = 0;
        if (::vaInitialize(vaDisplay_, &major, &minor) != VA_STATUS_SUCCESS) {
            vaDisplay_ = nullptr;
            fd_.reset();
            return false;
        }
        initialized_ = true;
        return true;
    }

    void reset() noexcept {
        if (initialized_ && vaDisplay_ != nullptr) {
            ::vaTerminate(vaDisplay_);
            initialized_ = false;
        }
        vaDisplay_ = nullptr;
        fd_.reset();
    }

    [[nodiscard]] VADisplay get() const noexcept { return vaDisplay_; }
    [[nodiscard]] bool valid() const noexcept { return initialized_ && vaDisplay_ != nullptr; }

private:
    UniqueFd fd_;
    VADisplay vaDisplay_{nullptr};
    bool initialized_{false};
};

[[nodiscard]] inline std::string av_error_string(int err) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
    ::av_strerror(err, buf.data(), buf.size());
    return std::string(buf.data());
}

} // namespace shine::platform::linux_impl
