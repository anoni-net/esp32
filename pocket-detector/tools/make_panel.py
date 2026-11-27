#!/usr/bin/env python3
"""把手機面板 pocket_detector/panel.html 用 gzip 壓縮，產生 pocket_detector/panel_gz.h。

板子送出網頁時標上 Content-Encoding: gzip，瀏覽器自己解壓縮，板子不必解壓，也不多用記憶體。
約 70 KB 的網頁壓縮後約 25 KB，用板子的熱點打開面板快很多。

    python3 -I tools/make_panel.py           # 重新產生 panel_gz.h（內容沒變就不寫）
    python3 -I tools/make_panel.py --check   # 檢查 panel_gz.h 是不是由目前的 panel.html 產生的

PlatformIO 建置前會自動執行（platformio.ini 的 extra_scripts）。panel_gz.h 也放進版控，
用 Arduino IDE 編譯時不必先執行這支腳本，但改了 panel.html 之後要記得重新產生。
檢查比對的是 panel.html 的 SHA-256，不比對壓縮後的 bytes，不同版本的 zlib 壓出來的結果可能不同。
"""
import gzip
import hashlib
import re
import sys
from pathlib import Path

try:
    ROOT = Path(__file__).resolve().parent.parent
except NameError:  # PlatformIO 以 SConscript 執行時沒有 __file__
    ROOT = Path.cwd()
SRC = ROOT / "pocket_detector" / "panel.html"
OUT = ROOT / "pocket_detector" / "panel_gz.h"


def source_sha(data):
    return hashlib.sha256(data).hexdigest()


def render(data):
    gz = gzip.compress(data, 9, mtime=0)
    lines = [
        "// 由 tools/make_panel.py 從 panel.html 產生，不要手改。改面板請改 panel.html，",
        "// PlatformIO 建置時會自動重新產生，用 Arduino IDE 時先執行 python3 -I tools/make_panel.py",
        f"// panel.html 的 SHA-256：{source_sha(data)}",
        f"// 原始 {len(data)} bytes，gzip 後 {len(gz)} bytes",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "static const uint8_t PANEL_GZ[] PROGMEM = {",
    ]
    for i in range(0, len(gz), 20):
        lines.append("  " + ",".join(str(b) for b in gz[i:i + 20]) + ",")
    lines += ["};", f"static const size_t PANEL_GZ_LEN = {len(gz)};", ""]
    return "\n".join(lines)


def write():
    data = SRC.read_bytes()
    text = render(data)
    old = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
    if recorded_sha(old) == source_sha(data):
        return False
    OUT.write_text(text, encoding="utf-8")
    print(f"make_panel：{OUT.name} 已更新（{len(data)} → {len(gzip.compress(data, 9, mtime=0))} bytes）")
    return True


def recorded_sha(text):
    m = re.search(r"panel\.html 的 SHA-256：([0-9a-f]{64})", text)
    return m.group(1) if m else ""


def check():
    data = SRC.read_bytes()
    old = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
    if recorded_sha(old) != source_sha(data):
        print("panel_gz.h 不是由目前的 panel.html 產生的，執行 python3 -I tools/make_panel.py")
        return 1
    # 解回原文比對，確認壓縮的內容真的是這份 panel.html
    body = re.search(r"PANEL_GZ\[\] PROGMEM = \{(.*?)\};", old, re.S).group(1)
    raw = gzip.decompress(bytes(int(x) for x in re.findall(r"\d+", body)))
    if raw != data:
        print("panel_gz.h 解壓縮後跟 panel.html 不同，執行 python3 -I tools/make_panel.py")
        return 1
    print("面板網頁：panel_gz.h 與 panel.html 一致")
    return 0


try:
    Import("env")  # noqa: F821  PlatformIO 的 extra_scripts
    write()
except NameError:
    if __name__ == "__main__":
        sys.exit(check() if "--check" in sys.argv[1:] else (write() and 0))
