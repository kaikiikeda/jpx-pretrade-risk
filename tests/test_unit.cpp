// Unit tests: price parsing, every JPX table boundary, FIX framing, each reject reason.
#include <string>

#include "jpx/fix.hpp"
#include "jpx/price.hpp"
#include "jpx/risk.hpp"
#include "jpx/tables.hpp"
#include "testing.hpp"

using namespace jpx;

// ------------------------------------------------------------------ prices
TEST("parse_price accepts JPX-representable prices") {
    CHECK_EQ(*parse_price("2345"), 23450);
    CHECK_EQ(*parse_price("2345.5"), 23455);
    CHECK_EQ(*parse_price("2345.50"), 23455);
    CHECK_EQ(*parse_price("0.1"), 1);
    CHECK_EQ(*parse_price("100.0"), 1000);
}

TEST("parse_price rejects junk and sub-0.1-yen prices") {
    for (const char* s : {"", ".", "1.", ".5", "-1", "1e3", "12a", "100.25", "100.05", "1,000", " 100"})
        CHECK(!parse_price(s).has_value());
}

// ------------------------------------------------------------------ tick sizes
TEST("TOPIX500 tick bands: inclusive upper bounds") {
    // {price at band top, tick there, tick just above}
    struct Row { Price top, tick, next; };
    const Row rows[] = {
        {yen(1'000), 1, 5},           {yen(3'000), 5, yen(1)},         {yen(10'000), yen(1), yen(5)},
        {yen(30'000), yen(5), yen(10)}, {yen(100'000), yen(10), yen(50)}, {yen(300'000), yen(50), yen(100)},
        {yen(1'000'000), yen(100), yen(500)}, {yen(3'000'000), yen(500), yen(1'000)},
        {yen(10'000'000), yen(1'000), yen(5'000)}, {yen(30'000'000), yen(5'000), yen(10'000)},
    };
    for (const auto& r : rows) {
        CHECK_EQ(tick_size(TickTable::Topix500, r.top), r.tick);
        CHECK_EQ(tick_size(TickTable::Topix500, r.top + 1), r.next);
    }
    CHECK_EQ(tick_size(TickTable::Topix500, yen(90'000'000)), yen(10'000));
}

TEST("Other-issues tick bands: inclusive upper bounds") {
    struct Row { Price top, tick, next; };
    const Row rows[] = {
        {yen(3'000), yen(1), yen(5)},          {yen(5'000), yen(5), yen(10)},
        {yen(30'000), yen(10), yen(50)},       {yen(50'000), yen(50), yen(100)},
        {yen(300'000), yen(100), yen(500)},    {yen(500'000), yen(500), yen(1'000)},
        {yen(3'000'000), yen(1'000), yen(5'000)}, {yen(5'000'000), yen(5'000), yen(10'000)},
        {yen(30'000'000), yen(10'000), yen(50'000)}, {yen(50'000'000), yen(50'000), yen(100'000)},
    };
    for (const auto& r : rows) {
        CHECK_EQ(tick_size(TickTable::Other, r.top), r.tick);
        CHECK_EQ(tick_size(TickTable::Other, r.top + 1), r.next);
    }
}

TEST("on_tick examples") {
    CHECK(on_tick(TickTable::Topix500, *parse_price("999.9")));
    CHECK(on_tick(TickTable::Topix500, *parse_price("1000.0")));
    CHECK(!on_tick(TickTable::Topix500, *parse_price("1000.1")));  // 0.5 tick above 1,000
    CHECK(on_tick(TickTable::Topix500, *parse_price("1000.5")));
    CHECK(!on_tick(TickTable::Other, *parse_price("2999.5")));
    CHECK(on_tick(TickTable::Other, yen(3'005)));
    CHECK(!on_tick(TickTable::Other, yen(3'001)));
    CHECK(!on_tick(TickTable::Other, 0));
}

// ------------------------------------------------------------------ daily limits
TEST("daily limit bands: strict 'less than' on the base price") {
    const std::pair<std::int64_t, std::int64_t> rows[] = {
        {100, 30}, {200, 50}, {500, 80}, {700, 100}, {1'000, 150}, {1'500, 300}, {2'000, 400},
        {3'000, 500}, {5'000, 700}, {7'000, 1'000}, {10'000, 1'500}, {15'000, 3'000},
        {20'000, 4'000}, {30'000, 5'000}, {50'000, 7'000}, {70'000, 10'000}, {100'000, 15'000},
        {150'000, 30'000}, {200'000, 40'000}, {300'000, 50'000}, {500'000, 70'000},
        {700'000, 100'000}, {1'000'000, 150'000}, {1'500'000, 300'000}, {2'000'000, 400'000},
        {3'000'000, 500'000}, {5'000'000, 700'000}, {7'000'000, 1'000'000}, {10'000'000, 1'500'000},
        {15'000'000, 3'000'000}, {20'000'000, 4'000'000}, {30'000'000, 5'000'000}, {50'000'000, 7'000'000},
    };
    for (const auto& [below, lim] : rows) {
        CHECK_EQ(daily_limit(yen(below) - 1), yen(lim));  // just below the boundary
    }
    // at each boundary the next band applies
    CHECK_EQ(daily_limit(yen(100)), yen(50));
    CHECK_EQ(daily_limit(yen(1'000)), yen(300));
    CHECK_EQ(daily_limit(yen(50'000'000)), yen(10'000'000));
}

TEST("price band and one-sided 4x expansion (TriIs, base 745)") {
    const auto normal = price_band(yen(745));
    CHECK_EQ(normal.upper, yen(895));
    CHECK_EQ(normal.lower, yen(595));
    const auto up = price_band(yen(745), /*expand_upper=*/true, false);
    CHECK_EQ(up.upper, yen(1'345));  // JPX: upper limit becomes 1,345 yen
    CHECK_EQ(up.lower, yen(595));    // lower side unchanged
    const auto low = price_band(yen(20));
    CHECK_EQ(low.lower, 1);          // floored at the smallest positive price
}

// ------------------------------------------------------------------ FIX framing
TEST("FIX parse: valid NewOrderSingle round-trips") {
    const auto m = fix::build_new_order("ORD1", "ACC001", "7203", '1', "100", '2', "2845.5");
    fix::NewOrder o;
    CHECK_EQ(fix::parse(m, o), fix::ParseError::None);
    CHECK(o.msg_type == "D");
    CHECK(o.cl_ord_id == "ORD1");
    CHECK(o.symbol == "7203");
    CHECK(o.price_raw == "2845.5");
    // readable '|' form parses with the same checksum rule
    const auto p = fix::build_new_order("ORD1", "ACC001", "7203", '1', "100", '2', "2845.5", '|');
    CHECK_EQ(fix::parse(p, o, '|'), fix::ParseError::None);
}

TEST("FIX parse: framing errors are detected") {
    const std::string good = fix::build_new_order("ORD1", "ACC001", "7203", '1', "100", '2', "2845.5", '|');
    fix::NewOrder o;
    auto mutate = [&](std::size_t pos, char c) { std::string s = good; s[pos] = c; return s; };
    // flip one body byte: checksum must fail
    CHECK_EQ(fix::parse(mutate(good.find("7203"), '8'), o, '|'), fix::ParseError::BadChecksum);
    // wrong BeginString
    std::string b = good; b.replace(2, 7, "FIX.4.2");
    CHECK_EQ(fix::parse(b, o, '|'), fix::ParseError::BadBeginString);
    // body length off by one
    std::string bl = good;
    const auto p9 = bl.find("|9=") + 3;
    bl[p9 + 1] = static_cast<char>(bl[p9 + 1] + 1);
    CHECK_EQ(fix::parse(bl, o, '|'), fix::ParseError::BadBodyLength);
    // truncated
    CHECK(fix::parse(good.substr(0, good.size() - 4), o, '|') != fix::ParseError::None);
    // trailing junk
    CHECK(fix::parse(good + "x", o, '|') != fix::ParseError::None);
}

TEST("FIX parse: duplicate tag rejected") {
    // build by hand: 55 twice
    std::string body = "35=D|1=ACC001|11=X|55=7203|55=6758|54=1|38=100|40=2|44=2845.5|";
    std::string m = "8=FIX.4.4|9=" + std::to_string(body.size()) + "|" + body;
    unsigned s = 0;
    for (char c : m) s += (c == '|') ? 1u : static_cast<unsigned char>(c);
    char cs[8];
    std::snprintf(cs, sizeof cs, "%03u", s & 0xFF);
    m += std::string("10=") + cs + "|";
    fix::NewOrder o;
    CHECK_EQ(fix::parse(m, o, '|'), fix::ParseError::DuplicateTag);
}

// ------------------------------------------------------------------ risk engine
namespace {
RiskEngine make_engine(Limits lim = {}) {
    RiskEngine e(lim, 1024);
    Instrument toyota;
    std::copy_n("7203", 4, toyota.code);
    toyota.tick_table = TickTable::Topix500;
    toyota.base_price = *parse_price("2845.5");  // limit +/-500 -> [2345.5, 3345.5]
    e.add_instrument(toyota);
    Instrument halted;
    std::copy_n("5016", 4, halted.code);
    halted.base_price = yen(612);
    halted.halted = true;
    e.add_instrument(halted);
    Instrument alpha;  // new-style alphanumeric code
    std::copy_n("130A", 4, alpha.code);
    alpha.base_price = yen(1'234);
    e.add_instrument(alpha);
    e.add_account("ACC001", 2'000'000'000);
    e.add_account("TINY", 1'000'000);
    return e;
}
Reject run(RiskEngine& e, const char* id, const char* acct, const char* sym, char side, const char* qty,
           char type, const char* px) {
    return e.check(fix::build_new_order(id, acct, sym, side, qty, type, px, '|'), '|').reason;
}
}  // namespace

TEST("risk: accepts valid limit and market orders at the band edges") {
    auto e = make_engine();
    CHECK_EQ(run(e, "1", "ACC001", "7203", '1', "100", '2', "2845.5"), Reject::None);
    // Limit-up is 2,845.5 + 500 = 3,345.5, but above 3,000 yen the tick is 1 yen, so the
    // highest orderable price is 3,345. The limit price itself is off tick.
    CHECK_EQ(run(e, "2", "ACC001", "7203", '1', "100", '2', "3345"), Reject::None);
    CHECK_EQ(run(e, "2b", "ACC001", "7203", '1', "100", '2', "3345.5"), Reject::OffTick);
    CHECK_EQ(run(e, "3", "ACC001", "7203", '2', "100", '2', "2345.5"), Reject::None);  // limit-down exactly
    CHECK_EQ(run(e, "4", "ACC001", "7203", '1', "100", '1', ""), Reject::None);        // market
    CHECK_EQ(run(e, "5", "ACC001", "130A", '2', "300", '2', "1200"), Reject::None);
}

TEST("risk: each reject reason fires") {
    auto e = make_engine({.max_order_qty = 50'000, .max_order_notional_yen = 100'000'000,
                          .fat_finger_bps = 1'000, .allow_short_sell = false});
    CHECK_EQ(e.check("garbage", '|').reason, Reject::MalformedMessage);
    CHECK_EQ(run(e, "a", "ACC001", "9999", '1', "100", '2', "100"), Reject::UnknownSymbol);
    CHECK_EQ(run(e, "b", "ACC001", "5016", '1', "100", '2', "612"), Reject::SymbolHalted);
    CHECK_EQ(run(e, "c", "NOBODY", "7203", '1', "100", '2', "2845.5"), Reject::UnknownAccount);
    CHECK_EQ(run(e, "d", "ACC001", "7203", '3', "100", '2', "2845.5"), Reject::InvalidSide);
    CHECK_EQ(run(e, "e", "ACC001", "7203", '5', "100", '2', "2845.5"), Reject::ShortSellNotPermitted);
    CHECK_EQ(run(e, "f", "ACC001", "7203", '1', "100", '3', "2845.5"), Reject::InvalidOrdType);
    CHECK_EQ(run(e, "g", "ACC001", "7203", '1', "0", '2', "2845.5"), Reject::InvalidQuantity);
    CHECK_EQ(run(e, "h", "ACC001", "7203", '1', "150", '2', "2845.5"), Reject::NotTradingUnit);
    CHECK_EQ(run(e, "i", "ACC001", "7203", '1', "100", '2', "28.455"), Reject::InvalidPrice);
    CHECK_EQ(run(e, "j", "ACC001", "7203", '1', "100", '2', "3346"), Reject::OutsideDailyLimit);
    CHECK_EQ(run(e, "k", "ACC001", "7203", '1', "100", '2', "2345"), Reject::OutsideDailyLimit);
    CHECK_EQ(run(e, "l", "ACC001", "7203", '1', "100", '2', "2845.1"), Reject::OffTick);
    CHECK_EQ(run(e, "m", "ACC001", "7203", '1', "60000", '2', "2845.5"), Reject::ExceedsMaxQty);
    CHECK_EQ(run(e, "n", "ACC001", "7203", '1', "40000", '2', "2845.5"), Reject::ExceedsMaxNotional);
    CHECK_EQ(run(e, "o", "ACC001", "7203", '1', "100", '2', "3200"), Reject::FatFinger);  // +12.5% vs 10% collar
    CHECK_EQ(run(e, "p", "TINY", "7203", '1', "400", '2', "2845.5"), Reject::ExceedsAccountCredit);
    CHECK_EQ(run(e, "q", "ACC001", "7203", '1', "100", '1', "2845.5"), Reject::InvalidPrice);  // price on market
    CHECK_EQ(run(e, "r", "ACC001", "7203", '1', "100", '2', ""), Reject::MissingField);
}

TEST("risk: duplicate ClOrdID per account; rejected orders do not burn the ID") {
    auto e = make_engine();
    CHECK_EQ(run(e, "X1", "ACC001", "7203", '1', "100", '2', "9999"), Reject::OutsideDailyLimit);
    CHECK_EQ(run(e, "X1", "ACC001", "7203", '1', "100", '2', "2845.5"), Reject::None);
    CHECK_EQ(run(e, "X1", "ACC001", "7203", '1', "100", '2', "2845.5"), Reject::DuplicateClOrdId);
    CHECK_EQ(run(e, "X1", "TINY", "7203", '1', "100", '2', "2845.5"), Reject::None);  // other account
}

TEST("risk: credit usage accumulates and market orders use the limit-up price") {
    auto e = make_engine();
    // TINY has 1,000,000 yen. A market buy of 300 is valued at 3,345.5 * 300 = 1,003,650 > limit.
    CHECK_EQ(run(e, "m1", "TINY", "7203", '1', "300", '1', ""), Reject::ExceedsAccountCredit);
    CHECK_EQ(run(e, "m2", "TINY", "7203", '1', "200", '2', "2845.5"), Reject::None);   // 569,100
    CHECK_EQ(run(e, "m3", "TINY", "7203", '1', "200", '2', "2845.5"), Reject::ExceedsAccountCredit);
    CHECK_EQ(e.account("TINY")->used_yen, 569'100);
}

int main() { return t::run_all(); }
