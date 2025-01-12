// Loading instrument and account reference data from CSV.
#pragma once

#include <string>

#include "jpx/risk.hpp"

namespace jpx {

// instruments.csv: code,tick_table(TOPIX500|OTHER),base_price,trading_unit,expand_upper,expand_lower,halted
// Returns the number of instruments loaded; throws std::runtime_error on bad rows.
int load_instruments(RiskEngine& eng, const std::string& path);

// accounts.csv: account,credit_limit_yen
int load_accounts(RiskEngine& eng, const std::string& path);

}  // namespace jpx
