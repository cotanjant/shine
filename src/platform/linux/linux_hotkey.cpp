#include "platform/linux/linux_hotkey.hpp"

#include "platform/linux/linux_raii.hpp"
#include "shine/service/hotkey_service.hpp"

#include <X11/XKBlib.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <future>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace shine::platform::linux_impl {

KeySym linux_hotkey_to_keysym(std::string_view keyName) noexcept {
    if (keyName.size() == 1U) {
        const char ch = keyName[0];
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<KeySym>(XK_a + (ch - 'A'));
        }
        if (ch >= '0' && ch <= '9') {
            return static_cast<KeySym>(XK_0 + (ch - '0'));
        }
    }

    if (keyName.size() >= 2U && keyName.size() <= 3U && keyName[0] == 'F') {
        int fn = 0;
        if (keyName.size() == 2U && keyName[1] >= '1' && keyName[1] <= '9') {
            fn = keyName[1] - '0';
        } else if (keyName.size() == 3U && keyName[1] == '1' && keyName[2] >= '0' && keyName[2] <= '2') {
            fn = 10 + (keyName[2] - '0');
        }
        if (fn >= 1 && fn <= 12) {
            return static_cast<KeySym>(XK_F1 + (fn - 1));
        }
    }

    if (keyName == "Space") return XK_space;
    if (keyName == "Tab") return XK_Tab;
    if (keyName == "CapsLock") return XK_Caps_Lock;
    if (keyName == "Insert") return XK_Insert;
    if (keyName == "Delete") return XK_Delete;
    if (keyName == "Home") return XK_Home;
    if (keyName == "End") return XK_End;
    if (keyName == "PageUp") return XK_Page_Up;
    if (keyName == "PageDown") return XK_Page_Down;
    if (keyName == "Pause") return XK_Pause;
    if (keyName == "PrintScreen") return XK_Print;
    if (keyName == "Backquote") return XK_grave;
    if (keyName == "Minus") return XK_minus;
    if (keyName == "Equal") return XK_equal;
    if (keyName == "BracketLeft") return XK_bracketleft;
    if (keyName == "BracketRight") return XK_bracketright;

    return 0UL;
}

namespace {

[[nodiscard]] unsigned int to_x11_modifiers(std::uint32_t mods) noexcept {
    unsigned int mask = 0U;
    if (core::has_modifier(mods, core::KeyModifier::Ctrl)) {
        mask |= ControlMask;
    }
    if (core::has_modifier(mods, core::KeyModifier::Alt)) {
        mask |= Mod1Mask;
    }
    if (core::has_modifier(mods, core::KeyModifier::Shift)) {
        mask |= ShiftMask;
    }
    if (core::has_modifier(mods, core::KeyModifier::Super)) {
        mask |= Mod4Mask;
    }
    return mask;
}

class LinuxGlobalHotkeyService final : public service::IGlobalHotkeyService {
public:
    LinuxGlobalHotkeyService() = default;
    ~LinuxGlobalHotkeyService() override {
        unregister_hotkey();
    }

    core::Result<void> register_hotkey(
        const core::HotkeyBinding& binding,
        std::function<void()> callback) override {
        if (!binding.valid() || !callback) {
            return core::Result<void>::make_err(
                core::ErrorCode::InvalidArgument, "Invalid hotkey binding or callback");
        }

        const KeySym keysym = linux_hotkey_to_keysym(binding.key);
        if (keysym == 0UL) {
            return core::Result<void>::make_err(
                core::ErrorCode::InvalidArgument, "Unsupported hotkey key: " + binding.key);
        }

        unregister_hotkey();

        int pipeFds[2]{-1, -1};
        if (::pipe(pipeFds) != 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::InternalError, "pipe() failed for hotkey service");
        }
        wakeReadFd_.reset(pipeFds[0]);
        wakeWriteFd_.reset(pipeFds[1]);
        ::fcntl(wakeReadFd_.get(), F_SETFL, O_NONBLOCK);
        ::fcntl(wakeWriteFd_.get(), F_SETFL, O_NONBLOCK);

