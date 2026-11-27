"""隨身偵測器的多語系：讀寫語系目錄、翻譯面板網頁、找出韌體裡的 TR() 字串、檢查翻譯。

原文是正體中文，寫在 panel.html 與韌體的原始碼裡。翻譯放在 i18n/ 的兩份目錄：

    i18n/panel.toml      面板網頁（panel.html）裡含中文的片段
    i18n/firmware.toml   韌體裡用 TR("…") 包起來、會顯示在面板或藍牙文字通道的字串

每一筆寫原文（tw）、簡體中文（cn）與英文（en）：

    [[s]]
    tw = '''開始尋找'''
    cn = '''开始寻找'''
    en = '''Start finding'''

面板的片段用 <、>、引號與 \\n 切開，同一段文字裡的 HTML 標籤與 JS 的變數不在片段裡，翻譯時照原文的
順序接回去。韌體的字串照 C 的寫法（\\n、%s 照原樣），printf 的格式（%s、%d…）翻譯後要一樣、順序也要一樣。

目錄是 TOML 的一小部分（[[s]] 與單行的 '''…'''），自己解析，不用 tomllib：macOS 內建的 python3 是 3.9，
PlatformIO 與 CI 的 Python 版本也不一定。
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent
I18N = ROOT / "i18n"
PANEL_SRC = ROOT / "pocket_detector" / "panel.html"
PANEL_CATALOG = I18N / "panel.toml"
FW_CATALOG = I18N / "firmware.toml"
TRAD_CHARS = I18N / "trad_chars.txt"

# 語系：代碼、目錄裡的欄位、韌體的巨集、產生的檔名後綴、HTML 的 lang、日期時間的 locale
LANGS = {
    "zh-TW": {"key": "tw", "macro": None, "suffix": "", "html": "zh-Hant", "locale": "zh-TW"},
    "zh-CN": {"key": "cn", "macro": "FW_LANG_ZH_CN", "suffix": "_zh_cn", "html": "zh-Hans", "locale": "zh-CN"},
    "en": {"key": "en", "macro": "FW_LANG_EN", "suffix": "_en", "html": "en", "locale": "en-GB"},
}
TRANSLATED = [k for k in LANGS if k != "zh-TW"]

CJK = re.compile(r"[　-〿㐀-鿿＀-￯]")
# 韌體原始碼：TR("…")，只接受單一個字串常值
TR_RE = re.compile(r'\bTR\(\s*"((?:[^"\\\n]|\\.)*)"\s*\)')
FMT_RE = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[diouxXfFeEgGcsp%]")
FW_SOURCES = [ROOT / "pocket_detector", REPO / "lib" / "netupdate", REPO / "lib" / "phonelink"]


# ---- 目錄 ----

ENTRY_RE = re.compile(r"^(tw|cn|en) = '''(.*)'''$")


def load(path: Path) -> list[dict]:
    """讀 dump() 寫出的目錄。只認 [[s]] 與單行的 key = '''…''' 兩種行，其他格式直接報錯"""
    if not path.exists():
        return []
    entries = []
    for n, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        if line == "[[s]]":
            entries.append({})
            continue
        m = ENTRY_RE.match(line)
        if not m or not entries:
            raise ValueError(f"{path.name} 第 {n} 行的格式不對：{line[:60]}")
        entries[-1][m.group(1)] = m.group(2)
    return entries


def dump(path: Path, entries: list[dict], header: str) -> None:
    out = [header.rstrip(), ""]
    for e in entries:
        out.append("[[s]]")
        for k in ("tw", "cn", "en"):
            v = e.get(k, "")
            if "'''" in v:
                raise ValueError(f"字串裡不能有三個單引號：{v}")
            out.append(f"{k} = '''{v}'''")
        out.append("")
    path.write_text("\n".join(out), encoding="utf-8")


# 英文刻意留空時寫 ∅（例如數字後面的量詞「個」），跟忘了翻譯的空字串分開
EMPTY = "∅"


def table(entries: list[dict], key: str) -> dict[str, str]:
    """原文 → 翻譯。沒有翻譯的是 None，刻意留空（∅）的是空字串"""
    return {e["tw"]: (None if not e.get(key) else "" if e[key] == EMPTY else e[key]) for e in entries}


# ---- 面板 ----

SPLIT = re.compile(r"(<|>|\"|'|\\n|\n)")


def panel_segments(text: str) -> list[str]:
    """panel.html 裡所有含中文的片段（去掉前後空白），依出現順序，可能重複"""
    return [t.strip() for t in SPLIT.split(strip_comments(text)) if CJK.search(t)]


def strip_comments(text: str) -> str:
    """去掉 CSS 的 /* */ 與 JS 的 // 註解。翻譯過的面板不帶中文註解，也小一點"""
    def css(m):
        return re.sub(r"/\*.*?\*/", "", m.group(0), flags=re.S)
    text = re.sub(r"<style>.*?</style>", css, text, flags=re.S)

    def js(m):
        body = re.sub(r"^[ \t]*//[^\n]*\n", "", m.group(0), flags=re.M)
        return re.sub(r"(?<=[;{}),])[ \t]+//[^\n]*", "", body)
    return re.sub(r"<script>.*?</script>", js, text, flags=re.S)


