#!/usr/bin/env bash
# Generate labeled orders with the independent Python oracle and replay them through the
# C++ engine. Fails if there is any false reject, false accept or wrong reject reason.
set -euo pipefail
N=${1:-300000}
BUILD=${BUILD:-build}
mkdir -p out
python3 scripts/gen_replay.py data/instruments_replay.csv data/accounts_replay.csv "$N" out/replay.tsv
"$BUILD/replay" data/instruments_replay.csv data/accounts_replay.csv out/replay.tsv
python3 scripts/gen_replay.py data/instruments_replay.csv data/accounts_replay.csv "$N" out/replay_ff.tsv 1500
"$BUILD/replay" data/instruments_replay.csv data/accounts_replay.csv out/replay_ff.tsv --fat-finger-bps 1500