        stopRequested_.store(false, std::memory_order_release);
        std::promise<core::Result<void>> readyPromise;
        auto readyFuture = readyPromise.get_future();

        worker_ = std::thread(
            &LinuxGlobalHotkeyService::worker_main,
            this,
            keysym,
            to_x11_modifiers(binding.modifiers),
            std::move(callback),
            std::move(readyPromise));

        core::Result<void> res = readyFuture.get();
        if (!res) {
            unregister_hotkey();
        }
        return res;
    }

    void unregister_hotkey() override {
        stopRequested_.store(true, std::memory_order_release);
        if (wakeWriteFd_.valid()) {
            const char b = 1;
            const ssize_t ignored = ::write(wakeWriteFd_.get(), &b, 1);
            (void)ignored;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        wakeReadFd_.reset();
        wakeWriteFd_.reset();
    }

private:
    void worker_main(
        KeySym keysym,
        unsigned int modMask,
        std::function<void()> callback,
        std::promise<core::Result<void>> readyPromise) {
        XDisplayPtr dpy(::XOpenDisplay(nullptr));
        if (!dpy) {
            readyPromise.set_value(core::Result<void>::make_err(
                core::ErrorCode::NotSupported, "XOpenDisplay failed for global hotkey"));
            return;
        }

        const KeyCode keycode = ::XKeysymToKeycode(dpy.get(), keysym);
        if (keycode == 0) {
            readyPromise.set_value(core::Result<void>::make_err(
                core::ErrorCode::InvalidArgument, "XKeysymToKeycode returned 0"));
            return;
        }

        Window root = DefaultRootWindow(dpy.get());
        const std::array<unsigned int, 4> lockVariants{
            0U,
            Mod2Mask,
            LockMask,
            static_cast<unsigned int>(Mod2Mask | LockMask)
        };

        for (const unsigned int extra : lockVariants) {
            ::XGrabKey(
                dpy.get(),
                static_cast<int>(keycode),
                modMask | extra,
                root,
                False,
                GrabModeAsync,
                GrabModeAsync);
        }
        ::XSelectInput(dpy.get(), root, KeyPressMask);
        ::XSync(dpy.get(), False);

        readyPromise.set_value(core::Result<void>::make_ok());

        const int xFd = ConnectionNumber(dpy.get());
        while (!stopRequested_.load(std::memory_order_acquire)) {
            while (::XPending(dpy.get()) > 0) {
                XEvent ev{};
                ::XNextEvent(dpy.get(), &ev);
                if (ev.type == KeyPress && ev.xkey.keycode == static_cast<unsigned int>(keycode)) {
                    const unsigned int cleanState = ev.xkey.state & ~(Mod2Mask | LockMask);
                    if (cleanState == modMask && callback) {
                        callback();
                    }
                }
            }

            pollfd fds[2]{};
            fds[0].fd = wakeReadFd_.get();
            fds[0].events = POLLIN;
            fds[1].fd = xFd;
            fds[1].events = POLLIN;

            const int pr = ::poll(fds, 2, -1);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if ((fds[0].revents & POLLIN) != 0) {
                break;
            }
        }

        for (const unsigned int extra : lockVariants) {
            ::XUngrabKey(dpy.get(), static_cast<int>(keycode), modMask | extra, root);
        }
        ::XSync(dpy.get(), False);
    }

    UniqueFd wakeReadFd_;
    UniqueFd wakeWriteFd_;
    std::atomic<bool> stopRequested_{false};
    std::thread worker_;
};

} // namespace

} // namespace shine::platform::linux_impl

namespace shine::service {

std::unique_ptr<IGlobalHotkeyService> create_global_hotkey_service() {
    return std::make_unique<platform::linux_impl::LinuxGlobalHotkeyService>();
}

} // namespace shine::service
