#!/usr/bin/env bash
# 執行不必接板子的測試：單元測試、多語系、面板測試、對比測試與翻譯過的面板。加上 --hil 埠 時接著做實機測試。
#
#   ./tools/run_tests.sh
#   ./tools/run_tests.sh --hil /dev/cu.usbmodemXXXX [--update] [--long]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
fail=0
echo "== 單元測試"
tests/unit/run.sh || fail=1
echo "== 多語系"
python3 -I tools/make_i18n.py --check || fail=1
echo "== 面板網頁"
python3 -I tools/make_panel.py --check || fail=1
echo "== 面板測試"
python3 -I tests/panel/test_panel.py || fail=1
echo "== 對比測試"
python3 -I tests/panel/test_contrast.py || fail=1
echo "== 翻譯過的面板"
python3 -I tests/panel/test_i18n.py || fail=1
if [ "${1:-}" = "--hil" ]; then
  shift
  [ -n "${1:-}" ] || { echo "錯誤：--hil 後面要接序列埠" >&2; exit 2; }
  port="$1"; shift
  PY="${PIO_PYTHON:-$HOME/.local/share/uv/tools/platformio/bin/python}"
  [ -x "$PY" ] || PY=python3
  echo "== 實機測試"
  "$PY" -I tests/hil/run_hil.py --port "$port" "$@" || fail=1
fi
[ "$fail" = 0 ] && echo "全部通過" || echo "有測試失敗"
exit $fail
