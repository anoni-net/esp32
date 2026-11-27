// 藍牙廣播解析的單元測試，編譯韌體用的同一份 devinfo.h。廣播內容是照公開格式組出來的，不是實際收到的。
#include "../../pocket_detector/devinfo.h"

#include <cstdio>
#include <string>

static int fails = 0, total = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    total++;                                                         \
    if (!(cond)) {                                                   \
      fails++;                                                       \
      std::printf("失敗 %s:%d：%s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)

using namespace devinfo;

static std::string bytes(std::initializer_list<int> v) {
  std::string s;
  for (int b : v) s += (char)b;
  return s;
}

int main() {
  // Apple Nearby Info，活動碼 0x07：螢幕亮著
  Info a = parse(bytes({0x4C, 0x00, 0x10, 0x05, 0x37, 0x1C, 0x00, 0x00, 0x00}), "");
  CHECK(a.kind == APPLE && a.activity == 0x07 && std::string(activityText(a.activity)) == "螢幕亮著");
  CHECK(a.label == "Apple 裝置（iPhone、iPad、Mac 或手錶）");
  // 手錶：活動碼 0x0A
  Info w = parse(bytes({0x4C, 0x00, 0x10, 0x02, 0x1A, 0x00}), "");
  CHECK(w.label == "Apple Watch" && (w.flags & ESTIMATE));
  // Handoff 與 Nearby Info 同時出現：正在使用
  Info h = parse(bytes({0x4C, 0x00, 0x0C, 0x02, 0x00, 0x00, 0x10, 0x02, 0x0B, 0x00}), "");
  CHECK((h.flags & IN_USE) && h.activity == 0x0B);
  // 個人熱點來源：iPhone 或 iPad
  Info t = parse(bytes({0x4C, 0x00, 0x0E, 0x02, 0x01, 0x02}), "");
  CHECK((t.flags & HOTSPOT) && t.label == "iPhone 或 iPad（有行動網路）");
  // AirDrop：只記下開著，雜湊不解析
  Info d = parse(bytes({0x4C, 0x00, 0x05, 0x04, 0x11, 0x22, 0x33, 0x44}), "");
  CHECK((d.flags & AIRDROP) && d.label.find("11") == std::string::npos);
  // AirPods Pro
  Info p = parse(bytes({0x4C, 0x00, 0x07, 0x05, 0x01, 0x0E, 0x20, 0x55, 0xAA}), "");
  CHECK(p.label == "AirPods Pro" && p.rank == 4);
  // 沒有對照的耳機型號
  Info p2 = parse(bytes({0x4C, 0x00, 0x07, 0x03, 0x01, 0x99, 0x20}), "");
  CHECK(p2.label == "Apple 或 Beats 耳機（型號編號 0x9920）");
  // 2026-11-27 實測：第一個 byte 是 0x06 的變體，型號的位置不同，不能當成型號
  Info p3 = parse(bytes({0x4C, 0x00, 0x07, 0x11, 0x06, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}), "");
  CHECK(p3.label == "Apple 配件（耳機或其他配件）");
  // 長度超出的 TLV 不越界
  Info bad = parse(bytes({0x4C, 0x00, 0x10, 0x09, 0x07}), "");
  CHECK(bad.kind == APPLE && bad.activity == -1);
  // Microsoft CDP：Windows 筆電（裝置類型 15）、Android（8）、Swift Pair
  CHECK(parse(bytes({0x06, 0x00, 0x01, 0x2F, 0x00}), "").label == "Windows 筆電");
  CHECK(parse(bytes({0x06, 0x00, 0x01, 0x08, 0x00}), "").label == "Android 手機（裝了 Microsoft 的手機連結）");
  CHECK(parse(bytes({0x06, 0x00, 0x03, 0x00, 0x80}), "").label == "可以用 Swift Pair 配對的配件");
  // Google Fast Pair 的型號編號
  CHECK(parse("", bytes({0x2A, 0x41, 0x0F})).label == "可以用 Fast Pair 配對的配件（型號編號 2A410F）");
  CHECK(parse(bytes({0xE0, 0x00, 0x01}), "").kind == GOOGLE);
  CHECK(parse(bytes({0x75, 0x00, 0x01}), "").label == "Samsung 裝置");
  CHECK(parse("", "").label == "其他藍牙裝置");

  std::printf("藍牙廣播解析測試：%d / %d 通過\n", total - fails, total);
  return fails ? 1 : 0;
}
