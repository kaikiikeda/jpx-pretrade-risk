// bench: per-order latency of parse + full risk check, and throughput.
//
// Builds N realistic NewOrderSingle messages up front (wire format, SOH delimiters,
// ~10% that break some rule), then times each check individually with steady_clock.
// Reports p50 / p90 / p99 / p99.9 / max, the timer's own overhead, and messages/second
// for an untimed pass.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "jpx/refdata.hpp"
#include "jpx/risk.hpp"

using clk = std::chrono::steady_clock;

static double pct(std::vector<long long>& v, double p) {
    const std::size_t k = static_cast<std::size_t>(p / 100.0 * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return static_cast<double>(v[k]);
}

int main(int argc, char** argv) {
    const std::size_t N = argc > 1 ? std::stoul(argv[1]) : 1'000'000;
    const std::string inst = argc > 2 ? argv[2] : "data/instruments.csv";

    jpx::Limits lim;
    lim.fat_finger_bps = 2'000;
    jpx::RiskEngine eng(lim, N * 2 + 16);
    jpx::load_instruments(eng, inst);
    eng.add_account("ACC001", INT64_MAX / 4);

    struct Sym { const char* code; double base; bool topix; };
    const Sym syms[] = {{"7203", 2845.5, true}, {"6758", 3512, true}, {"9984", 9870, true},
                        {"8306", 1789.5, true}, {"6861", 61230, true}, {"2121", 4120, false},
                        {"130A", 1234, false}, {"3350", 48, false}};
    std::mt19937_64 rng(42);
    std::vector<std::string> msgs;
    msgs.reserve(N);
    char px[32], qty[16], id[24];
    for (std::size_t i = 0; i < N; ++i) {
        const Sym& s = syms[rng() % 8];
        const auto* in = eng.instrument(s.code);
        const long long tick = jpx::tick_size(in->tick_table, in->base_price);
        // a random on-tick price within +/-5% of base
        long long p = in->base_price + static_cast<long long>((static_cast<double>(rng() % 1001) - 500) / 10000.0 * in->base_price);
        p -= p % tick;
        if (p <= 0) p = tick;
        const unsigned roll = rng() % 100;
        if (roll < 3) p += 1;                                   // off tick (mostly)
        else if (roll < 6) p = in->band.upper + 10 * tick;      // outside limit
        std::snprintf(px, sizeof px, "%s", jpx::format_price(p).c_str());
        std::snprintf(qty, sizeof qty, "%llu", (roll >= 6 && roll < 8) ? 150ull : 100ull * (1 + rng() % 20));
        std::snprintf(id, sizeof id, "C%zu", i);
        const char side = (rng() & 1) ? '1' : '2';
        msgs.push_back(jpx::fix::build_new_order(id, "ACC001", s.code, side, qty, '2', px));
    }

    // timer overhead
    std::vector<long long> ovh(100'000);
    for (auto& o : ovh) {
        const auto a = clk::now();
        const auto b = clk::now();
        o = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
    }

    // warm-up pass on a fresh engine state, then timed pass
    for (std::size_t i = 0; i < std::min<std::size_t>(N, 50'000); ++i) eng.check(msgs[i]);
    eng.reset_day();

    // Each message is first copied into one receive buffer (outside the timer), the way
    // a gateway reads from a socket: the bytes being checked are already in cache.
    std::vector<long long> lat(N), lat_parse(N), lat_check(N);
    char rx[512];
    auto recv = [&](std::size_t i) {
        std::memcpy(rx, msgs[i].data(), msgs[i].size());
        return std::string_view(rx, msgs[i].size());
    };
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const auto view = recv(i);
        const auto a = clk::now();
        const auto d = eng.check(view);
        const auto b = clk::now();
        lat[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        accepted += d.accepted();
    }

    // split: parse alone, then risk checks alone on the parsed order
    eng.reset_day();
    jpx::fix::NewOrder o;
    for (std::size_t i = 0; i < N; ++i) {
        const auto view = recv(i);
        const auto a = clk::now();
        const auto pe = jpx::fix::parse(view, o);
        const auto b = clk::now();
        const auto d = eng.check(o);
        const auto c = clk::now();
        lat_parse[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        lat_check[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(c - b).count();
        if (pe != jpx::fix::ParseError::None || d.reason == jpx::Reject::MalformedMessage) return 3;
    }

    // throughput (untimed per message)
    eng.reset_day();
    const auto t0 = clk::now();
    std::size_t acc2 = 0;
    for (std::size_t i = 0; i < N; ++i) acc2 += eng.check(recv(i)).accepted();
    const double secs = std::chrono::duration<double>(clk::now() - t0).count();

    const double avg_len = [&] { double s = 0; for (auto& m : msgs) s += m.size(); return s / msgs.size(); }();
    std::printf("orders: %zu  (avg %.0f bytes, %.1f%% accepted)\n", N, avg_len, 100.0 * accepted / N);
    std::printf("timer overhead (steady_clock pair): p50 %.0f ns\n", pct(ovh, 50));
    std::printf("latency per order, parse + all checks (ns, includes timer overhead):\n");
    std::printf("  p50 %.0f   p90 %.0f   p99 %.0f   p99.9 %.0f   max %.0f\n", pct(lat, 50), pct(lat, 90),
                pct(lat, 99), pct(lat, 99.9), static_cast<double>(*std::max_element(lat.begin(), lat.end())));
    std::printf("  of which FIX parse:   p50 %.0f   p99 %.0f   p99.9 %.0f\n", pct(lat_parse, 50), pct(lat_parse, 99), pct(lat_parse, 99.9));
    std::printf("  of which risk checks: p50 %.0f   p99 %.0f   p99.9 %.0f\n", pct(lat_check, 50), pct(lat_check, 99), pct(lat_check, 99.9));
    std::printf("throughput (untimed loop, incl. the receive copy): %.2f million orders/s  (%zu accepted)\n", N / secs / 1e6, acc2);
    return 0;
}
