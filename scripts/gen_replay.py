"""Generate a labeled replay file with an INDEPENDENT oracle for the JPX rules.

The C++ engine and this script implement the same rules separately: different language,
exact Decimal arithmetic instead of fixed-point integers, and table lookups written from
the JPX pages again rather than shared with the C++ code. The replay tool then checks that
the engine agrees with the oracle on every order: zero false rejects, zero false accepts,
and the same reject reason.

Orders are generated adversarially. Prices cluster on daily-limit edges, tick-band
boundaries (999.9 / 1,000 / 1,000.1 / 1,000.5 ...), the off-tick limit price, and random
junk, so the boundary cases appear thousands of times instead of by luck.

Usage:  python scripts/gen_replay.py data/instruments.csv data/accounts.csv N out.tsv [fat_finger_bps]
"""
from __future__ import annotations

import csv
import random
import sys
from decimal import Decimal as D

# --- JPX tables (independently transcribed) -----------------------------------------
TOPIX500 = [(1_000, "0.1"), (3_000, "0.5"), (10_000, "1"), (30_000, "5"), (100_000, "10"),
            (300_000, "50"), (1_000_000, "100"), (3_000_000, "500"), (10_000_000, "1000"),
            (30_000_000, "5000"), (None, "10000")]
OTHER = [(3_000, "1"), (5_000, "5"), (30_000, "10"), (50_000, "50"), (300_000, "100"),
         (500_000, "500"), (3_000_000, "1000"), (5_000_000, "5000"), (30_000_000, "10000"),
         (50_000_000, "50000"), (None, "100000")]
LIMITS = [(100, 30), (200, 50), (500, 80), (700, 100), (1_000, 150), (1_500, 300), (2_000, 400),
          (3_000, 500), (5_000, 700), (7_000, 1_000), (10_000, 1_500), (15_000, 3_000),
          (20_000, 4_000), (30_000, 5_000), (50_000, 7_000), (70_000, 10_000), (100_000, 15_000),
          (150_000, 30_000), (200_000, 40_000), (300_000, 50_000), (500_000, 70_000),
          (700_000, 100_000), (1_000_000, 150_000), (1_500_000, 300_000), (2_000_000, 400_000),
          (3_000_000, 500_000), (5_000_000, 700_000), (7_000_000, 1_000_000), (10_000_000, 1_500_000),
          (15_000_000, 3_000_000), (20_000_000, 4_000_000), (30_000_000, 5_000_000),
          (50_000_000, 7_000_000), (None, 10_000_000)]
MAX_QTY = 1_000_000
MAX_NOTIONAL = 500_000_000


def tick(table, px: D) -> D:
    for upto, t in table:
        if upto is None or px <= upto:
            return D(t)


def limit(base: D) -> D:
    for below, w in LIMITS:
        if below is None or base < below:
            return D(w)


def band(inst):
    w = limit(inst["base"])
    up = inst["base"] + (4 * w if inst["exp_up"] else w)
    lo = inst["base"] - (4 * w if inst["exp_lo"] else w)
    return max(lo, D("0.1")), up


def parse_price(s: str):
    """Exact JPX price or None: digits, optional '.d' (extra trailing zeros allowed)."""
    if not s or len(s) > 18:
        return None
    whole, dot, frac = s.partition(".")
    if not whole.isdigit() or (dot and (not frac or not frac.isdigit())):
        return None
    if frac and frac[1:].strip("0"):
        return None
    return D(s)


def checksum_msg(fields, delim="|"):
    body = "".join(f"{k}={v}{delim}" for k, v in fields)
    head = f"8=FIX.4.4{delim}9={len(body)}{delim}"
    msg = head + body
    cs = sum(1 if c == delim else ord(c) for c in msg) % 256
    return msg + f"10={cs:03d}{delim}"


class Oracle:
    def __init__(self, insts, accts, ff_bps):
        self.insts, self.accts, self.ff = insts, accts, ff_bps
        self.used = {a: 0 for a in accts}
        self.ids = set()

    def decide(self, o):
        if o.get("malformed"):
            return "MALFORMED_MESSAGE"
        if o["35"] != "D":
            return "UNSUPPORTED_MSG_TYPE"
        for k in ("11", "1", "55", "54", "38", "40"):
            if not o.get(k):
                return "MISSING_FIELD"
        inst = self.insts.get(o["55"])
        if inst is None:
            return "UNKNOWN_SYMBOL"
        if inst["halted"]:
            return "SYMBOL_HALTED"
        if o["1"] not in self.accts:
            return "UNKNOWN_ACCOUNT"
        if o["54"] not in ("1", "2", "5"):
            return "INVALID_SIDE"
        if o["54"] == "5":
            return "SHORT_SELL_NOT_PERMITTED"
        if o["40"] not in ("1", "2"):
            return "INVALID_ORD_TYPE"
        q = o["38"]
        if not q.isdigit() or len(q) > 12 or int(q) <= 0:
            return "INVALID_QUANTITY"
        qty = int(q)
        if qty % inst["unit"]:
            return "NOT_TRADING_UNIT"
        lo, up = band(inst)
        if o["40"] == "2":
            if not o.get("44"):
                return "MISSING_FIELD"
            px = parse_price(o["44"])
            if px is None or px <= 0:
                return "INVALID_PRICE"
            if px < lo or px > up:
                return "OUTSIDE_DAILY_LIMIT"
            if px % tick(inst["table"], px) != 0:
                return "OFF_TICK"
        else:
            if o.get("44"):
                return "INVALID_PRICE"
            px = up
        if qty > MAX_QTY:
            return "EXCEEDS_MAX_QTY"
        notional = int((qty * px).to_integral_value(rounding="ROUND_CEILING"))
        if notional > MAX_NOTIONAL:
            return "EXCEEDS_MAX_NOTIONAL"
        if o["40"] == "2" and self.ff and abs(px - inst["base"]) * 10_000 > self.ff * inst["base"]:
            return "FAT_FINGER"
        if self.used[o["1"]] + notional > self.accts[o["1"]]:
            return "EXCEEDS_ACCOUNT_CREDIT"
        key = (o["1"], o["11"])
        if key in self.ids:
            return "DUPLICATE_CLORDID"
        self.ids.add(key)
        self.used[o["1"]] += notional
        return "ACCEPT"


