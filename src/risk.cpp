#include "jpx/risk.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace jpx {

const char* to_string(Reject r) {
    switch (r) {
        case Reject::None: return "ACCEPT";
        case Reject::MalformedMessage: return "MALFORMED_MESSAGE";
        case Reject::UnsupportedMsgType: return "UNSUPPORTED_MSG_TYPE";
        case Reject::MissingField: return "MISSING_FIELD";
        case Reject::DuplicateClOrdId: return "DUPLICATE_CLORDID";
        case Reject::UnknownSymbol: return "UNKNOWN_SYMBOL";
        case Reject::SymbolHalted: return "SYMBOL_HALTED";
        case Reject::UnknownAccount: return "UNKNOWN_ACCOUNT";
        case Reject::InvalidSide: return "INVALID_SIDE";
        case Reject::InvalidOrdType: return "INVALID_ORD_TYPE";
        case Reject::InvalidQuantity: return "INVALID_QUANTITY";
        case Reject::NotTradingUnit: return "NOT_TRADING_UNIT";
        case Reject::InvalidPrice: return "INVALID_PRICE";
        case Reject::OutsideDailyLimit: return "OUTSIDE_DAILY_LIMIT";
        case Reject::OffTick: return "OFF_TICK";
        case Reject::ExceedsMaxQty: return "EXCEEDS_MAX_QTY";
        case Reject::ExceedsMaxNotional: return "EXCEEDS_MAX_NOTIONAL";
        case Reject::FatFinger: return "FAT_FINGER";
        case Reject::ExceedsAccountCredit: return "EXCEEDS_ACCOUNT_CREDIT";
        case Reject::ShortSellNotPermitted: return "SHORT_SELL_NOT_PERMITTED";
    }
    return "?";
}

namespace {

constexpr int kCodeSlots = 36 * 36 * 36 * 36;

int alnum36(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    return -1;
}

bool parse_qty(std::string_view s, std::int64_t& out) {
    if (s.empty() || s.size() > 12) return false;
    std::int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

std::uint64_t fnv1a(std::string_view a, std::string_view b) {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : a) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    h = (h ^ 0x1Fu) * 1099511628211ull;  // separator
    for (char c : b) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h | 1;  // never 0, which marks an empty slot
}

}  // namespace

RiskEngine::RiskEngine(Limits limits, std::size_t expected_orders)
    : limits_(limits), by_code_(kCodeSlots, -1) {
    std::size_t cap = 1;
    while (cap < expected_orders * 2) cap <<= 1;  // load factor <= 0.5
    id_hash_.assign(cap, 0);
    id_off_.assign(cap, 0);
    id_arena_.reserve(expected_orders * 24);
}

int RiskEngine::code_index(std::string_view code) {
    if (code.size() != 4) return -1;
    int idx = 0;
    for (char c : code) {
        const int d = alnum36(c);
        if (d < 0) return -1;
        idx = idx * 36 + d;
    }
    return idx;
}

void RiskEngine::add_instrument(Instrument inst) {
    const int idx = code_index(inst.code);
    if (idx < 0) throw std::invalid_argument("invalid JPX code");
    if (inst.base_price <= 0 || !on_tick(inst.tick_table, inst.base_price))
        throw std::invalid_argument("base price must be positive and on tick");
    inst.band = price_band(inst.base_price, inst.expand_upper, inst.expand_lower);
    if (by_code_[idx] >= 0) {
        instruments_[by_code_[idx]] = inst;
    } else {
        by_code_[idx] = static_cast<std::int32_t>(instruments_.size());
        instruments_.push_back(inst);
    }
}

void RiskEngine::add_account(std::string id, std::int64_t credit_limit_yen) {
    accounts_.push_back(Account{std::move(id), credit_limit_yen, 0});
}

const Instrument* RiskEngine::instrument(std::string_view code) const {
    const int idx = code_index(code);
    if (idx < 0 || by_code_[idx] < 0) return nullptr;
    return &instruments_[by_code_[idx]];
}

int RiskEngine::account_index(std::string_view id) const {
    // Accounts per gateway session are few; a linear scan beats hashing here.
    for (std::size_t i = 0; i < accounts_.size(); ++i)
        if (accounts_[i].id == id) return static_cast<int>(i);
    return -1;
}

const Account* RiskEngine::account(std::string_view id) const {
    const int i = account_index(id);
    return i < 0 ? nullptr : &accounts_[i];
}

