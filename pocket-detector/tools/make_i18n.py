#!/usr/bin/env python3
"""隨身偵測器的多語系：抽出要翻譯的字串、產生韌體的字串表、檢查翻譯。

    python3 -I tools/make_i18n.py             # 產生 pocket_detector/i18n_zh_cn.h、i18n_en.h（內容沒變就不寫）
    python3 -I tools/make_i18n.py --check     # 檢查目錄齊全、格式一致，字串表是由目前的目錄產生的
    uv run --with opencc-python-reimplemented tools/make_i18n.py --extract
                                              # 把面板與韌體裡新出現的字串加進 i18n/*.toml，
                                              # 簡體中文先用 OpenCC（tw2sp）轉一份草稿，英文留空

原文改了之後執行 --extract，再補上英文、校對簡體中文的用詞。目錄的格式見 tools/i18n_lib.py。
PlatformIO 建置前會自動產生字串表（platformio.ini 的 extra_scripts），表跟 panel_gz*.h 一樣放進版控。
"""
import sys
from pathlib import Path

try:
    HERE = Path(__file__).resolve().parent
except NameError:  # PlatformIO 以 SConscript 執行時沒有 __file__
    HERE = Path.cwd() / "tools"
sys.path.insert(0, str(HERE))
import i18n_lib as L  # noqa: E402

HEADER_PANEL = """# 面板網頁（pocket_detector/panel.html）的翻譯。tw 是 panel.html 裡含中文的片段，
# 片段以 HTML 標籤、引號與 \\n 切開。改了 panel.html 之後執行 tools/make_i18n.py --extract 補上新的片段。
# 格式與規則見 tools/i18n_lib.py"""
HEADER_FW = """# 韌體字串的翻譯。tw 是原始碼裡 TR("…") 的內容，照 C 的寫法（\\n、%s 照原樣）。
# printf 的格式翻譯後要一樣、順序也要一樣。改了原始碼之後執行 tools/make_i18n.py --extract
# 格式與規則見 tools/i18n_lib.py"""


def header_path(lang):
    return L.ROOT / "pocket_detector" / f"i18n{L.LANGS[lang]['suffix']}.h"


def write():
    entries = L.load(L.FW_CATALOG)
    keys = L.fw_strings()
    changed = False
    for lang in L.TRANSLATED:
        p = header_path(lang)
        text = L.c_header(lang, entries, keys)
        if not p.exists() or p.read_text(encoding="utf-8") != text:
            p.write_text(text, encoding="utf-8")
            print(f"make_i18n：{p.name} 已更新（{len(keys)} 個字串）")
            changed = True
    return changed


def check():
    problems = []
    keys = L.fw_strings()
    fw = L.load(L.FW_CATALOG)
    problems += L.check_catalog(fw, keys, True, "韌體")
    segs = list(dict.fromkeys(L.panel_segments(L.PANEL_SRC.read_text(encoding="utf-8"))))
    panel = L.load(L.PANEL_CATALOG)
    problems += L.check_catalog(panel, segs, False, "面板")
    for lang in L.TRANSLATED:
        p = header_path(lang)
        if not p.exists() or p.read_text(encoding="utf-8") != L.c_header(lang, fw, keys):
            problems.append(f"{p.name} 不是由目前的目錄產生的，執行 python3 -I tools/make_i18n.py")
    unused_fw = [e["tw"] for e in fw if e["tw"] not in keys]
    unused_panel = [e["tw"] for e in panel if e["tw"] not in segs]
    if unused_fw or unused_panel:
        problems.append(f"目錄裡有用不到的字串（韌體 {len(unused_fw)} 個、面板 {len(unused_panel)} 個），"
                        "執行 tools/make_i18n.py --extract 清掉")
    for p in problems[:60]:
        print(p)
    if len(problems) > 60:
        print(f"……另外 {len(problems) - 60} 個問題")
    if problems:
        return 1
    print(f"多語系：韌體 {len(keys)} 個字串、面板 {len(segs)} 個片段，{'、'.join(L.TRANSLATED)} 都有翻譯")
    return 0


def extract():
    try:
        from opencc import OpenCC
        cc = OpenCC("tw2sp")
    except ImportError:
        cc = None
        print("沒有 OpenCC，新的字串簡體中文留空（uv run --with opencc-python-reimplemented 執行可以先轉一份草稿）")
    segs = list(dict.fromkeys(L.panel_segments(L.PANEL_SRC.read_text(encoding="utf-8"))))
    for path, needed, header in ((L.PANEL_CATALOG, segs, HEADER_PANEL), (L.FW_CATALOG, L.fw_strings(), HEADER_FW)):
        old = {e["tw"]: e for e in L.load(path)}
        out, added = [], 0
        for k in needed:
            e = old.get(k)
            if not e:
                e = {"tw": k, "cn": cc.convert(k) if cc else "", "en": ""}
                added += 1
            out.append(e)
        removed = len([k for k in old if k not in needed])
        L.I18N.mkdir(exist_ok=True)
        L.dump(path, out, header)
        print(f"{path.relative_to(L.ROOT)}：{len(out)} 筆，新增 {added}，移除 {removed}")
    if cc:
        # 正體字清單：原文裡轉成簡體會變的字。檢查簡體中文的翻譯時不能出現這些字
        chars = set()
        for path in (L.PANEL_CATALOG, L.FW_CATALOG):
            for e in L.load(path):
                chars |= {c for c in e["tw"] if L.CJK.match(c) and OpenCC("t2s").convert(c) != c}
        L.TRAD_CHARS.write_text("".join(sorted(chars)) + "\n", encoding="utf-8")
        print(f"{L.TRAD_CHARS.relative_to(L.ROOT)}：{len(chars)} 個字")


try:
    Import("env")  # noqa: F821  PlatformIO 的 extra_scripts
    write()
except NameError:
    if __name__ == "__main__":
        args = sys.argv[1:]
        if "--check" in args:
            sys.exit(check())
        if "--extract" in args:
            extract()
        else:
            write()
