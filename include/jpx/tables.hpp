// JPX (Tokyo Stock Exchange) tick-size and daily-price-limit tables.
//
// Source: JPX, "Tick Size" and "Daily Price Limits" pages (current tables, retrieved
// January 2025):
//   https://www.jpx.co.jp/english/equities/trading/domestic/07.html
//   https://www.jpx.co.jp/english/equities/trading/domestic/06.html
//
// Boundary conventions, exactly as JPX states them:
//   * Tick size: "up to X" is inclusive. A price of exactly 1,000 yen in a TOPIX500
//     name has a 0.1-yen tick; 1,000.5 has a 0.5-yen tick.
//   * Daily price limit: "less than X" is strict, applied to the BASE price (normally
//     the previous close or special quote), not to the order price.
#pragma once

#include <array>
#include <cstdint>

#include "jpx/price.hpp"

namespace jpx {

enum class TickTable : std::uint8_t { Topix500, Other };

struct TickBand {
    Price upto;  // inclusive upper bound of the band (deci-yen)
    Price tick;  // tick size in deci-yen
};

inline constexpr Price kNoUpper = INT64_MAX;

// TOPIX500 constituents (TOPIX100 + TOPIX Mid400); also ETFs/ETNs with unit >= 10.
inline constexpr std::array<TickBand, 11> kTopix500Ticks{{
    {yen(1'000), 1},          // 0.1 yen
    {yen(3'000), 5},          // 0.5
    {yen(10'000), yen(1)},
    {yen(30'000), yen(5)},
    {yen(100'000), yen(10)},
    {yen(300'000), yen(50)},
    {yen(1'000'000), yen(100)},
    {yen(3'000'000), yen(500)},
    {yen(10'000'000), yen(1'000)},
    {yen(30'000'000), yen(5'000)},
    {kNoUpper, yen(10'000)},
}};

// All other issues.
inline constexpr std::array<TickBand, 11> kOtherTicks{{
    {yen(3'000), yen(1)},
    {yen(5'000), yen(5)},
    {yen(30'000), yen(10)},
    {yen(50'000), yen(50)},
    {yen(300'000), yen(100)},
    {yen(500'000), yen(500)},
    {yen(3'000'000), yen(1'000)},
    {yen(5'000'000), yen(5'000)},
    {yen(30'000'000), yen(10'000)},
    {yen(50'000'000), yen(50'000)},
    {kNoUpper, yen(100'000)},
}};

constexpr Price tick_size(TickTable t, Price px) {
    const auto& bands = (t == TickTable::Topix500) ? kTopix500Ticks : kOtherTicks;
    for (const auto& b : bands)
        if (px <= b.upto) return b.tick;
    return bands.back().tick;
}

constexpr bool on_tick(TickTable t, Price px) { return px > 0 && px % tick_size(t, px) == 0; }

struct LimitBand {
    Price below;  // band applies when base price < below (strict)
    Price limit;  // daily price limit, +/- deci-yen
};

inline constexpr std::array<LimitBand, 34> kDailyLimits{{
    {yen(100), yen(30)},
    {yen(200), yen(50)},
    {yen(500), yen(80)},
    {yen(700), yen(100)},
    {yen(1'000), yen(150)},
    {yen(1'500), yen(300)},
    {yen(2'000), yen(400)},
    {yen(3'000), yen(500)},
    {yen(5'000), yen(700)},
    {yen(7'000), yen(1'000)},
    {yen(10'000), yen(1'500)},
    {yen(15'000), yen(3'000)},
    {yen(20'000), yen(4'000)},
    {yen(30'000), yen(5'000)},
    {yen(50'000), yen(7'000)},
    {yen(70'000), yen(10'000)},
    {yen(100'000), yen(15'000)},
    {yen(150'000), yen(30'000)},
    {yen(200'000), yen(40'000)},
    {yen(300'000), yen(50'000)},
    {yen(500'000), yen(70'000)},
    {yen(700'000), yen(100'000)},
    {yen(1'000'000), yen(150'000)},
    {yen(1'500'000), yen(300'000)},
    {yen(2'000'000), yen(400'000)},
    {yen(3'000'000), yen(500'000)},
    {yen(5'000'000), yen(700'000)},
    {yen(7'000'000), yen(1'000'000)},
    {yen(10'000'000), yen(1'500'000)},
    {yen(15'000'000), yen(3'000'000)},
    {yen(20'000'000), yen(4'000'000)},
    {yen(30'000'000), yen(5'000'000)},
    {yen(50'000'000), yen(7'000'000)},
    {kNoUpper, yen(10'000'000)},  // 50 million yen or more
}};

constexpr Price daily_limit(Price base) {
    for (const auto& b : kDailyLimits)
        if (base < b.below) return b.limit;
    return kDailyLimits.back().limit;
}

// When a stock is stuck at a limit with no trading on two consecutive days, JPX
// broadens that side's limit on the third business day. JPX broadens
// one side to 4x the normal width (e.g. base 745 yen: normal +/-150, upper set to 600).
inline constexpr int kExpansionMultiple = 4;

struct PriceBand {
    Price lower;  // lowest legal limit price (inclusive)
    Price upper;  // highest legal limit price (inclusive)
};

constexpr PriceBand price_band(Price base, bool expand_upper = false, bool expand_lower = false) {
    const Price w = daily_limit(base);
    const Price up = base + (expand_upper ? kExpansionMultiple * w : w);
    Price lo = base - (expand_lower ? kExpansionMultiple * w : w);
    if (lo < 1) lo = 1;  // smallest positive price; the tick check then applies normally
    return {lo, up};
}

}  // namespace jpx
