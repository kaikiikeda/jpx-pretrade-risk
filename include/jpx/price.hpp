// Fixed-point prices for JPX cash equities.
//
// JPX's smallest tick is 0.1 yen (TOPIX500 constituents priced at or below 1,000 yen),
// so every legal price is an integer number of tenths of a yen. Storing prices as
// int64 "deci-yen" makes tick checks exact: no floating-point modulo, no epsilon.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace jpx {

using Price = std::int64_t;  // tenths of a yen: 1234.5 yen == 12345
inline constexpr Price kScale = 10;

constexpr Price yen(std::int64_t whole) { return whole * kScale; }

// Parse a FIX price field ("2345", "2345.5", "2345.50", "2345.0"). Rejects anything
// that is not exactly representable in tenths of a yen (e.g. "100.25"), negative
// values, exponents and junk. Never allocates.
constexpr std::optional<Price> parse_price(std::string_view s) {
    if (s.empty() || s.size() > 18) return std::nullopt;
    Price whole = 0;
    std::size_t i = 0;
    bool any_digit = false;
    for (; i < s.size() && s[i] != '.'; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return std::nullopt;
        whole = whole * 10 + (c - '0');
        any_digit = true;
    }
    Price tenths = 0;
    if (i < s.size()) {  // s[i] == '.'
        if (!any_digit) return std::nullopt;  // ".5": a digit is required before the point
        ++i;
        if (i == s.size()) return std::nullopt;  // "123."
        tenths = 0;
        bool first = true;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (c < '0' || c > '9') return std::nullopt;
            if (first) {
                tenths = c - '0';
                first = false;
            } else if (c != '0') {
                return std::nullopt;  // finer than 0.1 yen: not a legal JPX price
            }
            any_digit = true;
        }
    }
    if (!any_digit) return std::nullopt;
    return whole * kScale + tenths;
}

inline std::string format_price(Price p) {
    std::string s = std::to_string(p / kScale);
    if (p % kScale) s += "." + std::to_string(p % kScale);
    return s;
}

}  // namespace jpx
