#!/usr/bin/env bash
# 建置一個工具的韌體，發布到 assets.anoni.net 的測試通道，打開「接收測試版」的板子就能從網址更新。
#
#   ./tools/publish_firmware.sh pocket-detector            # 建置並發布到測試通道
#   ./tools/publish_firmware.sh pocket-detector --dry-run  # 建置並產生 latest.json，不上傳
#
# 工具的設定在 <工具>/publish.env（通道目錄、PlatformIO 環境、檔名開頭）。上傳用 ssh 與 rsync，
# 主機與 assets.anoni.net 在主機上的根目錄由環境變數 PUBLISH_HOST、PUBLISH_ROOT 指定，
# 也可以寫在 tools/publish.local.env（不進版控）。
#
# 這支腳本不發布正式版。實機測試（<工具>/tests/hil/run_hil.py）通過後，用 tools/promote_firmware.sh
# 把測試通道上的同一個檔案複製到正式通道，不重新建置，建置時間與 SHA-256 都不變。
# 韌體的檔名帶 SHA-256 的前 12 碼，同一個檔名的內容不會變，Cloudflare 快取舊的也沒關係。
# latest.json 最後才換上，先寫到暫存檔再 mv，板子不會讀到寫一半的檔案。
# 工具的資料夾或 lib/ 有還沒 commit 的變更時拒絕發布，發布出去的版本要對得回 commit。ALLOW_DIRTY=1 可以略過。
set -euo pipefail

die() { printf '錯誤：%s\n' "$*" >&2; exit 1; }
TOOL="${1:-}"
[ -n "$TOOL" ] || die "要指定工具，例如 ./tools/publish_firmware.sh pocket-detector"
DRY_RUN=0
[ "${2:-}" = "--dry-run" ] && DRY_RUN=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/$TOOL/publish.env" ] || die "找不到 $TOOL/publish.env"
# shellcheck source=/dev/null
. "$ROOT/$TOOL/publish.env"
# shellcheck source=/dev/null
[ -f "$ROOT/tools/publish.local.env" ] && . "$ROOT/tools/publish.local.env"
URL="https://assets.anoni.net/${CHANNEL}-test"
# 板子下載時用的 User-Agent（lib/netupdate）
UA="anoni-tracker"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cd "$ROOT/$TOOL"
if [ -n "$(git status --porcelain -- . ../lib)" ] && [ "${ALLOW_DIRTY:-0}" != 1 ]; then
  die "$TOOL 或 lib/ 有還沒 commit 的變更，先 commit 再發布（或設 ALLOW_DIRTY=1）"
fi
COMMIT="$(git rev-parse --short HEAD)"
[ -n "$(git status --porcelain -- . ../lib)" ] && COMMIT="${COMMIT}-dirty"

# 每次都重新編譯主程式，建置時間才會是這一次的
touch "$SKETCH"
pio run -e "$PIO_ENV" >"$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log"; die "建置失敗"; }
BIN=".pio/build/${PIO_ENV}/firmware.bin"
BUILD="$(LC_ALL=C grep -a -o "${BUILD_MARKER}:[A-Z][a-z][a-z] [ 0-9][0-9] [0-9]\{4\} [0-9][0-9]:[0-9][0-9]:[0-9][0-9]" "$BIN" | head -1 | cut -c$((${#BUILD_MARKER} + 2))-)"
[ -n "${BUILD}" ] || die "在 firmware.bin 裡找不到建置時間"
SHA="$(shasum -a 256 "$BIN" | cut -d' ' -f1)"
SIZE="$(wc -c < "$BIN" | tr -d ' ')"
FILE="${PREFIX}-${SHA:0:12}.bin"
cp "$BIN" "$WORK/${FILE}"
python3 -I -c 'import json,sys; print(json.dumps({"build":sys.argv[1],"file":sys.argv[2],"size":int(sys.argv[3]),"sha256":sys.argv[4],"commit":sys.argv[5]}, ensure_ascii=False))' \
  "${BUILD}" "${FILE}" "${SIZE}" "${SHA}" "${COMMIT}" > "$WORK/latest.json"
cat "$WORK/latest.json"
[ "$DRY_RUN" = 1 ] && { echo "（--dry-run，沒有上傳）"; exit 0; }

[ -n "${PUBLISH_HOST:-}" ] && [ -n "${PUBLISH_ROOT:-}" ] || die "要設定 PUBLISH_HOST 與 PUBLISH_ROOT（或寫在 tools/publish.local.env）"
DEST="${PUBLISH_ROOT}/${CHANNEL}-test"
ssh "$PUBLISH_HOST" "mkdir -p '$DEST'"
rsync -t --chmod=F644 "$WORK/${FILE}" "$PUBLISH_HOST:$DEST/${FILE}"
rsync -t --chmod=F644 "$WORK/latest.json" "$PUBLISH_HOST:$DEST/.latest.json.tmp"
ssh "$PUBLISH_HOST" "mv '$DEST/.latest.json.tmp' '$DEST/latest.json'"

# 經過 Cloudflare 確認讀得到，內容跟剛才產生的一樣
got="$(curl -fsS "${URL}/latest.json?r=$RANDOM$RANDOM")" || die "讀不到 ${URL}/latest.json"
[ "$got" = "$(cat "$WORK/latest.json")" ] || die "${URL}/latest.json 的內容跟剛發布的不同"
curl -fsS "${URL}/${FILE}" -o "$WORK/check.bin" || die "下載不到 ${URL}/${FILE}"
[ "$(shasum -a 256 "$WORK/check.bin" | cut -d' ' -f1)" = "${SHA}" ] || die "${URL}/${FILE} 的 SHA-256 不符"
# 用板子的條件再確認一次：HTTP/1.1、板子的 User-Agent、不帶 Referer，要拿到 200 與正確的長度，
# 不能是 Cloudflare 的挑戰頁或分段傳送
for path in "latest.json?r=$RANDOM" "${FILE}"; do
  h="$(curl -sS -o /dev/null -D - --http1.1 -A "$UA" "${URL}/${path}" | tr -d '\r')"
  echo "$h" | head -1 | grep -q ' 200' || die "${URL}/${path} 用板子的條件讀取失敗：$(echo "$h" | head -1)"
  echo "$h" | grep -qi '^transfer-encoding: chunked' && [ "${path}" = "${FILE}" ] && die "${URL}/${FILE} 是分段傳送，板子不接受"
done
len="$(curl -sS -o /dev/null -D - --http1.1 -A "$UA" "${URL}/${FILE}" | tr -d '\r' | awk 'tolower($1)=="content-length:"{print $2}')"
[ "${len}" = "${SIZE}" ] || die "${URL}/${FILE} 的 Content-Length（${len}）跟檔案大小（${SIZE}）不同"
echo "已發布到測試通道 ${URL}/${FILE}（${SIZE} bytes，建置於 ${BUILD}，commit ${COMMIT}）"
