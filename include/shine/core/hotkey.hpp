#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace shine::core {

enum class KeyModifier : std::uint32_t {
    None = 0U,
    Ctrl = 1U << 0U,
    Alt = 1U << 1U,
    Shift = 1U << 2U,
    Super = 1U << 3U
};

[[nodiscard]] inline constexpr bool has_modifier(std::uint32_t mask, KeyModifier mod) noexcept {
    return (mask & static_cast<std::uint32_t>(mod)) != 0U;
}

struct HotkeyBinding {
    std::uint32_t modifiers{0};
    std::string key;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string to_string() const;

    bool operator==(const HotkeyBinding&) const = default;
};

[[nodiscard]] std::optional<HotkeyBinding> parse_hotkey(std::string_view text);

} // namespace shine::core
