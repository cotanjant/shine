#include "platform/windows/win_hotkey.hpp"

#include "platform/windows/win_raii.hpp"
#include "shine/service/hotkey_service.hpp"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace shine::platform::win {

unsigned int win_hotkey_to_vk(std::string_view keyName) noexcept {
    if (keyName.size() == 1U) {
        const char ch = keyName[0];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            return static_cast<unsigned int>(ch);
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
            return static_cast<unsigned int>(VK_F1 + (fn - 1));
        }
    }

    if (keyName == "Space") return VK_SPACE;
    if (keyName == "Tab") return VK_TAB;
    if (keyName == "CapsLock") return VK_CAPITAL;
    if (keyName == "Insert") return VK_INSERT;
    if (keyName == "Delete") return VK_DELETE;
    if (keyName == "Home") return VK_HOME;
    if (keyName == "End") return VK_END;
    if (keyName == "PageUp") return VK_PRIOR;
    if (keyName == "PageDown") return VK_NEXT;
    if (keyName == "Pause") return VK_PAUSE;
    if (keyName == "PrintScreen") return VK_SNAPSHOT;
    if (keyName == "Backquote") return VK_OEM_3;
    if (keyName == "Minus") return VK_OEM_MINUS;
    if (keyName == "Equal") return VK_OEM_PLUS;
    if (keyName == "BracketLeft") return VK_OEM_4;
    if (keyName == "BracketRight") return VK_OEM_6;

    return 0U;
}

namespace {

constexpr UINT kMsgRebind = WM_APP + 1;
constexpr UINT kMsgUnregister = WM_APP + 2;
constexpr int kHotkeyId = 1;

[[nodiscard]] UINT to_win_modifiers(std::uint32_t mods) noexcept {
    UINT winMods = MOD_NOREPEAT;
    if (core::has_modifier(mods, core::KeyModifier::Ctrl)) {
        winMods |= MOD_CONTROL;
    }
    if (core::has_modifier(mods, core::KeyModifier::Alt)) {
        winMods |= MOD_ALT;
    }
    if (core::has_modifier(mods, core::KeyModifier::Shift)) {
        winMods |= MOD_SHIFT;
    }
    if (core::has_modifier(mods, core::KeyModifier::Super)) {
        winMods |= MOD_WIN;
    }
    return winMods;
}

class WinGlobalHotkeyService final : public service::IGlobalHotkeyService {
public:
    WinGlobalHotkeyService() = default;
    ~WinGlobalHotkeyService() override {
        stop_thread();
    }

    core::Result<void> register_hotkey(
        const core::HotkeyBinding& binding,
        std::function<void()> callback) override {
        if (!binding.valid() || !callback) {
            return core::Result<void>::make_err(
                core::ErrorCode::InvalidArgument, "Invalid hotkey binding or callback");
        }

        const unsigned int vk = win_hotkey_to_vk(binding.key);
        if (vk == 0U) {
            return core::Result<void>::make_err(
                core::ErrorCode::InvalidArgument, "Unsupported hotkey key: " + binding.key);
        }

        auto threadRes = ensure_thread();
        if (!threadRes) {
            return threadRes;
        }

        std::promise<core::Result<void>> rebindPromise;
        auto rebindFuture = rebindPromise.get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingMods_ = to_win_modifiers(binding.modifiers);
            pendingVk_ = static_cast<UINT>(vk);
            pendingCallback_ = std::move(callback);
            pendingRebindPromise_ = &rebindPromise;
        }

        ::PostMessageW(hwnd_, kMsgRebind, 0, 0);
        return rebindFuture.get();
    }

    void unregister_hotkey() override {
        if (!running_.load(std::memory_order_acquire) || hwnd_ == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeCallback_ = nullptr;
        }
        ::PostMessageW(hwnd_, kMsgUnregister, 0, 0);
    }

private:
    core::Result<void> ensure_thread() {
        if (running_.load(std::memory_order_acquire)) {
            return core::Result<void>::make_ok();
        }
        if (thread_.joinable()) {
            thread_.join();
        }

        std::promise<core::Result<void>> readyPromise;
        auto readyFuture = readyPromise.get_future();
        thread_ = std::thread(&WinGlobalHotkeyService::thread_main, this, std::move(readyPromise));
        return readyFuture.get();
    }

    void stop_thread() {
        if (hwnd_ != nullptr) {
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        hwnd_ = nullptr;
        running_.store(false, std::memory_order_release);
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<WinGlobalHotkeyService*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<WinGlobalHotkeyService*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }

        if (self == nullptr) {
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        switch (msg) {
        case kMsgRebind: {
            ::UnregisterHotKey(hwnd, kHotkeyId);
            UINT mods = 0;
            UINT vk = 0;
            std::function<void()> cb;
            std::promise<core::Result<void>>* prom = nullptr;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                mods = self->pendingMods_;
                vk = self->pendingVk_;
                cb = std::move(self->pendingCallback_);
                prom = self->pendingRebindPromise_;
                self->pendingRebindPromise_ = nullptr;
            }
            const BOOL ok = ::RegisterHotKey(hwnd, kHotkeyId, mods, vk);
            if (ok != FALSE) {
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    self->activeCallback_ = std::move(cb);
                    self->registered_ = true;
                }
                if (prom != nullptr) {
                    prom->set_value(core::Result<void>::make_ok());
                }
            } else {
                const DWORD err = ::GetLastError();
                if (prom != nullptr) {
                    prom->set_value(core::Result<void>::make_err(
                        core::ErrorCode::InvalidArgument,
                        "RegisterHotKey failed (" + std::to_string(err) + ")"));
                }
            }
            return 0;
        }

        case kMsgUnregister:
            ::UnregisterHotKey(hwnd, kHotkeyId);
            self->registered_ = false;
            return 0;

        case WM_HOTKEY:
            if (static_cast<int>(wParam) == kHotkeyId) {
                std::function<void()> cb;
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    cb = self->activeCallback_;
                }
                if (cb) {
                    cb();
                }
            }
            return 0;

        case WM_CLOSE:
            if (self->registered_) {
                ::UnregisterHotKey(hwnd, kHotkeyId);
                self->registered_ = false;
            }
            ::DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void thread_main(std::promise<core::Result<void>> readyPromise) {
        const wchar_t* kClass = L"ShineGlobalHotkeyWindow";
        HINSTANCE hInst = ::GetModuleHandleW(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = hInst;
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);

        hwnd_ = ::CreateWindowExW(
            0,
            kClass,
            L"ShineHotkey",
            0,
            0,
            0,
            0,
            0,
            HWND_MESSAGE,
            nullptr,
            hInst,
            this);

        if (hwnd_ == nullptr) {
            readyPromise.set_value(core::Result<void>::make_err(
                core::ErrorCode::InternalError, "Failed to create hotkey message window"));
            return;
        }

        running_.store(true, std::memory_order_release);
        readyPromise.set_value(core::Result<void>::make_ok());

        MSG msg{};
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        running_.store(false, std::memory_order_release);
    }

    HWND hwnd_{nullptr};
    bool registered_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex mutex_;
    UINT pendingMods_{0};
    UINT pendingVk_{0};
    std::function<void()> pendingCallback_;
    std::promise<core::Result<void>>* pendingRebindPromise_{nullptr};
    std::function<void()> activeCallback_;
};

} // namespace

} // namespace shine::platform::win

namespace shine::service {

std::unique_ptr<IGlobalHotkeyService> create_global_hotkey_service() {
    return std::make_unique<platform::win::WinGlobalHotkeyService>();
}

} // namespace shine::service
