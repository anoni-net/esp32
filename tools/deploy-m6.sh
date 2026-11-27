#!/bin/sh
# 在社群的主機上建置並發布專案網頁 anoni.net/esp32/，由 crontab 每 5 分鐘執行一次：
#
#   */5 * * * * /home/ubuntu/esp32-deploy.sh
#
# 本檔是那支腳本的原始檔，改了之後要複製到主機的 /home/ubuntu/esp32-deploy.sh。
# 做法跟 anoni-net/privacy-mystery 的 tools/deploy-m6.sh 相同。nginx 的設定範本在 tools/nginx-esp32.conf。
#
# 目錄結構：
#   /srv/anoni-net-esp32/repo            anoni-net/esp32 的 clone，只拉 main
#   /srv/anoni-net-esp32/releases/<key>  每次建置一份，保留最近 KEEP 份
#   /srv/anoni-net-esp32/current         symlink，nginx 的 /esp32/ 讀 current/clearnet，esp32.<onion> 讀 current/onion
#
# 網頁寫出正式通道目前的版本（latest.json），所以版本換了也要重建：建置的鍵是 commit 加上
# 各通道 latest.json 的雜湊。建置完成、檢查通過之後才切換 current，切換是原子操作，建置失敗時
# 線上維持原本的版本。只接受 fast-forward，main 的歷史被改寫時停下來寫進 log，等人處理。
# 同一個鍵建置失敗過就不再重試。有更新時才寫 log。上一輪還沒跑完時這一輪直接略過。
# 要暫停自動發布，建立 /srv/anoni-net-esp32/hold，刪掉就恢復。
set -eu

BASE=/srv/anoni-net-esp32
REPO=$BASE/repo
LOG=/home/ubuntu/esp32-deploy.log
UV=/home/ubuntu/.local/bin/uv
KEEP=5
# 網頁上寫出版本的通道（各工具 site.toml 的 channel）
CHANNELS="esp32-tracker"

exec 9>/tmp/esp32-deploy.lock
flock -n 9 || exit 0
[ -e "$BASE/hold" ] && exit 0

before=$(git -C "$REPO" rev-parse --short=12 HEAD)
if ! git -C "$REPO" pull --ff-only -q origin main 2>>"$LOG"; then
    echo "$(date -Iseconds) 無法 fast-forward，停在 $before，需要人工處理" >>"$LOG"
    exit 1
fi
sha=$(git -C "$REPO" rev-parse --short=12 HEAD)
ver=""
for c in $CHANNELS; do
    # 讀不到版本資訊時這一輪略過，不要建出一份沒有版本的網頁
    v=$(curl -fsS -A anoni-esp32-site "https://assets.anoni.net/$c/latest.json?r=$(date +%s)" 2>/dev/null) || exit 0
    ver="$ver$v"
done
key="$sha-$(printf '%s' "$ver" | sha256sum | cut -c1-8)"
current=$(readlink "$BASE/current" 2>/dev/null || true)
[ "$current" = "releases/$key" ] && exit 0
[ "$(cat "$BASE/failed" 2>/dev/null || true)" = "$key" ] && exit 0

dest=$BASE/releases/$key
need="index.html pocket-detector/index.html playground/index.html about-esp32/index.html install/index.html demo/pocket-detector/index.html demo/pocket-detector/frame/alert.html static/site.css shots/pocket-detector/alert.png"
rm -rf "$dest.tmp"
# build.py 本身會檢查站內連結、外部資源與 onion 版本裡的 clearnet 網址，失敗時回傳非零
if ! (cd "$REPO" && "$UV" run -q site/build.py --out "$dest.tmp" \
        && for t in clearnet onion; do for f in $need; do test -s "$dest.tmp/$t/$f" || exit 1; done; done) \
        >/dev/null 2>>"$LOG"; then
    echo "$(date -Iseconds) $key 建置或檢查失敗，線上維持 ${current:-（尚未發布）}" >>"$LOG"
    echo "$key" >"$BASE/failed"
    rm -rf "$dest.tmp"
    exit 1
fi
rm -rf "$dest"
mv "$dest.tmp" "$dest"
ln -sfn "releases/$key" "$BASE/current.tmp"
mv -T "$BASE/current.tmp" "$BASE/current"
rm -f "$BASE/failed"
echo "$(date -Iseconds) 發布 $key $(git -C "$REPO" log -1 --format=%s)" >>"$LOG"

# 保留最近 KEEP 份，正在使用的那一份不會被刪
ls -1t "$BASE/releases" | tail -n +$((KEEP + 1)) | while read -r old; do
    [ "releases/$old" = "releases/$key" ] || rm -rf "${BASE:?}/releases/$old"
done
