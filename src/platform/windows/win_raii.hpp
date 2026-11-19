#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <combaseapi.h>
#include <dxgi1_2.h>
#include <mfapi.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace shine::platform::win {

class ComScope {
public:
    ComScope() noexcept {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            ok_ = true;
            shouldUninit_ = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            ok_ = true;
            shouldUninit_ = false;
        } else {
            hr_ = hr;
        }
    }

    ~ComScope() noexcept {
        if (shouldUninit_) {
            ::CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] HRESULT hr() const noexcept { return hr_; }

private:
    bool ok_{false};
    bool shouldUninit_{false};
    HRESULT hr_{S_OK};
};

class MfScope {
public:
    MfScope() noexcept {
        hr_ = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
        ok_ = SUCCEEDED(hr_);
    }

    ~MfScope() noexcept {
        if (ok_) {
            ::MFShutdown();
        }
    }

    MfScope(const MfScope&) = delete;
    MfScope& operator=(const MfScope&) = delete;
    MfScope(MfScope&&) = delete;
    MfScope& operator=(MfScope&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] HRESULT hr() const noexcept { return hr_; }

private:
    bool ok_{false};
    HRESULT hr_{S_OK};
};

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}

    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    HANDLE release() noexcept {
        HANDLE tmp = handle_;
        handle_ = nullptr;
        return tmp;
    }

    void reset(HANDLE next = nullptr) noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = next;
    }

private:
    HANDLE handle_{nullptr};
};

class ScopedFrameRelease {
public:
    explicit ScopedFrameRelease(IDXGIOutputDuplication* dupl) noexcept
        : dupl_(dupl) {}

    ~ScopedFrameRelease() noexcept {
        if (dupl_ != nullptr) {
            dupl_->ReleaseFrame();
        }
    }

    ScopedFrameRelease(const ScopedFrameRelease&) = delete;
    ScopedFrameRelease& operator=(const ScopedFrameRelease&) = delete;

private:
    IDXGIOutputDuplication* dupl_{nullptr};
};

class ScopedInputDesktop {
public:
    ScopedInputDesktop() noexcept = default;
    ~ScopedInputDesktop() noexcept { restore(); }

    ScopedInputDesktop(const ScopedInputDesktop&) = delete;
    ScopedInputDesktop& operator=(const ScopedInputDesktop&) = delete;
    ScopedInputDesktop(ScopedInputDesktop&&) = delete;
    ScopedInputDesktop& operator=(ScopedInputDesktop&&) = delete;

    [[nodiscard]] bool attach() noexcept {
        if (prevDesktop_ == nullptr) {
            prevDesktop_ = ::GetThreadDesktop(::GetCurrentThreadId());
        }
        HDESK newDesk = ::OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
        if (newDesk == nullptr) {
            return false;
        }
        if (::SetThreadDesktop(newDesk) == FALSE) {
            ::CloseDesktop(newDesk);
            return false;
        }
        if (inputDesktop_ != nullptr) {
            ::CloseDesktop(inputDesktop_);
        }
        inputDesktop_ = newDesk;
        return true;
    }

    void restore() noexcept {
        if (inputDesktop_ != nullptr) {
            if (prevDesktop_ != nullptr) {
                ::SetThreadDesktop(prevDesktop_);
            }
            ::CloseDesktop(inputDesktop_);
            inputDesktop_ = nullptr;
        }
    }

private:
    HDESK prevDesktop_{nullptr};
    HDESK inputDesktop_{nullptr};
};

[[nodiscard]] inline std::string hr_to_string(HRESULT hr) {
    char hexBuf[32]{};
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%08lX", static_cast<unsigned long>(hr));

    LPSTR msgBuf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = ::FormatMessageA(
        flags,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&msgBuf),
        0,
        nullptr);

    std::string result(hexBuf);
    if (len > 0 && msgBuf != nullptr) {
        std::string_view msg(msgBuf, len);
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) {
            msg.remove_suffix(1);
        }
        if (!msg.empty()) {
            result += " (";
            result.append(msg);
            result += ')';
        }
        ::LocalFree(msgBuf);
    }
    return result;
}

[[nodiscard]] inline std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

[[nodiscard]] inline std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

} // namespace shine::platform::win
