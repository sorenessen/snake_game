#!/usr/bin/env bash
set -euo pipefail

SRC="snake_raw.cpp"
BIN="snake"

# find a compiler
CXX="${CXX:-}"
if [[ -z "${CXX}" ]]; then
  if command -v clang++ >/dev/null 2>&1; then CXX=clang++;
  elif command -v g++ >/dev/null 2>&1; then CXX=g++;
  else echo "No C++ compiler found (clang++/g++)."; exit 1; fi
fi

# build only if needed (no-op if up to date)
if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
  echo "Building $BIN from $SRC with $CXX..."
  "$CXX" -std=c++17 "$SRC" -O2 -pthread -o "$BIN"
fi

echo "Running ./$BIN"
"./$BIN"

