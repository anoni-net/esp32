"""把手機面板 pocket_detector/panel.html 用 gzip 壓縮，產生 pocket_detector/panel_gz.h 與各語系的版本。

板子送出網頁時標上 Content-Encoding: gzip，瀏覽器自己解壓縮，板子不必解壓，也不多用記憶體。
約 70 KB 的網頁壓縮後約 25 KB，用板子的熱點打開面板快很多。

    python3 -I tools/make_panel.py           # 重新產生 panel_gz*.h（內容沒變就不寫）
    python3 -I tools/make_panel.py --check   # 檢查 panel_gz*.h 是不是由目前的 panel.html 與翻譯產生的

正體中文（panel_gz.h）照 panel.html 原樣壓縮。簡體中文（panel_gz_zh_cn.h）與英文（panel_gz_en.h）
用 i18n/panel.toml 的翻譯替換含中文的片段、去掉註解再壓縮（tools/i18n_lib.py）。

PlatformIO 建置前會自動執行（platformio.ini 的 extra_scripts）。panel_gz*.h 也放進版控，
用 Arduino IDE 編譯時不必先執行這支腳本，但改了 panel.html 或翻譯之後要記得重新產生。
檢查比對的是面板網頁的 SHA-256，不比對壓縮後的 bytes，不同版本的 zlib 壓出來的結果可能不同。
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
sys.path.insert(0, str(ROOT / "tools"))
import i18n_lib as L  # noqa: E402

SRC = ROOT / "pocket_detector" / "panel.html"


def out_path(lang):
    return ROOT / "pocket_detector" / f"panel_gz{L.LANGS[lang]['suffix']}.h"


def page(lang):
    """回傳（這個語系的面板網頁 bytes, 缺少翻譯的片段）"""
    text = SRC.read_text(encoding="utf-8")
    if lang == "zh-TW":
        return SRC.read_bytes(), []
    html, missing = L.translate_panel(text, lang, L.load(L.PANEL_CATALOG))
    return html.encode("utf-8"), missing


def source_sha(data):
    return hashlib.sha256(data).hexdigest()


def render(data, lang="zh-TW"):
    gz = gzip.compress(data, 9, mtime=0)
    lines = [
        f"// 由 tools/make_panel.py 從 panel.html 產生（{lang}），不要手改。改面板請改 panel.html，",
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


def write(require=()):
    """產生各語系的 panel_gz*.h。require 裡的語系缺翻譯時丟出錯誤（建置那個語系的韌體時）"""
    changed = False
    for lang in L.LANGS:
        data, missing = page(lang)
        if missing:
            msg = f"make_panel：{lang} 的面板缺 {len(missing)} 個翻譯，例如「{missing[0]}」，執行 tools/make_i18n.py --extract 後補上"
            if lang in require:
                raise SystemExit(msg)
            print(msg)
        out = out_path(lang)
        old = out.read_text(encoding="utf-8") if out.exists() else ""
        if recorded_sha(old) == source_sha(data):
            continue
        out.write_text(render(data, lang), encoding="utf-8")
        print(f"make_panel：{out.name} 已更新（{len(data)} → {len(gzip.compress(data, 9, mtime=0))} bytes）")
        changed = True
    return changed


def recorded_sha(text):
    m = re.search(r"panel\.html 的 SHA-256：([0-9a-f]{64})", text)
    return m.group(1) if m else ""


def check():
    fail = 0
    for lang in L.LANGS:
        data, missing = page(lang)
        out = out_path(lang)
        old = out.read_text(encoding="utf-8") if out.exists() else ""
        if missing:
            print(f"{lang} 的面板缺 {len(missing)} 個翻譯，執行 tools/make_i18n.py --check 看是哪些")
            fail = 1
            continue
        if lang == "en" and L.CJK.search(data.decode("utf-8")):
            print(f"英文的面板還有中文：{L.CJK.search(data.decode('utf-8')).group(0)}")
            fail = 1
            continue
        if recorded_sha(old) != source_sha(data):
            print(f"{out.name} 不是由目前的 panel.html 產生的，執行 python3 -I tools/make_panel.py")
            fail = 1
            continue
        # 解回原文比對，確認壓縮的內容真的是這份面板
        body = re.search(r"PANEL_GZ\[\] PROGMEM = \{(.*?)\};", old, re.S).group(1)
        raw = gzip.decompress(bytes(int(x) for x in re.findall(r"\d+", body)))
        if raw != data:
            print(f"{out.name} 解壓縮後跟面板網頁不同，執行 python3 -I tools/make_panel.py")
            fail = 1
    if not fail:
        print(f"面板網頁：panel_gz*.h 與 panel.html 一致（{'、'.join(L.LANGS)}）")
    return fail


try:
    Import("env")  # noqa: F821  PlatformIO 的 extra_scripts
    write(require=(env.GetProjectOption("custom_fw_lang", "zh-TW"),))  # noqa: F821
except NameError:
    if __name__ == "__main__":
        sys.exit(check() if "--check" in sys.argv[1:] else (write() and 0))
