#!/usr/bin/env bash
# 在電腦上編譯並執行單元測試，不必接板子。測的是韌體 include 的同一份標頭。
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
fail=0
for src in test_*.cpp; do
  "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -O1 -o "$OUT/${src%.cpp}" "$src"
  "$OUT/${src%.cpp}" || fail=1
done
exit $fail
