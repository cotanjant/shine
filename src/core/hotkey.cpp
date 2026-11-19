#include "shine/core/hotkey.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <vector>

namespace shine::core {

namespace {

[[nodiscard]] std::string to_lower_ascii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (const char ch : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

[[nodiscard]] std::string_view trim_ascii(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())) != 0) {
        sv.remove_suffix(1);
    }
    return sv;
}

[[nodiscard]] std::optional<std::string> normalize_key_name(std::string_view raw) {
    const std::string lower = to_lower_ascii(trim_ascii(raw));
    if (lower.empty()) {
        return std::nullopt;
    }

    if (lower.size() == 1U) {
        const char ch = lower[0];
        if (ch >= 'a' && ch <= 'z') {
            return std::string(1U, static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        if (ch >= '0' && ch <= '9') {
            return std::string(1U, ch);
        }
    }

    if (lower.size() >= 2U && lower.size() <= 3U && lower[0] == 'f') {
        int fn = 0;
        if (lower.size() == 2U && lower[1] >= '1' && lower[1] <= '9') {
            fn = lower[1] - '0';
        } else if (lower.size() == 3U && lower[1] == '1' && lower[2] >= '0' && lower[2] <= '2') {
            fn = 10 + (lower[2] - '0');
        }
        if (fn >= 1 && fn <= 12) {
            return "F" + std::to_string(fn);
        }
    }

    struct NamedKey {
        std::string_view alias;
        std::string_view canonical;
    };
    static constexpr std::array<NamedKey, 16> kNamedKeys{{
        {"space", "Space"},
        {"tab", "Tab"},
        {"capslock", "CapsLock"},
        {"insert", "Insert"},
        {"delete", "Delete"},
        {"del", "Delete"},
        {"home", "Home"},
        {"end", "End"},
        {"pageup", "PageUp"},
        {"pgup", "PageUp"},
        {"pagedown", "PageDown"},
        {"pgdn", "PageDown"},
        {"pause", "Pause"},
        {"printscreen", "PrintScreen"},
        {"backquote", "Backquote"},
        {"minus", "Minus"}
    }};

    for (const auto& entry : kNamedKeys) {
        if (lower == entry.alias) {
            return std::string(entry.canonical);
        }
    }

    if (lower == "equal" || lower == "=") {
        return std::string("Equal");
    }
    if (lower == "bracketleft" || lower == "[") {
        return std::string("BracketLeft");
    }
    if (lower == "bracketright" || lower == "]") {
        return std::string("BracketRight");
    }
    if (lower == "`") {
        return std::string("Backquote");
    }
    if (lower == "-") {
        return std::string("Minus");
    }

    return std::nullopt;
}

} // namespace

bool HotkeyBinding::valid() const noexcept {
    return !key.empty() && normalize_key_name(key).has_value();
}

std::string HotkeyBinding::to_string() const {
    if (!valid()) {
        return {};
    }
    std::ostringstream oss;
    if (has_modifier(modifiers, KeyModifier::Ctrl)) {
        oss << "Ctrl+";
    }
    if (has_modifier(modifiers, KeyModifier::Alt)) {
        oss << "Alt+";
    }
    if (has_modifier(modifiers, KeyModifier::Shift)) {
        oss << "Shift+";
    }
    if (has_modifier(modifiers, KeyModifier::Super)) {
        oss << "Super+";
    }
    oss << key;
    return oss.str();
}

std::optional<HotkeyBinding> parse_hotkey(std::string_view text) {
    const std::string_view trimmed = trim_ascii(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t plus = trimmed.find('+', start);
        if (plus == std::string_view::npos) {
            parts.push_back(trim_ascii(trimmed.substr(start)));
            break;
        }
        parts.push_back(trim_ascii(trimmed.substr(start, plus - start)));
        start = plus + 1;
    }

    if (parts.empty()) {
        return std::nullopt;
    }

    std::uint32_t modifiers = 0U;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string mod = to_lower_ascii(parts[i]);
        if (mod == "ctrl" || mod == "control") {
            modifiers |= static_cast<std::uint32_t>(KeyModifier::Ctrl);
        } else if (mod == "alt") {
            modifiers |= static_cast<std::uint32_t>(KeyModifier::Alt);
        } else if (mod == "shift") {
            modifiers |= static_cast<std::uint32_t>(KeyModifier::Shift);
        } else if (mod == "super" || mod == "win" || mod == "meta") {
            modifiers |= static_cast<std::uint32_t>(KeyModifier::Super);
        } else {
            return std::nullopt;
        }
    }

    auto normKey = normalize_key_name(parts.back());
    if (!normKey.has_value()) {
        return std::nullopt;
    }

    return HotkeyBinding{modifiers, *normKey};
}

} // namespace shine::core
