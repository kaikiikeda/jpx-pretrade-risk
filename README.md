# JPX Pre-Trade Risk Checker (C++20)

A pre-trade risk gate for Tokyo Stock Exchange cash-equity orders. It parses FIX 4.4 `NewOrderSingle` messages and accepts or rejects each one against TSE's daily price limits, tick-size tables and trading units, plus the firm-level limits a broker applies before an order reaches the exchange: maximum size and notional, a fat-finger collar, per-account credit, and duplicate order IDs.

The rules come straight from JPX's current published tables. Correctness is checked against an independent oracle, and the per-order latency is measured.

| | Result |
|---|---|
| Replay vs independent oracle | **300,000 orders, 0 false rejects, 0 false accepts, 0 wrong reject reasons** (also with fat-finger collar on) |
| Mutation testing | 5 of 5 plantable rule bugs caught by the replay, with hundreds to thousands of mismatches each |
| Latency, parse + all checks | **p50 ≈ 0.45 µs, p99 ≈ 1.0 µs** per order (shared 2-vCPU cloud VM; see below) |
| Throughput | ≈ 1.8 million orders/s on one core |
| Dependencies | none (C++20 standard library; Python 3 only for the test oracle) |

## Background

I first learned how pre-trade risk controls work during an internship in March 2024, where I was introduced to the checks a broker runs before an order reaches the exchange. This repository is my own independent rebuild of that concept, written from scratch in 2025 using only JPX's public rules and tables. It contains no code, data or proprietary logic from the internship. The goal was to understand the problem properly by implementing it end to end, then to test it rigorously (an independent oracle, mutation testing, and a latency benchmark).

## What it checks

Each order is checked in a fixed order and rejected for the **first** rule it breaks:

| Group | Check | Reject code |
|---|---|---|
| Message | FIX framing: BeginString, BodyLength, CheckSum, field order, duplicate tags | `MALFORMED_MESSAGE` |
| | MsgType must be `D`; required tags present | `UNSUPPORTED_MSG_TYPE`, `MISSING_FIELD` |
| Reference data | symbol known, not halted; account known | `UNKNOWN_SYMBOL`, `SYMBOL_HALTED`, `UNKNOWN_ACCOUNT` |
| Order fields | side, order type, quantity, short-sell permission | `INVALID_SIDE`, `INVALID_ORD_TYPE`, `INVALID_QUANTITY`, `SHORT_SELL_NOT_PERMITTED` |
| **Exchange rules** | quantity is a multiple of the trading unit (100 shares) | `NOT_TRADING_UNIT` |
| | price is inside the day's limit band, including one-sided 4× expansion | `OUTSIDE_DAILY_LIMIT` |
| | price is on the tick grid for its own price band (TOPIX500 table vs other issues) | `OFF_TICK` |
| | price is a legal number: no sub-0.1-yen prices, no junk | `INVALID_PRICE` |
| **Firm limits** | max order quantity, max order notional | `EXCEEDS_MAX_QTY`, `EXCEEDS_MAX_NOTIONAL` |
| | fat-finger collar around the base price (optional, in basis points) | `FAT_FINGER` |
| | per-account gross-notional credit; market orders valued at the limit-up price | `EXCEEDS_ACCOUNT_CREDIT` |
| | duplicate `ClOrdID` per account (checked last, so a rejected order does not burn its ID) | `DUPLICATE_CLORDID` |

### The JPX rules, and an edge case they create