def tick_boundary_price(inst, lo, up, rng):
    """A price at, just below or just above a tick-table boundary inside today's band."""
    t = inst["table"]
    edges = [D(u) for u, _ in t if u is not None and lo <= u <= up]
    if not edges:
        return lo
    b = rng.choice(edges)
    below, above = tick(t, b), tick(t, b + D("0.1"))
    return rng.choice([b, b - below, b + D("0.1"), b + D("0.5"), b + above, b + above - D("0.1"), b + below])


def candidate_prices(inst, rng):
    lo, up = band(inst)
    base, t = inst["base"], inst["table"]
    def on_tick_near(x):
        k = tick(t, x)
        return (x // k) * k
    picks = [
        lambda: on_tick_near(lo + (up - lo) * D(rng.random())),          # valid interior
        lambda: up, lambda: lo,                                            # exact edges (may be off tick)
        lambda: on_tick_near(up), lambda: on_tick_near(up) + tick(t, up),  # last legal, one tick beyond
        lambda: lo - D("0.1"), lambda: up + D("0.1"),
        lambda: on_tick_near(base) + D("0.1"),                             # off tick near base
        lambda: tick_boundary_price(inst, lo, up, rng),                   # around a tick-band edge
        lambda: base,
    ]
    # 40% valid interior prices, the rest spread over edge cases
    return picks[0]() if rng.random() < 0.4 else rng.choice(picks[1:])()


def fmt(px: D, rng) -> str:
    s = format(px.normalize(), "f")
    if rng.random() < 0.1 and "." not in s:
        s += ".0"          # trailing zero form
    if rng.random() < 0.05 and "." in s:
        s += "0"           # "123.50"
    return s


def main():
    inst_path, acct_path, n, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    ff = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    insts = {}
    with open(inst_path) as f:
        for r in csv.DictReader(row for row in f if not row.startswith("#")):
            insts[r["code"]] = {"base": D(r["base_price"]), "unit": int(r["trading_unit"]),
                                "table": TOPIX500 if r["tick_table"] == "TOPIX500" else OTHER,
                                "exp_up": r["expand_upper"] == "1", "exp_lo": r["expand_lower"] == "1",
                                "halted": r["halted"] == "1"}
    accts = {}
    with open(acct_path) as f:
        for r in csv.DictReader(f):
            accts[r["account"]] = int(r["credit_limit_yen"])
    oracle = Oracle(insts, accts, ff)
    rng = random.Random(7)
    codes = list(insts)
    with open(out, "w") as fo:
        for i in range(n):
            code = rng.choice(codes) if rng.random() > 0.01 else rng.choice(["9999", "72O3", "123"])
            inst = insts.get(code, insts[codes[0]])
            acct = rng.choices(list(accts) + ["GHOST"], weights=[85, 14, 1])[0]
            side = rng.choices(["1", "2", "5", "3"], weights=[49, 49, 1, 1])[0]
            otype = rng.choices(["2", "1", "4"], weights=[90, 9, 1])[0]
            unit = inst["unit"]
            qty = rng.choices([str(unit * rng.randint(1, 50)), str(unit * rng.randint(1, 50) + 50), "0",
                               "-100", "12a", str(unit * 20_000)], weights=[90, 3, 1, 1, 1, 4])[0]
            px = ""
            if otype == "2":
                r = rng.random()
                px = (rng.choice(["", "abc", "100.25", "-5", ".5", "1e3"]) if r < 0.02
                      else fmt(candidate_prices(inst, rng), rng))
            elif rng.random() < 0.02:
                px = "1000"   # price on a market order
            # ~2% reuse an earlier ClOrdID to exercise duplicate detection
            cid = f"R{rng.randint(0, i)}" if (i and rng.random() < 0.02) else f"R{i}"
            fields = [("35", "D" if rng.random() > 0.002 else "F"), ("1", acct), ("11", cid), ("55", code),
                      ("54", side), ("38", qty), ("40", otype)]
            if px:
                fields.append(("44", px))
            fields.append(("59", "0"))
            o = dict(fields)
            msg = checksum_msg(fields)
            if rng.random() < 0.003:                       # corrupt one byte: bad checksum
                j = msg.index("|55=") + 4
                msg = msg[:j] + ("8" if msg[j] != "8" else "9") + msg[j + 1:]
                o["malformed"] = True
            fo.write(f"{oracle.decide(o)}\t{msg}\n")


if __name__ == "__main__":
    main()
