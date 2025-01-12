// jpx_check: run a file of FIX NewOrderSingle messages through the pre-trade checks.
//
//   jpx_check --instruments data/instruments.csv --accounts data/accounts.csv orders.fix
//
// One message per line. Messages may use SOH or '|' as the field delimiter (detected
// per line). Prints one line per order: line number, ClOrdID, decision.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "jpx/refdata.hpp"
#include "jpx/risk.hpp"

int main(int argc, char** argv) {
    std::string inst = "data/instruments.csv", accts = "data/accounts.csv", orders;
    jpx::Limits lim;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--instruments" && i + 1 < argc) inst = argv[++i];
        else if (a == "--accounts" && i + 1 < argc) accts = argv[++i];
        else if (a == "--fat-finger-bps" && i + 1 < argc) lim.fat_finger_bps = std::stoi(argv[++i]);
        else if (a == "--allow-short") lim.allow_short_sell = true;
        else orders = a;
    }
    if (orders.empty()) {
        std::fprintf(stderr, "usage: jpx_check [--instruments f] [--accounts f] [--fat-finger-bps n] orders.fix\n");
        return 2;
    }
    jpx::RiskEngine eng(lim);
    jpx::load_instruments(eng, inst);
    jpx::load_accounts(eng, accts);

    std::ifstream in(orders);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", orders.c_str()); return 2; }
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        ++n;
        const char delim = line.find(jpx::fix::SOH) != std::string::npos ? jpx::fix::SOH : '|';
        jpx::fix::NewOrder o;
        const auto pe = jpx::fix::parse(line, o, delim);
        const auto d = (pe == jpx::fix::ParseError::None) ? eng.check(o) : eng.check(line, delim);
        std::printf("%d,%.*s,%s%s%s\n", n, static_cast<int>(o.cl_ord_id.size()), o.cl_ord_id.data(),
                    jpx::to_string(d.reason), pe == jpx::fix::ParseError::None ? "" : ":",
                    pe == jpx::fix::ParseError::None ? "" : jpx::fix::to_string(pe));
    }
    std::fprintf(stderr, "\n%d orders\n", n);
    for (int r = 0; r < jpx::kNumRejects; ++r)
        if (eng.counts()[r]) std::fprintf(stderr, "  %-26s %llu\n", jpx::to_string(static_cast<jpx::Reject>(r)),
                                          static_cast<unsigned long long>(eng.counts()[r]));
    return 0;
}