JPX defines two tables ([tick sizes](https://www.jpx.co.jp/english/equities/trading/domestic/07.html), [daily price limits](https://www.jpx.co.jp/english/equities/trading/domestic/06.html)), and the boundaries are easy to get wrong:

- **Tick size depends on the order price**, with inclusive upper bounds. For a TOPIX500 stock, 1,000.0 yen is on a 0.1-yen tick but 1,000.1 is not: above 1,000 the tick is 0.5.
- **The daily limit depends on the base price** (previous close), with strict lower-than bounds. A base of exactly 1,000 yen gets a ±300 limit, not ±150.
- **Limit expansion is one-sided.** When a stock stays locked at the limit on two consecutive days, JPX broadens that side only, to 4× the normal width: TriIs (code 4840), base 745, would get an upper limit of 1,345 while the lower limit stays at 595. The unit tests reproduce that case.

Combining the two tables creates a case that naive checkers miss. **The limit price itself can be off tick.** Toyota at a base of 2,845.5 has a ±500 limit, so limit-up is 3,345.5. Above 3,000 yen the tick is 1 yen, so 3,345.5 is not a legal price, and the highest orderable price is 3,345. A checker that only tests "price ≤ limit-up" would pass an order the exchange rejects.

Prices are stored as 64-bit integers in tenths of a yen. JPX's smallest tick is 0.1 yen, so every legal price is an exact integer and tick checks are an integer modulo, with no floating-point tolerance anywhere.

## How correctness is verified

1. **Unit tests** (`tests/test_unit.cpp`) cover every boundary of both tick tables and all 34 daily-limit bands, FIX framing errors, and each reject code.
2. **Replay against an independent oracle.** `scripts/gen_replay.py` re-implements the rules in Python with exact `Decimal` arithmetic, transcribing the tables from JPX a second time rather than sharing code. It generates adversarial orders: prices clustered on daily-limit edges, on tick-band boundaries (one tick below, exactly on, 0.1 yen above), on the off-tick limit price, plus junk prices, bad quantities, unknown symbols, duplicate IDs, and messages corrupted in transit. The replay universe includes synthetic instruments whose base prices sit exactly on daily-limit boundaries, so those cases occur thousands of times. The C++ `replay` tool must agree on every order.

   Result: 300,000 orders across all 19 decision types, **zero disagreements**, both with and without the fat-finger collar.

3. **Mutation testing**, which checks that the replay would actually catch bugs. I planted rule bugs one at a time and re-ran the replay:

   | Planted bug | False rejects | False accepts | Wrong reason |
   |---|---:|---:|---:|
   | Tick band treats prices up to 1 yen above a boundary as the lower band | 25 | 1,596 | 372 |
   | Daily-limit band uses `≤` instead of `<` on the base price | 33,467 | 394 | 8,191 |
   | Limit expansion 3× instead of 4× | 6,179 | 65 | 1,290 |
   | Upper band edge excluded (`≥` instead of `>`) | 23,217 | 251 | 6,448 |
   | Odd-lot check off (allows remainders up to 50 shares) | 52 | 4,815 | 3,042 |

   Two other mutations went undetected, and on inspection they cannot change any outcome. Making a tick boundary exclusive instead of inclusive is equivalent because every boundary price (1,000, 3,000, ...) is divisible by both neighbouring tick sizes. Rounding notional down instead of up makes no difference because 100-share lots of 0.1-yen prices are always whole yen. A test suite that kills 100% of mutations, including equivalent ones, would be suspicious.

   An earlier version of the replay caught **none** of the tick-boundary mutations: boundary prices rarely fell inside a test instrument's daily band, so they were rejected as out of range before the tick rule ran. Mutation testing exposed the gap, and adding boundary instruments fixed it.

## Latency

`bench` builds 1,000,000 realistic messages (8 symbols, ~11% rule-breaking), copies each into a receive buffer as a socket read would, then times parse + all checks per order:

```
orders: 1000000  (avg 105 bytes, 88.6% accepted)
timer overhead (steady_clock pair): p50 33 ns
latency per order, parse + all checks (ns, includes timer overhead):
  p50 446   p90 619   p99 972   p99.9 6058
  of which FIX parse:   p50 233   p99 616   p99.9 905
  of which risk checks: p50 214   p99 584   p99.9 867
throughput (untimed loop, incl. the receive copy): 1.78 million orders/s
```

That run was on a shared 2-vCPU cloud VM (Xeon 2.8 GHz, g++ 13, `-O3`, no core pinning), so treat the tails as noisy. Across runs p50 ranged from 0.45 to 0.51 µs; the p99.9 spikes come from the VM, not the code path. Design choices that keep the hot path fast:

- **No heap allocation per order.** Parsing returns `string_view`s into the receive buffer. Symbols resolve through a flat array indexed directly by the 4-character JPX code (36⁴ slots, which handles new alphanumeric codes like `130A`). ClOrdIDs go into a pre-sized open-addressing table.
- **Single-pass FIX parser.** The checksum and the field split happen in the same byte loop, so each byte is touched once. Replacing a `memchr`-based splitter with this loop cut parse time by about 30%.
- **Compact duplicate-ID table.** 8-byte hash slots, with full keys in an append-only arena that is only read on a hash match. Shrinking slots from 56 to 8 bytes cut risk-check p50 from about 300 to about 200 ns, because most of that cost is the one random memory access into a table sized for a million orders.

## Usage

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build                       # unit tests
./scripts/run_replay.sh 300000               # oracle replay (needs Python 3)
./build/bench 1000000                        # latency benchmark
./build/jpx_check data/sample_orders.fix     # check a file of orders
```

`data/sample_orders.fix` contains annotated examples, including the off-tick limit-up price, the one-sided expansion, a halted symbol, an alphanumeric code and a corrupted checksum. Selected output, with notes added on the right:

```
1,T-0001,ACCEPT
3,T-0003,OFF_TICK                  limit-up 3,345.5 is not on the 1-yen grid
4,T-0004,ACCEPT                    3,345 is the highest orderable price
8,T-0008,ACCEPT                    upper limit broadened 4x for code 4840
9,T-0009,OUTSIDE_DAILY_LIMIT       ...but the lower side was not
13,T-0001,DUPLICATE_CLORDID
16,T-0015,MALFORMED_MESSAGE:BAD_CHECKSUM
```

Reference data lives in CSV: `instruments.csv` holds code, tick table, base price, trading unit, expansion flags and halt flag; `accounts.csv` holds credit limits. The sample uses real JPX codes with illustrative base prices.

## Layout

```
include/jpx/
  price.hpp      fixed-point prices (0.1 yen), exact parser
  tables.hpp     JPX tick-size and daily-limit tables, price bands, expansion
  fix.hpp        zero-allocation FIX 4.4 NewOrderSingle parser
  risk.hpp       risk engine, reject codes, limits, reference data
  refdata.hpp    CSV loaders
src/             fix.cpp, risk.cpp, refdata.cpp
apps/            jpx_check (CLI), replay (oracle comparison), bench (latency)
tests/           unit tests (no framework dependency)
scripts/         gen_replay.py (independent oracle), run_replay.sh
data/            sample instruments, accounts, orders; replay universe
```

## Limitations and next steps

- **Scope.** NewOrderSingle only. No cancel/replace, so credit usage is only ever added (never released on fills or cancels), and there is no FIX session layer (logon, sequence numbers, resend).
- **Reference data is static per day.** A production gate would take base prices, halts and limit expansions from the exchange's start-of-day files and update them intraday.
- **Rules not modelled.** Order-rate throttles, special quotes, short-sale price restrictions (uptick rule) and per-symbol position limits.
- **Benchmark environment.** Pinned cores, isolated CPUs and huge pages would give cleaner tails than a shared VM.

*Kaiki Ikeda · 2025 · MIT License. Not affiliated with Japan Exchange Group; table values are transcribed from JPX's public website for educational use.*
