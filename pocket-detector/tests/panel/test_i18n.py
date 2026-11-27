#!/usr/bin/env python3
"""翻譯過的面板（簡體中文、英文）的測試：用面板測試的每個情境載入，確認沒有 JS 錯誤。

面板測試（test_panel.py）比對的是正體中文的文字，翻譯過的面板只檢查程式還能正常執行：翻譯是把片段
換掉再接回去，引號或標籤接錯會讓整段 JS 失效。每個情境沿用 test_panel.py 的假資料與分頁，
檢查換成「沒有 JS 錯誤、<html lang> 正確」。翻譯是否齊全、有沒有殘留中文由 tools/make_i18n.py --check
與 tools/make_panel.py --check 負責。

    python3 -I tests/panel/test_i18n.py            # 所有語系、所有情境
    python3 -I tests/panel/test_i18n.py en         # 只測英文
"""
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools"))
import i18n_lib as L  # noqa: E402
import test_panel as T  # noqa: E402


def main(langs):
    langs = langs or L.TRANSLATED
    browser = T.find_browser()
    src = L.PANEL_SRC.read_text(encoding="utf-8")
    entries = L.load(L.PANEL_CATALOG)
    failed = total = 0
    with tempfile.TemporaryDirectory() as d:
        for lang in langs:
            page, missing = L.translate_panel(src, lang, entries)
            if missing:
                print(f"失敗 {lang}：缺 {len(missing)} 個翻譯，先執行 tools/make_i18n.py --check")
                failed += 1
                continue
            want = L.LANGS[lang]["html"]
            for name, case in T.CASES.items():
                over, tab, _check, *extra = case
                T.CASES[name] = (over, tab, f"return document.documentElement.lang=={want!r}?[]:['lang 不對']", *extra)
                fails = T.run_case(browser, page, name, Path(d))
                T.CASES[name] = case
                total += 1
                if fails:
                    failed += 1
                    print(f"失敗 {lang} {name}")
                    for f in fails:
                        print("    " + f)
            print(f"{lang}：{len(T.CASES)} 個情境跑完")
    print(f"翻譯面板測試：{total - failed} / {total} 通過")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
