#include "jpx/refdata.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace jpx {

namespace {

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) {
        while (!cell.empty() && (cell.back() == '\r' || cell.back() == ' ')) cell.pop_back();
        out.push_back(cell);
    }
    return out;
}

bool flag(const std::string& s) { return s == "1" || s == "true" || s == "Y"; }

}  // namespace

int load_instruments(RiskEngine& eng, const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    std::getline(in, line);  // header
    int n = 0, lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#') continue;
        const auto c = split(line);
        if (c.size() < 7 || c[0].size() != 4) throw std::runtime_error(path + ":" + std::to_string(lineno) + ": bad row");
        Instrument inst;
        for (int i = 0; i < 4; ++i) inst.code[i] = c[0][i];
        if (c[1] == "TOPIX500") inst.tick_table = TickTable::Topix500;
        else if (c[1] == "OTHER") inst.tick_table = TickTable::Other;
        else throw std::runtime_error(path + ":" + std::to_string(lineno) + ": unknown tick table " + c[1]);
        const auto p = parse_price(c[2]);
        if (!p) throw std::runtime_error(path + ":" + std::to_string(lineno) + ": bad base price");
        inst.base_price = *p;
        inst.trading_unit = std::stoll(c[3]);
        inst.expand_upper = flag(c[4]);
        inst.expand_lower = flag(c[5]);
        inst.halted = flag(c[6]);
        eng.add_instrument(inst);
        ++n;
    }
    return n;
}

int load_accounts(RiskEngine& eng, const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    std::getline(in, line);
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto c = split(line);
        if (c.size() < 2) throw std::runtime_error(path + ": bad account row");
        eng.add_account(c[0], std::stoll(c[1]));
        ++n;
    }
    return n;
}

}  // namespace jpx
