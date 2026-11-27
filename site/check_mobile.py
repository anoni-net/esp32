#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["jinja2>=3.1", "markdown>=3.5"]
# ///
"""用無頭瀏覽器把每一頁拍成手機寬度（預設 390px）的截圖，檢查小螢幕的排版。

    uv run site/check_mobile.py 輸出資料夾 [寬度]

先執行 uv run site/build.py，讀 site/public/clearnet。無頭瀏覽器的視窗最窄 500px，頁面放進手機寬度的
iframe 再拍，用 macOS 的 sips 裁掉兩側。除了截圖，也列出比視窗寬、會讓頁面左右捲動的元素。
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shots import find_browser  # noqa: E402

PUB = Path(__file__).resolve().parent / "public" / "clearnet"
PROBE = ("<script>addEventListener('load',()=>setTimeout(()=>{const w=document.documentElement.clientWidth;"
         "const p=document.createElement('pre');p.id='__w';p.textContent=JSON.stringify([...document.querySelectorAll('body *')]"
         ".filter(e=>e.getBoundingClientRect().right>w+1&&!e.closest('pre,.table')).slice(0,8).map(e=>e.tagName+'.'+e.className+' '"
         "+Math.round(e.getBoundingClientRect().right)));parent.postMessage(p.textContent,'*')},1500))</script>")


def run(browser, args, tmp):
    for _ in range(3):
        try:
            return subprocess.run([browser, "--headless=new", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
                                   f"--user-data-dir={tmp / 'profile'}", "--virtual-time-budget=3000", *args],
                                  capture_output=True, text=True, timeout=40)
        except subprocess.TimeoutExpired:
            pass
    raise SystemExit("瀏覽器逾時")


def main(out, width):
    browser = find_browser()
    out.mkdir(parents=True, exist_ok=True)
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        for page in sorted(PUB.rglob("index.html")):
            rel = page.relative_to(PUB).parent.as_posix().replace("/", "-")
            rel = "home" if rel in ("", ".") else rel
            # 量測：在頁面副本裡加一段 script，列出超出寬度的元素
            probe = page.with_name("__probe.html")
            probe.write_text(page.read_text(encoding="utf-8").replace("</body>", PROBE + "</body>"), encoding="utf-8")
            frame = tmp / f"{rel}.html"
            # iframe 裡的頁面量完之後用 postMessage 傳出來，寫進外層的 DOM
            frame.write_text(f'<!doctype html><body style="margin:0"><iframe src="{probe.as_uri()}" '
                             f'style="border:0;display:block;margin:0 auto;width:{width}px;height:2600px"></iframe>'
                             "<script>addEventListener('message',e=>{const p=document.createElement('pre');p.id='__w';"
                             "p.textContent=e.data;p.style.display='none';document.body.appendChild(p)})</script>", encoding="utf-8")
            dom = run(browser, ["--window-size=500,2600", "--dump-dom", frame.as_uri()], tmp)
            shot = out / f"{rel}.png"
            run(browser, ["--window-size=500,2600", f"--screenshot={shot}", frame.as_uri()], tmp)
            probe.unlink()
            subprocess.run(["sips", "--cropToHeightWidth", "2600", str(width), str(shot)], capture_output=True)
            m = re.findall(r'id="__w">([^<]*)', dom.stdout)
            print(f"{rel}：{m[0] if m else '量測失敗'}")
    return bad


if __name__ == "__main__":
    main(Path(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 390)
