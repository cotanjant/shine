#include "shine/service/autostart.hpp"

#include "platform/windows/win_raii.hpp"

#include <windows.h>

#include <string>

namespace shine::service {

namespace {

constexpr const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValueName = L"ShineDaemon";

} // namespace

core::Result<void> install_autostart(const std::filesystem::path& daemonExe) {
    if (daemonExe.empty()) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Daemon executable path is empty");
    }

    HKEY rawKey = nullptr;
    const LONG openRc = ::RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRunKeyPath,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &rawKey,
        nullptr);
    if (openRc != ERROR_SUCCESS || rawKey == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "RegCreateKeyExW failed (" + std::to_string(openRc) + ")");
    }

    const std::wstring quoted = L"\"" + daemonExe.wstring() + L"\"";
    const DWORD byteLen = static_cast<DWORD>((quoted.size() + 1U) * sizeof(wchar_t));
    const LONG setRc = ::RegSetValueExW(
        rawKey,
        kRunValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(quoted.c_str()),
        byteLen);
    ::RegCloseKey(rawKey);

    if (setRc != ERROR_SUCCESS) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "RegSetValueExW failed (" + std::to_string(setRc) + ")");
    }

    return core::Result<void>::make_ok();
}

core::Result<void> uninstall_autostart() {
    HKEY rawKey = nullptr;
    const LONG openRc = ::RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kRunKeyPath,
        0,
        KEY_SET_VALUE,
        &rawKey);
    if (openRc == ERROR_FILE_NOT_FOUND) {
        return core::Result<void>::make_ok();
    }
    if (openRc != ERROR_SUCCESS || rawKey == nullptr) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "RegOpenKeyExW failed (" + std::to_string(openRc) + ")");
    }

    const LONG delRc = ::RegDeleteValueW(rawKey, kRunValueName);
    ::RegCloseKey(rawKey);
    if (delRc != ERROR_SUCCESS && delRc != ERROR_FILE_NOT_FOUND) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "RegDeleteValueW failed (" + std::to_string(delRc) + ")");
    }

    return core::Result<void>::make_ok();
}

bool is_autostart_installed() {
    HKEY rawKey = nullptr;
    const LONG openRc = ::RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kRunKeyPath,
        0,
        KEY_QUERY_VALUE,
        &rawKey);
    if (openRc != ERROR_SUCCESS || rawKey == nullptr) {
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    const LONG queryRc = ::RegQueryValueExW(rawKey, kRunValueName, nullptr, &type, nullptr, &size);
    ::RegCloseKey(rawKey);
    return queryRc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t);
}

core::Result<void> launch_daemon_detached(
    const std::filesystem::path& daemonExe,
    std::string_view endpointOverride) {
    if (daemonExe.empty()) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Daemon executable path is empty");
    }

    std::wstring cmdLine = L"\"" + daemonExe.wstring() + L"\"";
    if (!endpointOverride.empty()) {
        cmdLine += L" --endpoint \"";
        cmdLine += platform::win::utf8_to_wide(endpointOverride);
        cmdLine += L"\"";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const BOOL ok = ::CreateProcessW(
        daemonExe.c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &si,
        &pi);

    if (!ok) {
        const DWORD err = ::GetLastError();
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "CreateProcessW failed (" + std::to_string(err) + ")");
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return core::Result<void>::make_ok();
}

} // namespace shine::service
