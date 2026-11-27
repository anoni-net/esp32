#!/usr/bin/env python3
"""把序列埠 oled dump、oled demo 印出的 OLEDBUF 轉成 PNG，沒接 OLED 也能看畫面。

    python3 -I tools/oled_to_png.py 序列埠記錄.txt 輸出資料夾 [名稱 ...]

每一行 OLEDBUF 存成一張 oled_名稱.png，沒給名稱時用 shot0、shot1。SSD1306 的緩衝區是 8 列 × 128 欄，
每個 byte 是直的 8 個點，最低位元在上。只用標準函式庫。
"""
import os
import struct
import sys
import zlib

SCALE = 4


def png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main(log, outdir, names):
    with open(log, encoding="utf-8", errors="replace") as f:
        bufs = [bytes.fromhex(l.split("OLEDBUF,", 1)[1].strip()[:2048]) for l in f if "OLEDBUF," in l]
    os.makedirs(outdir, exist_ok=True)
    for i, b in enumerate(bufs):
        rows = []
        for y in range(64 * SCALE + 8):
            r = []
            for x in range(128 * SCALE + 8):
                yy, xx = (y - 4) // SCALE, (x - 4) // SCALE
                on = 0 <= yy < 64 and 0 <= xx < 128 and (b[(yy // 8) * 128 + xx] >> (yy % 8)) & 1
                r += [210, 235, 255] if on else [8, 10, 14]
            rows.append(r)
        name = names[i] if i < len(names) else f"shot{i}"
        path = os.path.join(outdir, f"oled_{name}.png")
        png(path, 128 * SCALE + 8, 64 * SCALE + 8, rows)
        print(path)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
