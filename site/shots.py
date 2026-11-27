#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["jinja2>=3.1", "markdown>=3.5"]
# ///
"""用無頭瀏覽器拍下模擬面板各個情境的截圖，存在 <工具>/site/shots/<情境>.png，
再用第一個工具的警示畫面做網站的分享卡 site/static/og.png（1200x630，同 anoni-net/www 的 og.png）。

    uv run site/shots.py                    # 全部的工具與情境
    uv run site/shots.py pocket-detector    # 只拍指定的工具
    CHROME=/path/to/chrome uv run site/shots.py

截圖裁切用 macOS 內建的 sips。情境與面板測試共用（例如 pocket-detector/tests/panel/scenarios.py），資料都是虛構的。截圖放進版控，
主機部署時不必裝瀏覽器。改了面板或情境之後重新執行，連同 PNG 一起 commit。
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import load_tools  # noqa: E402

W, H = 390, 844


def find_browser():
    cands = [os.environ.get("CHROME"), "google-chrome", "chromium", "chromium-browser",
             "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
             "/Applications/Chromium.app/Contents/MacOS/Chromium",
             "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
             "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"]
    for c in cands:
        if c and (shutil.which(c) or Path(c).exists()):
            return shutil.which(c) or c
    sys.exit("找不到 Chromium 系的瀏覽器，用 CHROME=路徑 指定")


def shoot(browser, page, dest, tmp):
    """全新的設定檔第一次啟動偶爾會卡住，逾時就重試一次。"""
    cmd = [browser, "--headless=new", "--disable-gpu", "--no-sandbox", "--hide-scrollbars", f"--window-size=500,{H}",
           "--force-device-scale-factor=2", f"--user-data-dir={tmp / 'profile'}", "--virtual-time-budget=4000",
           f"--screenshot={dest}", page.as_uri()]
    for attempt in range(3):
        try:
            subprocess.run(cmd, capture_output=True, timeout=40, check=True)
            return
        except subprocess.TimeoutExpired:
            if attempt == 2:
                raise


OG = """<!doctype html><html lang="zh-Hant-TW"><head><meta charset="utf-8"><style>
html,body{margin:0;width:1200px;height:630px;overflow:hidden;background:#00202d;color:#fff;
  font-family:system-ui,-apple-system,"PingFang TC","Noto Sans TC",sans-serif}
.wrap{display:flex;height:630px;padding:0 72px;gap:56px;align-items:center}
.text{flex:1}
.brand{display:flex;align-items:center;gap:14px;font-size:30px;font-weight:700;color:#b3e3ff}
.brand img{width:44px;height:44px}
h1{font-size:88px;line-height:1.15;margin:28px 0 24px}
p{font-size:34px;line-height:1.5;margin:0;color:#d8eef7}
.url{margin-top:36px;font-size:28px;color:#80d1ff}
.phone{width:300px;height:560px;border-radius:28px;overflow:hidden;border:3px solid #234757;align-self:flex-end;margin-top:70px}
.phone img{width:100%;display:block}
</style></head><body><div class="wrap"><div class="text">
<div class="brand"><img src="LOGO" alt="">anoni.net</div>
<h1>ESP32 小工具</h1>
<p>隨身偵測器：找出一直跟著你的藍牙追蹤器，也能檢查旅館房間與會議室</p>
<div class="url">anoni.net/esp32</div>
</div><div class="phone"><img src="SHOT" alt=""></div></div></body></html>"""


def og(browser, tool, tmp):
    page = tmp / "og.html"
    page.write_text(OG.replace("LOGO", (Path(__file__).resolve().parent / "static" / "logo.svg").as_uri())
                    .replace("SHOT", (tool["dir"] / "site" / "shots" / "alert.png").as_uri()), encoding="utf-8")
    dest = Path(__file__).resolve().parent / "static" / "og.png"
    cmd = [browser, "--headless=new", "--disable-gpu", "--no-sandbox", "--hide-scrollbars", "--window-size=1200,630",
           f"--user-data-dir={tmp / 'profile'}", "--virtual-time-budget=2000", f"--screenshot={dest}", page.as_uri()]
    for attempt in range(3):
        try:
            subprocess.run(cmd, capture_output=True, timeout=40, check=True)
            break
        except subprocess.TimeoutExpired:
            if attempt == 2:
                raise
    print(f"site/static/og.png（{dest.stat().st_size // 1024} KB）")


def main(only):
    browser = find_browser()
    tools = [t for t in load_tools() if t["scenarios"] and (not only or t["slug"] in only)]
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        for t in tools:
            panel = (t["dir"] / t["demo"]["page"]).read_text(encoding="utf-8")
            marker = "<script>\nconst $"
            out = t["dir"] / "site" / "shots"
            out.mkdir(parents=True, exist_ok=True)
            for s in t["scenarios"].DEMO:
                page = tmp / f"{t['slug']}-{s['key']}.html"
                page.write_text(panel.replace(marker, t["scenarios"].demo_script(s, shot=True) + marker, 1), encoding="utf-8")
                # 無頭瀏覽器的視窗最窄 500px，面板放進置中、手機寬度的 iframe，截圖後裁掉兩側
                # （sips 的裁切固定從中間取，--cropOffset 沒有作用）
                frame = tmp / f"{t['slug']}-{s['key']}-frame.html"
                frame.write_text(f'<!doctype html><body style="margin:0"><iframe src="{page.name}" '
                                 f'style="border:0;display:block;margin:0 auto;width:{W}px;height:{H}px"></iframe>',
                                 encoding="utf-8")
                dest = out / f"{s['key']}.png"
                shoot(browser, frame, dest, tmp)
                subprocess.run(["sips", "--cropToHeightWidth", str(H * 2), str(W * 2), str(dest)],
                               capture_output=True, check=True)
                print(f"{dest.relative_to(t['dir'].parent)}（{dest.stat().st_size // 1024} KB）")
        if tools and not only:
            og(browser, tools[0], tmp)


if __name__ == "__main__":
    main(sys.argv[1:])
