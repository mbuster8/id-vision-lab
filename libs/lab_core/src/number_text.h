#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <string>

namespace lab_core {

// std::to_chars never consults the locale, so findings read "4.2 %" whatever the user's locale.
inline std::string oneDecimal(double value) {
    std::array<char, 32> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, 1);
    return {buffer.data(), result.ptr};
}

// The nearest whole number: "512".
inline std::string wholeNumber(double value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::lround(value));
    return {buffer.data(), result.ptr};
}

} // namespace lab_core
