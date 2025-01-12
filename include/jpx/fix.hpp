// Minimal, zero-allocation FIX 4.4 tag=value parser for NewOrderSingle (35=D).
//
// The parser validates framing the way an exchange gateway would before any risk check
// runs: BeginString first, BodyLength second, MsgType third, CheckSum last, BodyLength
// and CheckSum both correct. Field values are returned as string_views into the
// caller's buffer, so the buffer must outlive the parsed message.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "jpx/price.hpp"

namespace jpx::fix {

inline constexpr char SOH = '\x01';

enum class ParseError : std::uint8_t {
    None = 0,
    Truncated,
    BadBeginString,
    BadBodyLength,
    BadMsgType,
    BadChecksum,
    MalformedField,
    DuplicateTag,
};

const char* to_string(ParseError e);

enum class Side : std::uint8_t { None = 0, Buy = '1', Sell = '2', SellShort = '5' };
enum class OrdType : std::uint8_t { None = 0, Market = '1', Limit = '2' };

// Fields of a NewOrderSingle that the risk engine needs. `has_*` flags record presence;
// value validity is the risk engine's job, so a present-but-garbage price is kept as
// raw text and reported as an invalid price rather than a parse failure.
struct NewOrder {
    std::string_view msg_type;  // 35
    std::string_view cl_ord_id; // 11
    std::string_view account;   // 1
    std::string_view symbol;    // 55
    std::string_view side_raw;  // 54
    std::string_view qty_raw;   // 38
    std::string_view type_raw;  // 40
    std::string_view price_raw; // 44
    std::string_view tif_raw;   // 59
};

// Parse one complete message. `delim` is SOH on the wire; '|' is accepted for
// human-readable test files.
ParseError parse(std::string_view msg, NewOrder& out, char delim = SOH);

// Build a valid NewOrderSingle with correct BodyLength and CheckSum (for tests,
// the replay generator and the benchmark).
std::string build_new_order(std::string_view cl_ord_id, std::string_view account,
                            std::string_view symbol, char side, std::string_view qty,
                            char ord_type, std::string_view price, char delim = SOH);

}  // namespace jpx::fix
