#pragma once

#include "shine/core/hotkey.hpp"
#include "shine/core/result.hpp"

#include <functional>
#include <memory>

namespace shine::service {

class IGlobalHotkeyService {
public:
    virtual ~IGlobalHotkeyService() = default;

    IGlobalHotkeyService(const IGlobalHotkeyService&) = delete;
    IGlobalHotkeyService& operator=(const IGlobalHotkeyService&) = delete;
    IGlobalHotkeyService(IGlobalHotkeyService&&) = delete;
    IGlobalHotkeyService& operator=(IGlobalHotkeyService&&) = delete;

    [[nodiscard]] virtual core::Result<void> register_hotkey(
        const core::HotkeyBinding& binding,
        std::function<void()> callback) = 0;

    virtual void unregister_hotkey() = 0;

protected:
    IGlobalHotkeyService() = default;
};

[[nodiscard]] std::unique_ptr<IGlobalHotkeyService> create_global_hotkey_service();

} // namespace shine::service
