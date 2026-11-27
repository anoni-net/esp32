#!/usr/bin/env bash
# 把測試通道上實機測試通過的韌體推到正式通道。
#
#   ./tools/promote_firmware.sh pocket-detector            # 測試通道目前的版本通過實機測試才推
#   ./tools/promote_firmware.sh pocket-detector --force    # 略過實機測試的紀錄（緊急修正用）
#
# 不重新建置：在主機上把 CHANNEL-test/ 的韌體檔與 latest.json 原封不動複製到 CHANNEL/，
# 正式通道的建置時間、SHA-256 與測試過的檔案完全相同。實機測試的紀錄在 <工具>/tests/hil/results/passed.tsv，
# 由 tests/hil/run_hil.py 寫入，比對的是板子開機時印出的建置時間，要先讓板子從測試通道更新到這個版本
# 再測，用 USB 燒進去的版本建置時間不同，不算數。主機設定同 tools/publish_firmware.sh。
set -euo pipefail

die() { printf '錯誤：%s\n' "$*" >&2; exit 1; }
TOOL="${1:-}"
[ -n "$TOOL" ] || die "要指定工具，例如 ./tools/promote_firmware.sh pocket-detector"
FORCE=0
[ "${2:-}" = "--force" ] && FORCE=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/$TOOL/publish.env" ] || die "找不到 $TOOL/publish.env"
# shellcheck source=/dev/null
. "$ROOT/$TOOL/publish.env"
# shellcheck source=/dev/null
[ -f "$ROOT/tools/publish.local.env" ] && . "$ROOT/tools/publish.local.env"
[ -n "${PUBLISH_HOST:-}" ] && [ -n "${PUBLISH_ROOT:-}" ] || die "要設定 PUBLISH_HOST 與 PUBLISH_ROOT（或寫在 tools/publish.local.env）"
SRC_DIR="${PUBLISH_ROOT}/${CHANNEL}-test"
DEST_DIR="${PUBLISH_ROOT}/${CHANNEL}"
TEST_URL="https://assets.anoni.net/${CHANNEL}-test"
URL="https://assets.anoni.net/${CHANNEL}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

curl -fsS "${TEST_URL}/latest.json?r=$RANDOM$RANDOM" -o "$WORK/latest.json" || die "讀不到 ${TEST_URL}/latest.json"
read -r BUILD FILE SHA COMMIT < <(python3 -I -c 'import json,sys; j=json.load(open(sys.argv[1])); print(j["build"].replace(" ","_"), j["file"], j["sha256"], j["commit"])' "$WORK/latest.json")
BUILD="${BUILD//_/ }"
echo "測試通道：${FILE}，建置於 ${BUILD}，commit ${COMMIT}"

PASSED="$ROOT/$TOOL/tests/hil/results/passed.tsv"
if [ "$FORCE" = 1 ]; then
  echo "（--force，略過實機測試的紀錄）"
elif ! { [ -f "$PASSED" ] && awk -F'\t' -v b="$BUILD" '$1==b && $5=="update_pass"{f=1} END{exit !f}' "$PASSED"; }; then
  die "建置於 ${BUILD} 的版本還沒通過實機測試。執行 $TOOL/tests/hil/run_hil.py --update，讓板子從測試通道裝上這個版本、新版也讀得到測試通道"
fi

cur="$(curl -fsS "${URL}/latest.json?r=$RANDOM$RANDOM" || true)"
[ "$cur" = "$(cat "$WORK/latest.json")" ] && { echo "正式通道已經是這個版本"; exit 0; }

# 網頁安裝的 manifest.json 與它列出的檔案（舊的版本沒有 manifest.json 時略過）
curl -fsS "${TEST_URL}/manifest.json?r=$RANDOM$RANDOM" -o "$WORK/manifest.json" 2>/dev/null || rm -f "$WORK/manifest.json"
PARTS=""
if [ -f "$WORK/manifest.json" ]; then
  PARTS="$(python3 -I -c 'import json,sys; j=json.load(open(sys.argv[1])); f=sys.argv[2]
ps=[p["path"] for b in j["builds"] for p in b["parts"]]
assert f in ps, "manifest.json 沒有指向 latest.json 的韌體"
print(" ".join(p for p in ps if p!=f))' "$WORK/manifest.json" "$FILE")" || die "測試通道的 manifest.json 跟 latest.json 對不上"
fi

# 先放韌體檔，manifest.json 與 latest.json 最後用 mv 換上，板子與網頁不會讀到指向還不存在的檔案的版本
ssh "$PUBLISH_HOST" "set -e; mkdir -p '$DEST_DIR'; cp -p '$SRC_DIR/$FILE' '$DEST_DIR/$FILE'; \
  for p in $PARTS; do cp -p '$SRC_DIR/'\$p '$DEST_DIR/'\$p; done; \
  if [ -n '$PARTS' ]; then cp '$SRC_DIR/manifest.json' '$DEST_DIR/.manifest.json.tmp'; mv '$DEST_DIR/.manifest.json.tmp' '$DEST_DIR/manifest.json'; fi; \
  cp '$SRC_DIR/latest.json' '$DEST_DIR/.latest.json.tmp'; mv '$DEST_DIR/.latest.json.tmp' '$DEST_DIR/latest.json'"

got="$(curl -fsS "${URL}/latest.json?r=$RANDOM$RANDOM")" || die "讀不到 ${URL}/latest.json"
[ "$got" = "$(cat "$WORK/latest.json")" ] || die "${URL}/latest.json 的內容跟測試通道不同"
curl -fsS "${URL}/${FILE}" -o "$WORK/check.bin" || die "下載不到 ${URL}/${FILE}"
[ "$(shasum -a 256 "$WORK/check.bin" | cut -d' ' -f1)" = "${SHA}" ] || die "${URL}/${FILE} 的 SHA-256 不符"
echo "已推到正式通道 ${URL}/${FILE}（建置於 ${BUILD}，commit ${COMMIT}）"