bool RiskEngine::remember_cl_ord_id(std::string_view acct, std::string_view id) {
    const std::size_t len = acct.size() + 1 + id.size();
    if (len > 255) return false;  // fail safe: absurdly long IDs are refused
    const std::uint64_t h = fnv1a(acct, id);
    const std::size_t mask = id_hash_.size() - 1;
    auto same_key = [&](std::uint32_t off) {
        const char* k = id_arena_.data() + off;
        return static_cast<unsigned char>(k[0]) == len && std::memcmp(k + 1, acct.data(), acct.size()) == 0 &&
               k[1 + acct.size()] == '\x1F' && std::memcmp(k + 2 + acct.size(), id.data(), id.size()) == 0;
    };
    for (std::size_t i = h & mask;; i = (i + 1) & mask) {
        const std::uint64_t slot = id_hash_[i];
        if (slot == 0) {
            if ((ids_used_ + 1) * 2 > id_hash_.size()) return false;  // table full: fail safe
            id_hash_[i] = h;
            id_off_[i] = static_cast<std::uint32_t>(id_arena_.size());
            id_arena_.push_back(static_cast<char>(len));
            id_arena_.insert(id_arena_.end(), acct.begin(), acct.end());
            id_arena_.push_back('\x1F');
            id_arena_.insert(id_arena_.end(), id.begin(), id.end());
            ++ids_used_;
            return true;
        }
        if (slot == h && same_key(id_off_[i])) return false;
    }
}

void RiskEngine::reset_day() {
    std::fill(id_hash_.begin(), id_hash_.end(), 0);
    id_arena_.clear();
    ids_used_ = 0;
    for (auto& a : accounts_) a.used_yen = 0;
    counts_.fill(0);
}

Decision RiskEngine::check(std::string_view raw, char delim) {
    fix::NewOrder o;
    if (fix::parse(raw, o, delim) != fix::ParseError::None) {
        ++counts_[static_cast<int>(Reject::MalformedMessage)];
        return {Reject::MalformedMessage, 0};
    }
    return check(o);
}

Decision RiskEngine::check(const fix::NewOrder& o) {
    auto reject = [&](Reject r) {
        ++counts_[static_cast<int>(r)];
        return Decision{r, 0};
    };

    // ---- message level
    if (o.msg_type != "D") return reject(Reject::UnsupportedMsgType);
    if (o.cl_ord_id.empty() || o.account.empty() || o.symbol.empty() || o.side_raw.empty() ||
        o.qty_raw.empty() || o.type_raw.empty())
        return reject(Reject::MissingField);

    // ---- reference data
    const Instrument* inst = instrument(o.symbol);
    if (!inst) return reject(Reject::UnknownSymbol);
    if (inst->halted) return reject(Reject::SymbolHalted);
    const int ai = account_index(o.account);
    if (ai < 0) return reject(Reject::UnknownAccount);
    Account& acct = accounts_[ai];

    // ---- order fields
    if (o.side_raw.size() != 1) return reject(Reject::InvalidSide);
    const char side = o.side_raw[0];
    if (side != '1' && side != '2' && side != '5') return reject(Reject::InvalidSide);
    if (side == '5' && !limits_.allow_short_sell) return reject(Reject::ShortSellNotPermitted);
    if (o.type_raw.size() != 1 || (o.type_raw[0] != '1' && o.type_raw[0] != '2'))
        return reject(Reject::InvalidOrdType);
    const bool is_limit = o.type_raw[0] == '2';

    std::int64_t qty = 0;
    if (!parse_qty(o.qty_raw, qty) || qty <= 0) return reject(Reject::InvalidQuantity);
    if (qty % inst->trading_unit != 0) return reject(Reject::NotTradingUnit);

    // ---- exchange rules (limit orders). Market orders are valued at the highest price
    // they could execute at today (the limit-up price), so gross-notional credit is
    // never understated whichever side they are on.
    Price px = 0;
    if (is_limit) {
        if (o.price_raw.empty()) return reject(Reject::MissingField);
        const auto p = parse_price(o.price_raw);
        if (!p || *p <= 0) return reject(Reject::InvalidPrice);
        px = *p;
        if (px < inst->band.lower || px > inst->band.upper) return reject(Reject::OutsideDailyLimit);
        if (!on_tick(inst->tick_table, px)) return reject(Reject::OffTick);
    } else {
        if (!o.price_raw.empty()) return reject(Reject::InvalidPrice);  // price on a market order
        px = inst->band.upper;
    }

    // ---- firm limits
    if (qty > limits_.max_order_qty) return reject(Reject::ExceedsMaxQty);
    // notional in yen, rounded up so the credit check is never optimistic
    const std::int64_t notional = (qty * px + (kScale - 1)) / kScale;
    if (notional > limits_.max_order_notional_yen) return reject(Reject::ExceedsMaxNotional);
    if (is_limit && limits_.fat_finger_bps > 0) {
        const std::int64_t dev = px > inst->base_price ? px - inst->base_price : inst->base_price - px;
        if (dev * 10'000 > static_cast<std::int64_t>(limits_.fat_finger_bps) * inst->base_price)
            return reject(Reject::FatFinger);
    }
    if (acct.used_yen + notional > acct.credit_limit_yen) return reject(Reject::ExceedsAccountCredit);

    // Duplicate check last, so a rejected order does not burn its ClOrdID.
    if (!remember_cl_ord_id(o.account, o.cl_ord_id)) return reject(Reject::DuplicateClOrdId);

    acct.used_yen += notional;
    ++counts_[0];
    return {Reject::None, notional};
}

}  // namespace jpx
