// 型別定義放在獨立的標頭檔。Arduino 會自動替 .ino 裡的函式產生宣告，
// 插在檔案前段，型別若寫在 .ino 裡，宣告會比型別先出現而無法編譯

#pragma once

#include <stdint.h>

#include <string>

enum class TrackerType : uint8_t {
  None,
  AppleFindMySeparated,  // Find My 配件與主人分開（AirTag、AirPods 與相容的 Find My 配件）
  AppleDevice,           // 與主人分開的 Apple 自家裝置（iPhone、iPad、Mac），iOS 不會對它們警示
  SamsungSmartTag,
  Tile,
  GoogleFindHub,         // Google 尋找我的裝置網路的配件
  Dult,                  // 只認得出 DULT 廣播、而且與主人分開的追蹤器
  Chipolo,               // 廣播不標示是否與主人分開
  Pebblebee,             // 廣播不標示是否與主人分開
};

// DULT（IETF 的 Detecting Unwanted Location Trackers）廣播的最近紀錄，在掃描的回呼裡
// 逐筆更新。支援 DULT 的追蹤器會輪流發送自家格式與 DULT 格式的廣播
struct DultSeen {
  uint8_t addr[6];
  uint32_t ms;     // 最後一次收到的時間
  uint8_t net;     // 尋找網路的編號
  bool nearOwner;  // 主人在附近
};

struct Sighting {
  std::string addr;
  TrackerType type;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint16_t windows;      // 在幾次掃描裡出現過
  uint16_t nearWindows;  // 其中訊號達到 ALERT_MIN_RSSI 的次數
  uint32_t firstScan;    // 第一次出現在第幾次掃描
  int rssi;
  bool alerted;
  bool acknowledged;
  uint16_t lastPlace;    // 最近一次出現時所在的地點編號（Wi-Fi 移動判斷）
  uint8_t places;        // 出現過的不同地點數。回到同一個地點不重複計算
  uint16_t seenPl[8];    // 出現過的地點編號，最近的 8 個
  uint8_t nPl;
  int8_t rh[10];         // 最近 10 次出現時的訊號強度，最新的在最後，面板畫成趨勢線
  uint8_t rhN;
};

// 自開機以來每個追蹤器的出現紀錄，給手機面板的警示歷史、時間軸與摘要使用。
// 出現時段以毫秒（開機後）記錄，相鄰的掃描合併成一段
static const uint8_t HIST_MAX_IV = 12;

struct TrackHist {
  std::string addr;
  TrackerType type;
  uint32_t firstMs, lastMs, alertMs;  // alertMs 為 0 代表沒有警示過
  int8_t maxRssi;
  uint16_t scans;
  bool alerted, acked;
  int8_t sound;   // 最近一次發聲的結果（trackersound::Result），-1 代表還沒試過
  bool dult;      // 這個位址收過 DULT 廣播，可能支援 DULT 發聲
  bool connectable;  // 最近一次的廣播接受連線
  uint8_t hint;   // Apple 的子類型（trackersound::Hint）：0 其他、1 AirTag、2 AirPods
  bool autoOff;   // 觸發警示時自動發聲沒有開
  uint8_t nIv;
  uint32_t ivStart[HIST_MAX_IV], ivEnd[HIST_MAX_IV];
  uint16_t pl[6];  // 依序經過的地點編號（連續相同的只記一次），最近的 6 個
  uint8_t nPl;
};

// 自開機以來打的記號，t 是開機後秒數
struct MarkRec {
  uint32_t n, t;
  std::string label;
};

// 發現時自動發聲：每個觸發警示的追蹤器嘗試的次數
struct AutoSoundRec {
  std::string addr;
  uint8_t attempts;
  bool done;  // 已經成功，或追蹤器不支援、明確拒絕，不再嘗試
  bool help;  // 協助周圍的人：不是跟著你的追蹤器，發聲前再確認一次距離
};

// 一組警示門檻，見 pocket_detector.ino 的 PRESETS
struct AlertPreset {
  const char* name;
  uint32_t spanMs;    // 首次與最後一次出現至少相隔多久
  uint16_t windows;   // 訊號達到 rssi 的掃描至少幾次
  int8_t rssi;
  uint8_t presence;   // 從第一次出現起，至少在幾 % 的掃描裡看得到
};

// 藍牙位址的類型。公開位址與隨機固定位址開機後不會改變，
// 可解析的隨機位址（RPA）與不可解析的隨機位址（NRPA）會定期更換
enum AddrKind : uint8_t {
  KIND_PUBLIC = 0,
  KIND_STATIC,
  KIND_RPA,
  KIND_NRPA,
  KIND_COUNT,
};

// 統計模式記錄的每一個裝置
struct DeviceSeen {
  uint8_t addr[6];
  uint8_t kind;
  int8_t rssi;
  uint32_t firstSeen;
  uint32_t lastSeen;
};

// Wi-Fi 基地台的出現紀錄，用來判斷周圍環境有沒有改變（有沒有在移動）
struct ApSeen {
  uint8_t bssid[6];
  uint32_t firstScan;
  uint32_t lastScan;
};

// 人流計數模式記錄的裝置。只存加了鹽值的雜湊，不存原始位址
struct CrowdSeen {
  uint64_t h;
  uint32_t firstScan;
  uint32_t lastScan;
};
