#!/usr/bin/env python3
"""面板配色的對比測試（WCAG 2.2 AA 的 1.4.3 與 1.4.11）。

從 panel.html 讀出 :root 與深色模式的顏色設定，計算文字與背景、元件與背景的對比。
不必接板子，也不必開瀏覽器。

    python3 -I tests/panel/test_contrast.py

一般文字要 4.5:1，大字（24px 以上，或 18.66px 以上的粗體）要 3:1，判斷狀態要看的元件
（開關、輸入框外框、焦點外框）要 3:1。改顏色之後執行一次。
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(os.environ.get("PANEL_SRC", ROOT / "pocket_detector" / "panel.html")).read_text(encoding="utf-8")


def tokens(block):
    return {k: v for k, v in re.findall(r"--(\w+):(#[0-9A-Fa-f]{6})", block)}


light = tokens(re.search(r":root\{([^}]*)\}", SRC).group(1))
dark = dict(light, **tokens(re.search(r"prefers-color-scheme:dark\)\{:root\{([^}]*)\}", SRC).group(1)))
light.setdefault("off", "#4A5560")
dark.setdefault("off", "#4A5560")
m = re.search(r"body\.off\{--state:(#[0-9A-Fa-f]{6})", SRC)
if m:
    light["off"] = dark["off"] = m.group(1)


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


def lum(c):
    f = lambda v: v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = map(f, c)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def ratio(a, b):
    la, lb = lum(a), lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def mix(fg, bg, alpha):
    return tuple(f * alpha + b * (1 - alpha) for f, b in zip(fg, bg))


def css_alpha(selector, prop):
    """讀出 CSS 規則裡的 rgba 透明度，例如 .chip 的背景"""
    m = re.search(re.escape(selector) + r"\{[^}]*" + prop + r":rgba\((\d+),(\d+),(\d+),([.\d]+)\)", SRC)
    return (tuple(int(m.group(i)) / 255 for i in (1, 2, 3)), float(m.group(4))) if m else None


def opacity(selector):
    m = re.search(re.escape(selector) + r"\{[^}]*opacity:([.\d]+)", SRC)
    return float(m.group(1)) if m else 1.0


W = rgb("#FFFFFF")
fails, total = [], 0


def check(mode, name, fg, bg, need):
    global total
    total += 1
    if fg is None or bg is None:
        fails.append(f"{mode}：{name} 缺少顏色設定")
        return
    r = ratio(fg, bg)
    if r < need:
        fails.append(f"{mode}：{name} 只有 {r:.2f}，需要 {need}")


chip = css_alpha(".chip", "background")
for mode, t in (("淺色", light), ("深色", dark)):
    class Colors(dict):
        def __missing__(self, key):
            return None
    c = Colors({k: rgb(v) for k, v in t.items()})
    for st in ("hsafe", "hwatch", "halert", "off"):
        s = c[st]
        check(mode, f"頂端小字（{st}）", mix(W, s, opacity(".who")), s, 4.5)
        check(mode, f"周圍環境的字（{st}）", mix(W, s, opacity(".env")), s, 4.5)
        check(mode, f"狀態說明（{st}）", mix(W, s, opacity(".say")), s, 4.5)
        if chip:
            check(mode, f"狀態標籤（{st}）", W, mix(chip[0], s, chip[1]), 4.5)
        check(mode, f"頂端的焦點外框（{st}）", W, s, 3)
    check(mode, "內文", c["ink"], c["surf"], 4.5)
    check(mode, "次要文字／卡片", c["mute"], c["surf"], 4.5)
    check(mode, "次要文字／底", c["mute"], c["bg"], 4.5)
    check(mode, "觀察中、可以留意", c["watchInk"], c["surf"], 4.5)
    check(mode, "警示中、建議找出來", c["alert"], c["surf"], 4.5)
    check(mode, "主要按鈕的字", c["onAct"], c["act"], 4.5)
    check(mode, "一般按鈕的字", c["ink"], c["bg"], 4.5)
    check(mode, "警告標籤", c["hwatch"], W, 4.5)
    check(mode, "警示列的按鈕", c["halert"], W, 4.5)
    check(mode, "開關關閉時的底", c["mute"], c["surf"], 3)
    check(mode, "開關打開時的底", c["safe"], c["surf"], 3)
    check(mode, "開關圓鈕／關閉", c["knob"], c["mute"], 3)
    check(mode, "開關圓鈕／打開", c["knob"], c["safe"], 3)
    check(mode, "輸入框外框／卡片", c["edge"], c["surf"], 3)
    check(mode, "輸入框外框／底", c["edge"], c["bg"], 3)
    check(mode, "分段按鈕選中的外框", c["act"], c["bg"], 3)
    check(mode, "焦點外框", c["act"], c["surf"], 3)

for f in fails:
    print("失敗 " + f)
print(f"對比測試：{total - len(fails)} / {total} 通過")
sys.exit(1 if fails else 0)