def translate_panel(text: str, lang: str, entries: list[dict]) -> tuple[str, list[str]]:
    """回傳（翻譯後的面板, 缺少翻譯的片段）"""
    info = LANGS[lang]
    if lang == "zh-TW":
        return text, []
    tr = table(entries, info["key"])
    missing = []
    out = []
    for tok in SPLIT.split(strip_comments(text)):
        if CJK.search(tok):
            core = tok.strip()
            val = tr.get(core)
            if val is None:
                missing.append(core)
                val = core
            lead = tok[: len(tok) - len(tok.lstrip())]
            trail = tok[len(tok.rstrip()):]
            tok = lead + val + trail
        out.append(tok)
    html = "".join(out)
    html = html.replace('<html lang="zh-Hant">', f'<html lang="{info["html"]}">', 1)
    html = html.replace("'zh-TW'", f"'{info['locale']}'")
    return html, missing


# ---- 韌體 ----

def fw_sources() -> list[Path]:
    files = []
    for d in FW_SOURCES:
        files += sorted(p for p in d.glob("*") if p.suffix in (".ino", ".h", ".cpp")
                        and not p.name.startswith(("panel_gz", "i18n_")))
    return files


def fw_strings() -> list[str]:
    """韌體原始碼裡所有 TR("…") 的字串，依出現順序、不重複"""
    seen = []
    for f in fw_sources():
        for m in TR_RE.finditer(f.read_text(encoding="utf-8")):
            if m.group(1) not in seen:
                seen.append(m.group(1))
    return seen


def c_header(lang: str, entries: list[dict], keys: list[str]) -> str:
    info = LANGS[lang]
    tr = table(entries, info["key"])
    lines = [
        f"// 由 tools/make_i18n.py 從 i18n/firmware.toml 產生，不要手改。{lang} 的韌體字串，",
        "// TR(\"正體中文\") 在編譯時查這張表（i18n.h），表裡沒有的字串編譯會失敗",
        "#pragma once",
        "namespace i18n {",
        f"constexpr int N = {len(keys)};",
        "constexpr const char* KEYS[N] = {",
    ]
    lines += [f'  "{k}",' for k in keys]
    lines += ["};", "constexpr const char* VALS[N] = {"]
    lines += [f'  "{k if tr.get(k) is None else tr[k]}",' for k in keys]
    lines += ["};", "}  // namespace i18n", ""]
    return "\n".join(lines)


# ---- 檢查 ----

def trad_chars() -> set[str]:
    return set(TRAD_CHARS.read_text(encoding="utf-8").strip()) if TRAD_CHARS.exists() else set()


def check_value(tw: str, val: str, lang: str, fmt: bool, trad: set[str]) -> str | None:
    if not val:
        return "沒有翻譯"
    if val == EMPTY:
        return None if lang == "en" else "只有英文可以留空（∅）"
    if lang == "en" and CJK.search(val):
        return "英文翻譯裡有中文字"
    if lang == "zh-CN":
        bad = sorted({c for c in val if c in trad})
        if bad:
            return "簡體中文翻譯裡有正體字：" + "".join(bad)
    if fmt and FMT_RE.findall(tw) != FMT_RE.findall(val):
        return f"printf 的格式不同：{FMT_RE.findall(tw)} → {FMT_RE.findall(val)}"
    return None


def check_catalog(entries: list[dict], needed: list[str], fmt: bool, name: str) -> list[str]:
    problems = []
    have = {e["tw"]: e for e in entries}
    trad = trad_chars()
    for k in needed:
        e = have.get(k)
        if not e:
            problems.append(f"{name}：目錄裡沒有「{k}」")
            continue
        for lang in TRANSLATED:
            why = check_value(k, e.get(LANGS[lang]["key"], ""), lang, fmt, trad)
            if why:
                problems.append(f"{name}：「{k}」的 {lang} {why}")
    return problems
