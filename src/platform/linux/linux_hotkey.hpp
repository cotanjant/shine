#pragma once

#include <X11/Xlib.h>
#include <X11/keysym.h>

#ifdef None
#undef None
#endif

#include <string_view>

namespace shine::platform::linux_impl {

[[nodiscard]] KeySym linux_hotkey_to_keysym(std::string_view keyName) noexcept;

} // namespace shine::platform::linux_impl
