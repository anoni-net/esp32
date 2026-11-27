// 會議室檢查用的藍牙廣播解析：裝置類型、使用中的跡象與型號。
//
// 來源：
// - Microsoft：MS-CDP（Connected Devices Platform）公開規格的 Bluetooth 信標格式、Swift Pair
// - Apple Continuity：Celosia、Cunche〈Discontinued Privacy: Personal Data Leaks in Apple
//   Bluetooth-Low-Energy Continuity Protocols〉（PETS 2020）與 hexway apple_bleee 的逆向研究。
//   不是官方規格，面板上標成「估計」
// - Google Fast Pair：可配對時的服務資料是 3 bytes 的型號編號
//
// 界線：AirDrop 廣播裡有電話號碼與 email 的雜湊片段，只記下「AirDrop 開著」，不解析雜湊。
// 裝置廣播的名稱（常帶主人的名字）由呼叫的人另外填進 name。都只放在記憶體、顯示在面板，不寫進記錄。
// 不依賴 Arduino，韌體與 tests/unit/ 編譯同一份。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string>

namespace devinfo {

// 粗分類，會議室檢查判斷「換了位址」時用（同一類才配對）
enum Kind : uint8_t { APPLE, GOOGLE, SAMSUNG, MICROSOFT, OTHER, KIND_COUNT };

enum Flag : uint8_t {
  IN_USE = 1,    // Apple Handoff：正在使用
  AIRDROP = 2,   // AirDrop 開著
  HOTSPOT = 4,   // 可以分享行動網路（多半是有門號的 iPhone、iPad）
  FINDMY = 8,    // 與主人分開的 Apple 裝置
  ESTIMATE = 16  // 類型或型號是從逆向研究推得的
};

struct Info {
  Kind kind = OTHER;
  std::string label;  // 面板顯示的類型與型號
  uint8_t flags = 0;
  int8_t activity = -1;  // Apple Nearby Info 的活動碼，-1 代表沒有
  uint8_t rank = 0;      // 類型有多具體，越大越具體，同一台裝置收到多種廣播時留最具體的
  std::string name;      // 裝置廣播的名稱，沒有時是空字串
};

// Apple Nearby Info 的活動碼（低 4 位元），PETS 2020 表格。只列有把握的
static inline const char* activityText(int code) {
  switch (code) {
    case 0x03: return "閒置";
    case 0x05: return "螢幕關著，正在播放聲音";
    case 0x07: return "螢幕亮著";
    case 0x09: return "螢幕亮著，正在播放影片";
    case 0x0A: return "手錶戴著、已解鎖";
    case 0x0B: return "剛剛有人操作";
    case 0x0D: return "開車中";
    case 0x0E: return "通話中或 FaceTime";
    default: return "";
  }
}

// AirPods、Beats 的型號編號（Proximity Pairing 廣播，大端序），apple_bleee 的對照
static inline const char* appleAudioModel(uint16_t m) {
  switch (m) {
    case 0x0220: return "AirPods";
    case 0x0F20: return "AirPods（第 2 代）";
    case 0x1320: return "AirPods（第 3 代）";
    case 0x0E20: return "AirPods Pro";
    case 0x1420: return "AirPods Pro（第 2 代）";
    case 0x0A20: return "AirPods Max";
    case 0x0320: return "Powerbeats3";
    case 0x0B20: return "Powerbeats Pro";
    case 0x0520: return "BeatsX";
    case 0x0620: return "Beats Solo3";
    case 0x0920: return "Beats Studio3";
    case 0x0C20: return "Beats Solo Pro";
    case 0x1020: return "Beats Flex";
    case 0x1120: return "Beats Studio Buds";
    default: return "";
  }
}

// MS-CDP 信標的裝置類型（第 2 個 byte 的低 5 位元）
static inline const char* cdpDeviceType(int t) {
  switch (t) {
    case 1: return "Xbox";
    case 6: return "iPhone（裝了 Microsoft 的手機連結）";
    case 7: return "iPad（裝了 Microsoft 的手機連結）";
    case 8: return "Android 手機（裝了 Microsoft 的手機連結）";
    case 9: return "Windows 桌上型電腦";
    case 11: return "Windows 手機";
    case 12: return "Linux 裝置";
    case 13: return "Windows IoT 裝置";
    case 14: return "Surface Hub";
    case 15: return "Windows 筆電";
    case 16: return "Windows 平板";
    default: return "";
  }
}

static inline void setLabel(Info& in, const std::string& label, uint8_t rank) {
  if (rank >= in.rank) {
    in.label = label;
    in.rank = rank;
  }
}

// mfr：manufacturer data（開頭兩個 byte 是公司代碼，小端序），沒有時給空字串。
// fastPair：0xFE2C 的服務資料，沒有時給空字串
static inline Info parse(const std::string& mfr, const std::string& fastPair) {
  Info in;
  const int company = mfr.size() >= 2 ? (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8) : -1;
  char b[96];
  if (company == 0x004C) {
    in.kind = APPLE;
    setLabel(in, "Apple 裝置（iPhone、iPad、Mac 或手錶）", 1);
    // Continuity：從第 3 個 byte 起是「類型、長度、內容」一段接一段
    size_t i = 2;
    while (i + 2 <= mfr.size()) {
      const uint8_t type = mfr[i], len = mfr[i + 1];
      if (i + 2 + len > mfr.size()) break;
      const uint8_t* p = (const uint8_t*)mfr.data() + i + 2;
      switch (type) {
        case 0x05:  // AirDrop：只記下開著，內容是聯絡資訊的雜湊，不解析
          in.flags |= AIRDROP;
          break;
        case 0x07:  // Proximity Pairing：AirPods、Beats。第一個 byte 是 0x01 時才是型號在第 2、3 個 byte 的格式，
                    // 2026-11-27 實測收到第一個 byte 是 0x06、長度 17 的變體，型號的位置不同，只標成配件
          if (len >= 3 && p[0] != 0x01) {
            setLabel(in, "Apple 配件（耳機或其他配件）", 2);
            in.flags |= ESTIMATE;
          } else if (len >= 3) {
            const uint16_t model = (p[1] << 8) | p[2];
            const char* name = appleAudioModel(model);
            if (*name) snprintf(b, sizeof(b), "%s", name);
            else snprintf(b, sizeof(b), "Apple 或 Beats 耳機（型號編號 0x%04X）", model);
            setLabel(in, b, 4);
            in.flags |= ESTIMATE;
          }
          break;
        case 0x0C:  // Handoff：使用者正在用支援接續互通的 App
          in.flags |= IN_USE;
          break;
        case 0x0E:  // Tethering Source：可以分享行動網路
          in.flags |= HOTSPOT;
          setLabel(in, "iPhone 或 iPad（有行動網路）", 3);
          in.flags |= ESTIMATE;
          break;
        case 0x10:  // Nearby Info：低 4 位元是活動碼
          if (len >= 1) {
            in.activity = p[0] & 0x0F;
            if (in.activity == 0x0A) {
              setLabel(in, "Apple Watch", 3);
              in.flags |= ESTIMATE;
            }
          }
          break;
        case 0x12:  // Find My：與主人分開
          in.flags |= FINDMY;
          setLabel(in, "與主人分開的 Apple 裝置或配件", 2);
          break;
      }
      i += 2 + len;
    }
    return in;
  }
  if (company == 0x0006 && mfr.size() >= 4) {
    in.kind = MICROSOFT;
    const uint8_t scenario = mfr[2];
    if (scenario == 0x01) {  // MS-CDP 信標
      const char* t = cdpDeviceType((uint8_t)mfr[3] & 0x1F);
      setLabel(in, *t ? t : "Windows、Microsoft 裝置", *t ? 3 : 1);
    } else if (scenario == 0x03) {
      setLabel(in, "可以用 Swift Pair 配對的配件", 2);
    } else {
      setLabel(in, "Windows、Microsoft 裝置", 1);
    }
    return in;
  }
  if (fastPair.size() == 3) {
    in.kind = GOOGLE;
    snprintf(b, sizeof(b), "可以用 Fast Pair 配對的配件（型號編號 %02X%02X%02X）", (uint8_t)fastPair[0],
             (uint8_t)fastPair[1], (uint8_t)fastPair[2]);
    setLabel(in, b, 3);
    return in;
  }
  if (company == 0x00E0 || !fastPair.empty()) {
    in.kind = GOOGLE;
    setLabel(in, "Android、Google 裝置", 1);
    return in;
  }
  if (company == 0x0075) {
    in.kind = SAMSUNG;
    setLabel(in, "Samsung 裝置", 1);
    return in;
  }
  setLabel(in, "其他藍牙裝置", 0);
  return in;
}

}  // namespace devinfo
