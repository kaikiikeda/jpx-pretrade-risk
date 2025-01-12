#include "jpx/fix.hpp"

#include <cstring>

namespace jpx::fix {

const char* to_string(ParseError e) {
    switch (e) {
        case ParseError::None: return "OK";
        case ParseError::Truncated: return "TRUNCATED";
        case ParseError::BadBeginString: return "BAD_BEGIN_STRING";
        case ParseError::BadBodyLength: return "BAD_BODY_LENGTH";
        case ParseError::BadMsgType: return "BAD_MSG_TYPE";
        case ParseError::BadChecksum: return "BAD_CHECKSUM";
        case ParseError::MalformedField: return "MALFORMED_FIELD";
        case ParseError::DuplicateTag: return "DUPLICATE_TAG";
    }
    return "?";
}

namespace {

bool parse_uint(std::string_view s, std::uint32_t& out) {
    if (s.empty() || s.size() > 9) return false;
    std::uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace

// Single pass over the bytes: the running checksum and the field split happen in the
// same loop, so every byte is touched exactly once and there are no library calls.
ParseError parse(std::string_view m, NewOrder& out, char delim) {
    out = NewOrder{};
    const auto* u = reinterpret_cast<const unsigned char*>(m.data());
    const std::size_t n = m.size();
    const unsigned char d = static_cast<unsigned char>(delim);

    unsigned sum = 0;            // checksum of all bytes before the current field
    unsigned field_sum = 0;      // value of `sum` at the start of the current field
    std::size_t fstart = 0;      // index of the current field's first byte
    std::size_t vstart = 0;      // index of the current value's first byte
    std::size_t body_start = 0;
    std::uint32_t body_len = 0;
    int field = 0;               // ordinal of the current field
    int tag = 0;
    bool in_tag = true;
    bool done = false;
    std::uint32_t seen = 0;

    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = u[i];
        if (done) return ParseError::MalformedField;  // bytes after the trailer
        if (in_tag) {
            if (c == '=') {
                if (i == fstart || i - fstart > 6) return ParseError::MalformedField;
                in_tag = false;
                vstart = i + 1;
            } else if (c >= '0' && c <= '9') {
                tag = tag * 10 + (c - '0');
            } else {
                return field == 0 ? ParseError::BadBeginString : ParseError::MalformedField;
            }
        } else if (c == d) {
            if (i == vstart) return ParseError::MalformedField;  // empty value
            const std::string_view v(m.data() + vstart, i - vstart);
            switch (field) {
                case 0:
                    if (tag != 8 || v != "FIX.4.4") return ParseError::BadBeginString;
                    break;
                case 1:
                    if (tag != 9 || !parse_uint(v, body_len)) return ParseError::BadBodyLength;
                    body_start = i + 1;
                    break;
                case 2:
                    if (tag == 10) return ParseError::BadBodyLength;
                    if (tag != 35) return ParseError::BadMsgType;
                    out.msg_type = v;
                    break;
                default: {
                    auto take = [&](std::string_view& slot, std::uint32_t bit) {
                        if (seen & bit) return false;
                        seen |= bit;
                        slot = v;
                        return true;
                    };
                    bool ok = true;
                    switch (tag) {
                        case 1: ok = take(out.account, 1u << 0); break;
                        case 11: ok = take(out.cl_ord_id, 1u << 1); break;
                        case 38: ok = take(out.qty_raw, 1u << 2); break;
                        case 40: ok = take(out.type_raw, 1u << 3); break;
                        case 44: ok = take(out.price_raw, 1u << 4); break;
                        case 54: ok = take(out.side_raw, 1u << 5); break;
                        case 55: ok = take(out.symbol, 1u << 6); break;
                        case 59: ok = take(out.tif_raw, 1u << 7); break;
                        case 8: case 9: case 35: return ParseError::DuplicateTag;
                        case 10: {
                            // Trailer: must start exactly BodyLength bytes after the body
                            // begins and carry the 3-digit checksum of everything before it.
                            if (fstart != body_start + body_len) return ParseError::BadBodyLength;
                            std::uint32_t declared = 0;
                            if (v.size() != 3 || !parse_uint(v, declared)) return ParseError::BadChecksum;
                            if (declared != (field_sum & 0xFF)) return ParseError::BadChecksum;
                            done = true;
                            break;
                        }
                        default: break;  // other fields (49, 56, 34, 52, 60, ...) are ignored
                    }
                    if (!ok) return ParseError::DuplicateTag;
                }
            }
            ++field;
            in_tag = true;
            tag = 0;
            fstart = i + 1;
            sum += 1;  // the delimiter counts as SOH
            field_sum = sum;
            continue;
        }
        sum += c;
    }
    if (!done) {
        if (field == 0) return ParseError::Truncated;
        if (field >= 2 && body_start + body_len + 7 > n) return ParseError::BadBodyLength;
        return ParseError::Truncated;
    }
    return ParseError::None;
}

std::string build_new_order(std::string_view cl_ord_id, std::string_view account, std::string_view symbol,
                            char side, std::string_view qty, char ord_type, std::string_view price, char delim) {
    std::string body;
    body.reserve(128);
    auto add = [&](std::string_view tag, std::string_view val) {
        body.append(tag);
        body.push_back('=');
        body.append(val);
        body.push_back(delim);
    };
    add("35", "D");
    add("49", "CLIENT");
    add("56", "JPXGW");
    add("1", account);
    add("11", cl_ord_id);
    add("55", symbol);
    add("54", std::string_view(&side, 1));
    add("38", qty);
    add("40", std::string_view(&ord_type, 1));
    if (!price.empty()) add("44", price);
    add("59", "0");

    std::string msg = "8=FIX.4.4";
    msg.push_back(delim);
    msg += "9=" + std::to_string(body.size());
    msg.push_back(delim);
    msg += body;
    unsigned sum = 0;
    for (char c : msg) sum += (c == delim) ? 1u : static_cast<unsigned char>(c);
    char cs[4];
    const unsigned s = sum & 0xFF;
    cs[0] = static_cast<char>('0' + s / 100);
    cs[1] = static_cast<char>('0' + (s / 10) % 10);
    cs[2] = static_cast<char>('0' + s % 10);
    cs[3] = 0;
    msg += "10=";
    msg += cs;
    msg.push_back(delim);
    return msg;
}

}  // namespace jpx::fix
