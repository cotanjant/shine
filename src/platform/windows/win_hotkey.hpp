#pragma once

#include <string_view>

namespace shine::platform::win {

[[nodiscard]] unsigned int win_hotkey_to_vk(std::string_view keyName) noexcept;

} // namespace shine::platform::win
