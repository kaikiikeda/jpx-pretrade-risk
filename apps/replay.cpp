// replay: check a labeled order file against an independent oracle's expected decisions.
//
// Input lines: <EXPECTED_REASON>\t<FIX message with '|' delimiters>
// The expected reasons are produced by scripts/gen_replay.py, a separate implementation
// of the JPX rules written in Python with exact decimal arithmetic. Any disagreement is
// printed, and the exit code is non-zero if there are false rejects or false accepts.
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

#include "jpx/refdata.hpp"
#include "jpx/risk.hpp"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: replay instruments.csv accounts.csv labeled.tsv [--fat-finger-bps n]\n");
        return 2;
    }
    jpx::Limits lim;
    for (int i = 4; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--fat-finger-bps") lim.fat_finger_bps = std::stoi(argv[i + 1]);
    jpx::RiskEngine eng(lim);
    jpx::load_instruments(eng, argv[1]);
    jpx::load_accounts(eng, argv[2]);

    std::ifstream in(argv[3]);
    std::string line;
    long n = 0, false_rejects = 0, false_accepts = 0, wrong_reason = 0;
    std::map<std::string, long> expected_counts;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string expected = line.substr(0, tab);
        const std::string msg = line.substr(tab + 1);
        const auto d = eng.check(msg, '|');
        const std::string got = jpx::to_string(d.reason);
        ++n;
        ++expected_counts[expected];
        if (got == expected) continue;
        if (expected == "ACCEPT") ++false_rejects;
        else if (got == "ACCEPT") ++false_accepts;
        else ++wrong_reason;
        if (false_rejects + false_accepts + wrong_reason <= 20)
            std::printf("MISMATCH line %ld: expected %s got %s\n  %s\n", n, expected.c_str(), got.c_str(), msg.c_str());
    }
    std::printf("replayed %ld orders\n", n);
    for (const auto& [k, v] : expected_counts) std::printf("  %-26s %ld\n", k.c_str(), v);
    std::printf("false rejects: %ld   false accepts: %ld   wrong reject reason: %ld\n", false_rejects,
                false_accepts, wrong_reason);
    return (false_rejects || false_accepts || wrong_reason) ? 1 : 0;
}
