#!/usr/bin/env python3
"""產生房間檢查用的廠商識別表 pocket_detector/roomcheck_ids.h。

從三份公開的登記資料挑出攝影機品牌與 IoT 模組廠商：

- IEEE MA-L（OUI，MAC 位址的前三碼）：https://standards-oui.ieee.org/oui/oui.csv
- 藍牙 SIG 公司代碼（廣播的 manufacturer data 開頭兩個 byte）：
  https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/company_identifiers/company_identifiers.yaml
- 藍牙 SIG 會員的 16 位元服務 UUID：
  https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/uuids/member_uuids.yaml

用法（下載的檔案放在另一個資料夾）：

    python3 -I tools/make_roomcheck_ids.py oui.csv company_identifiers.yaml member_uuids.yaml 2026-11-26

最後一個參數是下載日期，寫進產生的檔頭。廠商清單改 VENDORS，比對的是登記的公司名稱。
"""
import csv
import re
import sys
from pathlib import Path

# (面板顯示的名稱, 類別, 比對登記名稱的正規表示式)
# 類別 1：攝影機品牌，產品以監視器、網路攝影機為主，看到就值得找出來
# 類別 2：IoT 模組與晶片廠，便宜的 Wi-Fi 攝影機常用，但插座、燈泡、感測器也用，只算弱的理由
VENDORS = [
    ("Hikvision", 1, r"hikvision"),
    ("Dahua", 1, r"dahua"),
    ("EZVIZ", 1, r"ezviz"),
    ("Uniview", 1, r"uniview"),
    ("Axis", 1, r"^axis communications"),
    ("Vivotek", 1, r"^vivotek"),
    ("Reolink", 1, r"^reolink"),
    ("Wyze", 1, r"^wyze labs"),
    ("Arlo", 1, r"^arlo technolog"),
    ("Ring", 1, r"^ring llc"),
    ("Blink", 1, r"^blink by amazon"),
    ("Amcrest", 1, r"^amcrest"),
    ("Lorex", 1, r"^lorex"),
    ("Swann", 1, r"^swann communications"),
    ("Hanwha Vision", 1, r"hanwha vision|samsung techwin"),
    ("GeoVision", 1, r"^geovision"),
    ("Tiandy", 1, r"^tiandy"),
    ("Jovision", 1, r"jovision"),
    ("YI（小蟻）", 1, r"shanghai xiaoyi"),
    ("創米（小米攝影機）", 1, r"shanghai imilab"),
    ("Milesight", 1, r"milesight"),
    ("Gwell（Yoosee）", 1, r"gwelltimes"),
    ("Espressif", 2, r"espressif"),
    ("Tuya", 2, r"tuya"),
    ("Beken", 2, r"^beken"),
    ("Bouffalo Lab", 2, r"bouffalo"),
    ("AltoBeam", 2, r"^altobeam"),
    ("LB-LINK（必聯）", 2, r"shenzhen bilian"),
    ("Winner Micro", 2, r"winner micro"),
    ("HiSilicon", 2, r"^hisilicon"),
    ("Ingenic", 2, r"^ingenic semiconductor"),
    ("Aqara（綠米）", 2, r"^lumi united"),
    ("BroadLink", 2, r"broadlink"),
    ("小米", 2, r"^xiaomi inc"),  # 只用在藍牙：米家裝置的 MiBeacon。Wi-Fi 的小米 OUI 多半是手機，不列
]


def main(oui_csv, company_yaml, uuid_yaml, date):
    pats = [(i, re.compile(p, re.I)) for i, (_, _, p) in enumerate(VENDORS)]

    def match(name):
        for i, rx in pats:
            if rx.search(name.strip()):
                return i
        return None

    oui = []
    with open(oui_csv, encoding="utf-8") as f:
        for row in csv.reader(f):
            if len(row) < 3 or row[0] != "MA-L":
                continue
            i = match(row[2])
            if i is None or VENDORS[i][0] == "小米":
                continue
            oui.append((int(row[1], 16), i))
    oui.sort()

    def yaml_pairs(path, key):
        # 這兩份 YAML 的格式固定：「- key: 0x…」下一行是「name: …」，不必另外裝 YAML 函式庫
        out, val = [], None
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            m = re.match(rf"\s*-\s*{key}:\s*(0x[0-9A-Fa-f]+)", line)
            if m:
                val = int(m.group(1), 16)
                continue
            m = re.match(r"\s*name:\s*['\"]?(.*?)['\"]?\s*$", line)
            if m and val is not None:
                out.append((val, m.group(1)))
                val = None
        return out

    company = sorted((v, i) for v, n in yaml_pairs(company_yaml, "value") if (i := match(n)) is not None)
    uuids = sorted((v, i) for v, n in yaml_pairs(uuid_yaml, "uuid") if (i := match(n)) is not None)

    used = sorted({i for _, i in oui + company + uuids})
    lines = [
        "// 由 tools/make_roomcheck_ids.py 產生，不要手改。",
        f"// 來源（{date} 下載）：IEEE MA-L 登記資料 oui.csv，藍牙 SIG assigned numbers 的",
        "// company_identifiers.yaml 與 member_uuids.yaml。只收 VENDORS 列出的廠商。",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "namespace roomcheck {",
        "",
        "struct Vendor {",
        "  const char* name;",
        "  uint8_t cat;  // 1 攝影機品牌、2 IoT 模組與晶片廠",
        "};",
        "",
        "static const Vendor VENDORS[] = {",
    ]
    for i, (name, cat, _) in enumerate(VENDORS):
        lines.append(f'  {{"{name}", {cat}}},  // {i}' + ("" if i in used else "（登記資料裡沒有對應的項目）"))
    lines += ["};", "", f"// MAC 位址前三碼 << 8 | 廠商編號，依前三碼排序，共 {len(oui)} 筆",
              "static const uint32_t OUI_TABLE[] = {"]
    for k in range(0, len(oui), 6):
        lines.append("  " + " ".join(f"0x{o:06X}{i:02X}," for o, i in oui[k:k + 6]))
    lines += ["};", "", f"// 藍牙公司代碼 << 8 | 廠商編號，共 {len(company)} 筆", "static const uint32_t BLE_COMPANY[] = {"]
    lines.append("  " + " ".join(f"0x{v:04X}{i:02X}," for v, i in company))
    lines += ["};", "", f"// 藍牙 16 位元服務 UUID << 8 | 廠商編號，共 {len(uuids)} 筆", "static const uint32_t BLE_UUID16[] = {"]
    lines.append("  " + " ".join(f"0x{v:04X}{i:02X}," for v, i in uuids))
    lines += ["};", "", "}  // namespace roomcheck", ""]

    out = Path(__file__).resolve().parent.parent / "pocket_detector" / "roomcheck_ids.h"
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"{out}：OUI {len(oui)} 筆、公司代碼 {len(company)} 筆、UUID {len(uuids)} 筆")
    for i, (name, cat, _) in enumerate(VENDORS):
        n = [sum(1 for _, j in t if j == i) for t in (oui, company, uuids)]
        print(f"  {name}（類別 {cat}）：OUI {n[0]}、公司代碼 {n[1]}、UUID {n[2]}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
