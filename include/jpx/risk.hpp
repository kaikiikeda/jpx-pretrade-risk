// Pre-trade risk engine for JPX cash-equity orders.
//
// Background: an independent 2025 rebuild of pre-trade risk concepts the author first
// learned during an internship in March 2024. Written from scratch from JPX's public
// rules; contains no code or proprietary logic from that internship. See README.
//
// Every incoming NewOrderSingle is checked in a fixed order and either accepted or
// rejected with exactly one reason (the first rule it breaks). Checks fall into two
// groups:
//   * Exchange rules  - the order would be rejected by TSE anyway (daily price limit,
//     tick size, trading unit). Catching them before the exchange avoids rejects that
//     count against the member.
//   * Firm limits     - the broker's own controls (max order size, max notional, fat-
//     finger collar, per-account credit, duplicate order IDs, halted symbols).
//
// The hot path does no heap allocation: symbols resolve through a flat array indexed by
// the 4-character JPX securities code, and ClOrdIDs go into a pre-sized open-addressing
// table.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "jpx/fix.hpp"
#include "jpx/price.hpp"
#include "jpx/tables.hpp"

namespace jpx {

enum class Reject : std::uint8_t {
    None = 0,
    // message level
    MalformedMessage,
    UnsupportedMsgType,
    MissingField,
    DuplicateClOrdId,
    // reference data
    UnknownSymbol,
    SymbolHalted,
    UnknownAccount,
    // order fields
    InvalidSide,
    InvalidOrdType,
    InvalidQuantity,
    NotTradingUnit,
    InvalidPrice,
    // exchange rules
    OutsideDailyLimit,
    OffTick,
    // firm limits
    ExceedsMaxQty,
    ExceedsMaxNotional,
    FatFinger,
    ExceedsAccountCredit,
    ShortSellNotPermitted,
};
inline constexpr int kNumRejects = static_cast<int>(Reject::ShortSellNotPermitted) + 1;

const char* to_string(Reject r);

struct Instrument {
    char code[5] = {};           // JPX securities code, e.g. "7203" or "130A"
    TickTable tick_table = TickTable::Other;
    Price base_price = 0;        // previous close / special quote (deci-yen)
    std::int64_t trading_unit = 100;
    bool expand_upper = false;   // limit broadened on the upper side today
    bool expand_lower = false;
    bool halted = false;
    // derived at load time
    PriceBand band{};
};

struct Limits {
    std::int64_t max_order_qty = 1'000'000;          // shares
    std::int64_t max_order_notional_yen = 500'000'000;  // 500 million yen per order
    int fat_finger_bps = 0;                          // 0 = off; else max |px/base - 1| in bps
    bool allow_short_sell = false;
};

struct Account {
    std::string id;
    std::int64_t credit_limit_yen = 0;   // max gross notional of accepted orders today
    std::int64_t used_yen = 0;
};

struct Decision {
    Reject reason = Reject::None;
    std::int64_t notional_yen = 0;  // worst-case notional used for credit (accepted orders)
    bool accepted() const { return reason == Reject::None; }
};

class RiskEngine {
public:
    explicit RiskEngine(Limits limits = {}, std::size_t expected_orders = 1 << 20);

    // Reference data. add_instrument computes the day's price band from the base price.
    void add_instrument(Instrument inst);
    void add_account(std::string id, std::int64_t credit_limit_yen);
    const Instrument* instrument(std::string_view code) const;
    const Account* account(std::string_view id) const;

    // Parse and check one raw FIX message.
    Decision check(std::string_view raw, char delim = fix::SOH);
    // Check an already-parsed message.
    Decision check(const fix::NewOrder& o);

    const std::array<std::uint64_t, kNumRejects>& counts() const { return counts_; }
    void reset_day();  // clears ClOrdIDs and credit usage

private:
    static int code_index(std::string_view code);  // -1 if not a valid 4-char code
    int account_index(std::string_view id) const;
    bool remember_cl_ord_id(std::string_view account, std::string_view id);  // false if duplicate

    Limits limits_;
    std::vector<Instrument> instruments_;
    std::vector<std::int32_t> by_code_;  // 36^4 slots -> index into instruments_ or -1
    std::vector<Account> accounts_;

    // ClOrdID set: open addressing on 64-bit hashes (8 bytes per slot keeps the table
    // small enough to stay mostly in cache); full keys live in an append-only arena and
    // are compared only when two hashes match.
    std::vector<std::uint64_t> id_hash_;
    std::vector<std::uint32_t> id_off_;
    std::vector<char> id_arena_;
    std::size_t ids_used_ = 0;
    std::array<std::uint64_t, kNumRejects> counts_{};
};

}  // namespace jpx
