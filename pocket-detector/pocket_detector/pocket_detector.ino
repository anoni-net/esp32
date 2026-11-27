// BLE 追蹤器偵測器（ESP32-S3 原型）
//
// 被動掃描周圍的 BLE 廣播，只記錄符合追蹤器特徵的裝置。
// 同一個追蹤器在一段時間內反覆出現時，亮紅燈並從序列埠輸出警示。
//
// 隱私設計：
//   - 只做被動掃描，不連線、不送出掃描請求
//   - 不符合追蹤器特徵的裝置（手機、耳機、手錶）不記錄
//   - 紀錄只存在記憶體，斷電即消失，不開 WiFi、不上傳
//     （研究用的 WIFI_SCAN 會開 Wi-Fi。長按 BOOT 2 秒才開啟手機連線，用完自動關閉）
//
// 相依（Arduino IDE 的程式庫管理員搜尋名稱安裝）：
//   - NimBLE-Arduino 2.x
//   - U8g2（OLED 顯示，沒接 OLED 也要安裝才能編譯）

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FFat.h>
#include <Preferences.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <U8g2lib.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

#include "i18n.h"
#include "flashlog.h"
#include "types.h"
#include "phonelink.h"
#include "trackersound.h"
#include "webpanel.h"
// 網址更新的兩個通道（lib/netupdate）。已經燒進板子的版本都讀這兩個網址，
// 換到新路徑之前要先讓發布腳本同時發布新舊路徑。每個語系各有自己的通道（publish.env），
// 英文版從網址更新時不會換成正體中文版
#if defined(FW_LANG_ZH_CN)
#define NETUPDATE_URL_STABLE "https://assets.anoni.net/esp32-tracker-zh-cn/"
#define NETUPDATE_URL_TEST "https://assets.anoni.net/esp32-tracker-zh-cn-test/"
#elif defined(FW_LANG_EN)
#define NETUPDATE_URL_STABLE "https://assets.anoni.net/esp32-tracker-en/"
#define NETUPDATE_URL_TEST "https://assets.anoni.net/esp32-tracker-en-test/"
#else
#define NETUPDATE_URL_STABLE "https://assets.anoni.net/esp32-tracker/"
#define NETUPDATE_URL_TEST "https://assets.anoni.net/esp32-tracker-test/"
#endif
#include "netupdate.h"
#include "roomcheck.h"
#include "screen.h"
#include "places.h"
#include "probes.h"
#include "meeting.h"

#include <algorithm>
#include <string>
#include <vector>

// ---- 設定 ----------------------------------------------------------------

// 每次掃描的長度，以及兩次掃描開始的間隔。這裡是預設值，執行中可以在手機面板
// 或序列埠（interval 15、30、60，duration 3、5）調整，設定存在 NVS
static const uint32_t SCAN_DURATION_MS = 5000;
static const uint32_t SCAN_INTERVAL_MS = 30000;

// true 時預設使用快速測試的門檻（2 分鐘、3 次掃描），拿身邊的追蹤器測試警示流程時使用。
// 執行中也可以在手機面板切換門檻，不必重新燒錄
static const bool QUICK_TEST = false;

// true 時進入統計模式：記錄周圍所有藍牙裝置，統計位址類型，以及有多少裝置
// 用同一個位址持續出現。OLED 改顯示統計表，序列埠每次掃描輸出一行 CSV。
// 追蹤器的偵測與警示照常運作。
// 這裡是預設值，執行中可以從序列埠送 stats on、stats off，或在手機面板切換，
// 切換後的設定存在 NVS，重新開機後維持
static const bool STATS_MODE = false;

// 統計模式：同一個位址持續出現超過這段時間，算作「位址固定、一直在附近」。
// 手機的隨機位址大約 15 分鐘更換，門檻設得比它長
static const uint32_t PERSIST_MS = 20UL * 60 * 1000;

// 統計模式：超過這段時間沒再出現就從紀錄移除，同一個裝置之後再出現會重新計時
static const uint32_t STATS_FORGET_MS = 5UL * 60 * 1000;

static const size_t MAX_DEVICES = 512;

// true 時把序列埠的每一行同時寫進 flash 的 FAT 分割區，斷電不會消失，
// 出門實測時用。這裡是預設值，執行中可以送 log on、log off 或在手機面板切換，
// 設定存在 NVS。接上電腦後從序列埠送指令取回：
//   dump   印出整份記錄
//   clear  清空記錄
//   info   記錄的大小與剩餘空間
// Arduino IDE 的 Partition Scheme 要選 16M Flash (3MB APP/9.9MB FATFS)
static const bool LOG_TO_FLASH = false;
static const char* LOG_PATH = "/log.txt";
static const size_t LOG_MAX_BYTES = 8UL * 1024 * 1024;

// true 時進入人流計數模式，用在公共場所量測經過的裝置數量。跟統計模式不同，
// 這個模式不記錄、不輸出任何原始位址：位址加上開機時產生的隨機鹽值後雜湊，
// 鹽值只存在記憶體，重新開機就換掉，之後無法比對回原本的位址。
// 只輸出數量：每次掃描一行 CROWD，每 CROWD_WINDOW_SCANS 次掃描一行 CROWDW。
// 與 STATS_MODE 擇一使用
static const bool CROWD_MODE = false;
static const uint32_t CROWD_WINDOW_SCANS = 10;  // 10 次掃描，約 5 分鐘彙整一次
static const uint32_t CROWD_FORGET_SCANS = 10;  // 10 次掃描沒出現就移除
static const size_t MAX_CROWD = 1024;

// true 時每次藍牙掃描之後，再被動掃描一次周圍的 Wi-Fi 基地台（預設值，執行中可以
// 在手機面板或序列埠 wifiscan on、wifiscan off 切換）。基地台的位置固定，
// 周圍的基地台整批換掉代表你在移動，在室內與地下也有效。被動掃描只收基地台的廣播，
// 不送出探測封包，也不連線。每個基地台輸出一行 AP，每次掃描輸出一行 WIFI
static const bool WIFI_SCAN = false;
static const uint32_t WIFI_MS_PER_CHANNEL = 150;  // 13 個頻道，約 2 秒
static const uint32_t WIFI_FORGET_SCANS = 20;     // 20 次掃描（約 10 分鐘）沒出現就移除
static const size_t MAX_APS = 512;

// 手機連線：長按 BOOT 鍵 LONG_PRESS_MS，Wi-Fi 沒開就打開 Wi-Fi 面板，開著就全部關閉。
// 藍牙的開關、只留藍牙，都在手機面板操作，按鍵不必記得現在是第幾段
// Wi-Fi 是熱點加手機面板：手機連上後自動跳出頁面，可以打 MARK 記號、對時、確認警示、
// 切換統計模式、下載記錄。藍牙是文字通道，用藍牙終端機 App 送指令。
// 兩者都會讓板子對外發出訊號，周圍的裝置收得到，平常保持關閉。
// 熱點與藍牙掃描共用同一組射頻，開著時掃到的裝置可能略少
static const uint32_t LONG_PRESS_MS = 2000;
static const uint32_t DOUBLE_CLICK_MS = 350;  // 放開後這段時間內再按一下算雙擊
// 開啟後這段時間都沒有手機連著，自動全部關閉。設成 0 不自動關閉（預設值，
// 執行中可以在手機面板或序列埠 idle 0、5、10、30 調整）
static const uint32_t LINK_IDLE_OFF_MS = 10UL * 60 * 1000;
// 射頻由 Wi-Fi 與藍牙輪流使用。Wi-Fi 開著時藍牙掃描只用每 100 毫秒裡的這麼多毫秒，
// 手機連入後 JOIN_GRACE_MS 內先不掃描，頁面才跳得出來。追蹤器約每 2 秒廣播一次，
// 5 秒的掃描仍收得到，但記錄裡同一段時間掃到的裝置會略少
static const uint16_t SCAN_WINDOW_WIFI_MS = 30;
static const uint32_t JOIN_GRACE_MS = 15000;
// true 時開機就打開 Wi-Fi 面板，不必長按
static const bool WEB_PANEL = false;
// 熱點與藍牙的預設名稱（後面接 MAC 末四碼）與預設密碼。執行中可以在手機面板或序列埠
// （name、pass）改成不起眼的名稱與自訂密碼，存在 NVS。長按 BOOT 10 秒或序列埠送
// factory reset 恢復原廠設定
static const char* WEB_PANEL_PREFIX = "BLE-Tracker";
static const char* WEB_PANEL_PASS = "anoni-tracker";
// 這份韌體的建置時間，手機面板顯示，更新後用來確認版本換了
// 前面的 TRKBUILD: 讓 tools/publish_firmware.sh 從 firmware.bin 找出建置時間，寫進 latest.json
static const char BUILD_TAG[] = "TRKBUILD:" __DATE__ " " __TIME__;
static const char* BUILD_ID = BUILD_TAG + 9;
// 版本號是建置時的 git commit，由 platformio.ini 帶進來。用 Arduino IDE 建置時沒有
#ifndef FW_REV
#define FW_REV "dev"
#endif
static const char* FW_VERSION = FW_REV;
// 韌體更新：面板按「開放更新」後要在這段時間內短按 BOOT，之後開放上傳的時間
static const uint32_t OTA_ARM_WAIT_MS = 60UL * 1000;
static const uint32_t OTA_OPEN_MS = 5UL * 60 * 1000;
// 自動更新：開機或開啟後多久第一次檢查，之後每 6 小時一次。附近沒有更新用的 Wi-Fi 時
// 30 分鐘後再被動掃描一次，下載失敗 1 小時後再試。正在使用板子時每 30 秒再看一次，
// 只檢查條件，不動射頻。手機離開熱點後很快就開始，iPhone 的個人熱點才能替板子上網
static const uint32_t AUTO_FIRST_MS = 90UL * 1000;
static const uint32_t AUTO_EVERY_MS = 6UL * 3600 * 1000;
static const uint32_t AUTO_NONET_MS = 30UL * 60 * 1000;
static const uint32_t AUTO_FAIL_MS = 60UL * 60 * 1000;
static const uint32_t AUTO_DEFER_MS = 30UL * 1000;
static const uint8_t AUTO_DEFER_PCT = 50;  // 有追蹤器離警示過半時先不重新開機，累計會歸零
// 長按 BOOT 這麼久後放開，恢復原廠設定。按著 BOOT 重新開機會進入燒錄模式，所以等放開才執行
static const uint32_t FACTORY_PRESS_MS = 10000;

// 判定為「一直跟著你」的門檻，三個條件都達到才警示：
//   spanMs    首次與最後一次出現相隔至少這麼久
//   windows   訊號達到 rssi 的掃描至少這麼多次。放在身上或包包裡的追蹤器通常在
//             -40 到 -70 dBm，隔牆或在路口的大約 -90 dBm 以下
//   presence  從第一次出現起，至少在這個比例的掃描裡看得到。跟著你的追蹤器幾乎每次都
//             看得到，時有時無的多半是附近經過或隔壁的
// 執行中可以在手機面板或序列埠（preset 0 到 3）切換，設定存在 NVS
static const AlertPreset PRESETS[] = {
    {"sensitive", 5UL * 60 * 1000, 4, -90, 60},      // 敏感
    {"standard", 10UL * 60 * 1000, 6, -85, 70},      // 標準
    {"conservative", 15UL * 60 * 1000, 9, -80, 80},  // 保守
    {"quick", 2UL * 60 * 1000, 3, -85, 70},          // 快速測試
};
static const uint8_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);
static const uint8_t DEFAULT_PRESET = QUICK_TEST ? 3 : 1;

// 超過這段時間沒再出現就從紀錄移除
static const uint32_t FORGET_AFTER_MS = 30UL * 60 * 1000;

static const size_t MAX_TRACKED = 160;

// Wi-Fi 移動判斷：這次掃描到的基地台，有不到這個比例出現在目前地點看過的基地台裡，
// 就算換了地點。基地台少於 PLACE_MIN_APS 個時（地下、郊外）不判斷。
// 打開 Wi-Fi 基地台掃描時，追蹤器要在至少 MOVE_MIN_PLACES 個不同地點都出現才警示，
// 停留在同一個地方時，周圍的追蹤器不會再造成誤報。2026-11-24 A 板的記錄：兩個長時間
// 停留的地方各約 800 次掃描都沒有誤判，移動途中判斷出約 28 次換地點
static const uint8_t PLACE_MIN_OVERLAP_PCT = 30;
static const uint8_t PLACE_MIN_APS = 3;
static const uint8_t MOVE_MIN_PLACES = 3;

// ---- 執行中可調整的設定（存在 NVS，開機時讀回） -------------------------

static uint8_t presetIdx = DEFAULT_PRESET;
static uint32_t alertSpanMs = PRESETS[DEFAULT_PRESET].spanMs;
static uint16_t alertMinWindows = PRESETS[DEFAULT_PRESET].windows;
static int alertMinRssi = PRESETS[DEFAULT_PRESET].rssi;
static uint8_t alertMinPresence = PRESETS[DEFAULT_PRESET].presence;
static uint32_t scanDurationMs = SCAN_DURATION_MS;
static uint32_t scanIntervalMs = SCAN_INTERVAL_MS;
static uint32_t linkIdleMs = LINK_IDLE_OFF_MS;
static bool wifiScanOn = WIFI_SCAN;
// 從網址更新韌體時連線的 Wi-Fi，存在 NVS。netPending：等這一輪掃描結束再開始下載
static String netSsid, netPass;
static bool netPending = false;
// 自動更新，預設關閉。開啟後從網址更新不必按 BOOT，上傳檔案仍要按
static bool autoUpdate = false;
static bool netAuto = false;      // 這一次檢查是自動更新發起的
static bool netLastAuto = false;  // 面板顯示的上一次結果是不是自動檢查
static uint32_t netDoneAt = 0;    // 上一次檢查結束的時間
static uint32_t autoNextAt = 0;   // 下一次自動檢查的時間
static bool autoNonetLogged = false;  // 附近沒有 Wi-Fi 連續發生時只記一次
static String autoSkip;           // 自動安裝後沒有正常開機的建置時間
static String autoFrom;           // 這次開機是自動更新上來的，更新前的版本
static bool testChannel = false;  // 從測試通道（esp32-tracker-test/）更新，預設正式通道

// 測試用：序列埠送 fake on 後，每次掃描加入一個假的追蹤器，不必帶真的追蹤器
// 就能測試警示、面板與尋找模式。記錄裡會寫一行 FAKE 標明
static bool fakeTracker = false;
static const char* FAKE_ADDR = "fa:ke:00:00:00:01";

// true 時把每個符合特徵的廣播原始內容印出來，實測新型號時使用
static const bool VERBOSE = true;

// 板上 RGB LED 的腳位。多數 N16R8 相容板接 GPIO48，v1.1 版的 DevKitC 接 GPIO38，
// 沒有 RGB LED 就設成 -1
static const int RGB_LED_GPIO = 48;

// 外接蜂鳴器的腳位，沒接時維持原設定也不影響運作，設成 -1 則完全不使用。
// 程式寫給有源蜂鳴器：給觸發電位就響，不必產生頻率。
// 低電位觸發的模組把 BUZZER_ON 改成 LOW
static const int BUZZER_GPIO = 4;
static const uint8_t BUZZER_ON = HIGH;

// false 時蜂鳴器不響，只用 LED 的燈號提示，適合需要安靜、不想讓旁人察覺的場合。
// 開機那一聲也一起關掉
// 這裡是預設值，執行中可以在手機面板切換安靜模式，設定存在 NVS
static const bool BUZZER_ENABLED = true;

// 0.96 吋 SSD1306 OLED 的 I2C 腳位。開機時偵測，沒接就略過，不影響其他功能。
// OLED 的 VCC 一律接 3.3V：模組的 I2C 上拉接在自己的 VCC，接 5V 會把
// SDA 與 SCL 拉到 5V，ESP32-S3 的腳位不耐 5V
static const int OLED_SDA_GPIO = 8;
static const int OLED_SCL_GPIO = 9;

// 板上的 BOOT 按鈕，按下代表「已確認」，關掉目前的警示
static const int BUTTON_GPIO = 0;

// ---- 追蹤器的識別 --------------------------------------------------------

static const char* trackerName(TrackerType t) {
  switch (t) {
    case TrackerType::AppleFindMySeparated: return "Apple Find My（與主人分開）";
    case TrackerType::SamsungSmartTag:      return "Samsung SmartTag";
    case TrackerType::Tile:                 return "Tile";
    case TrackerType::GoogleFindHub:        return "Google 尋找我的裝置";
    case TrackerType::Dult:                 return "DULT 追蹤器（與主人分開）";
    case TrackerType::AppleDevice:          return "Apple 自家裝置（iPhone、iPad、Mac，與主人分開）";
    case TrackerType::Chipolo:              return "Chipolo";
    case TrackerType::Pebblebee:            return "Pebblebee";
    default:                                return "未知";
  }
}

// 藍牙文字通道顯示的追蹤器類型，會翻譯。序列埠用上面的 trackerName，維持原文
static const char* trackerNameUi(TrackerType t) {
  switch (t) {
    case TrackerType::AppleFindMySeparated: return TR("Apple Find My（與主人分開）");
    case TrackerType::SamsungSmartTag:      return "Samsung SmartTag";
    case TrackerType::Tile:                 return "Tile";
    case TrackerType::GoogleFindHub:        return TR("Google 尋找我的裝置");
    case TrackerType::Dult:                 return TR("DULT 追蹤器（與主人分開）");
    case TrackerType::AppleDevice:          return TR("Apple 自家裝置（iPhone、iPad、Mac，與主人分開）");
    case TrackerType::Chipolo:              return "Chipolo";
    case TrackerType::Pebblebee:            return "Pebblebee";
    default:                                return TR("未知");
  }
}

static const NimBLEUUID UUID_SAMSUNG((uint16_t)0xFD5A);
static const NimBLEUUID UUID_TILE((uint16_t)0xFEED);
static const NimBLEUUID UUID_GOOGLE_FHN((uint16_t)0xFEAA);
static const NimBLEUUID UUID_TILE_2((uint16_t)0xFEEC);
static const NimBLEUUID UUID_CHIPOLO((uint16_t)0xFE33);
static const NimBLEUUID UUID_PEBBLEBEE((uint16_t)0xFA25);
// DULT 的 Service Data UUID。內容第 1 個 byte 是尋找網路的編號，第 2 個 byte 的最低位元
// 是「主人在附近」（1 在附近、0 與主人分開），之後是各家的資料。
// 依據 draft-ietf-dult-accessory-protocol-01（2026-10-27）的 Table 1 與 3.9 節
static const NimBLEUUID UUID_DULT((uint16_t)0xFCB2);

// DULT 廣播的內容，不是 DULT 或長度不足時回傳 false
static bool parseDult(const NimBLEAdvertisedDevice* d, uint8_t& net, bool& nearOwner) {
  for (uint8_t i = 0; i < d->getServiceDataCount(); i++) {
    if (d->getServiceDataUUID(i) != UUID_DULT) continue;
    const std::string s = d->getServiceData(i);
    if (s.size() < 2) return false;
    net = (uint8_t)s[0];
    nearOwner = ((uint8_t)s[1] & 0x01) != 0;
    return true;
  }
  return false;
}

// ---- DULT 廣播的逐筆紀錄 --------------------------------------------------
//
// 一次掃描裡，NimBLE 對同一個位址只留下最後一則廣播。支援 DULT 的追蹤器輪流發送自家格式
// 與 DULT 格式，為了兩種都看得到，掃描的回呼逐筆檢查每一則廣播，記下 DULT 的內容

static const size_t DULT_MAX = 64;
static const uint32_t DULT_REMEMBER_MS = 15UL * 60 * 1000;  // 與規格裡主人在附近時的換位址週期相同
static DultSeen dultSeen[DULT_MAX];
static portMUX_TYPE dultLock = portMUX_INITIALIZER_UNLOCKED;

class DultScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    uint8_t net;
    bool nearOwner;
    if (!parseDult(d, net, nearOwner)) return;
    const uint8_t* a = d->getAddress().getVal();
    const uint32_t now = millis();
    portENTER_CRITICAL(&dultLock);
    size_t slot = 0;
    for (size_t i = 0; i < DULT_MAX; i++) {
      if (memcmp(dultSeen[i].addr, a, 6) == 0) { slot = i; break; }
      if (dultSeen[i].ms < dultSeen[slot].ms) slot = i;  // 沒找到就用最舊的位置
    }
    memcpy(dultSeen[slot].addr, a, 6);
    dultSeen[slot].ms = now ? now : 1;
    dultSeen[slot].net = net;
    dultSeen[slot].nearOwner = nearOwner;
    portEXIT_CRITICAL(&dultLock);
  }
};

static bool dultLookup(const NimBLEAddress& addr, DultSeen& out, uint32_t now) {
  const uint8_t* a = addr.getVal();
  bool found = false;
  portENTER_CRITICAL(&dultLock);
  for (size_t i = 0; i < DULT_MAX; i++) {
    if (dultSeen[i].ms && memcmp(dultSeen[i].addr, a, 6) == 0 && now - dultSeen[i].ms < DULT_REMEMBER_MS) {
      out = dultSeen[i];
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&dultLock);
  return found;
}

// 識別規則參考公開的研究、AirGuard 專案、Kismet 的整理與 DULT 規格草案，各型號仍需實測確認
static TrackerType classify(const NimBLEAdvertisedDevice* d) {
  if (d->haveManufacturerData()) {
    const std::string m = d->getManufacturerData();
    // Apple 的公司代碼 0x004C，類型 0x12 是 Find My 的離線尋找廣播。
    // 長度欄位 0x19 代表與主人分開，0x02 代表主人就在附近（不警示）
    if (m.size() >= 4 && (uint8_t)m[0] == 0x4C && (uint8_t)m[1] == 0x00 &&
        (uint8_t)m[2] == 0x12) {
      if ((uint8_t)m[3] != 0x19) return TrackerType::None;
      // 狀態 byte 的位元 4、5 是裝置類型，0 是 Apple 自家裝置（PETS 2023〈Track You〉表 4）
      if (m.size() >= 5 && (((uint8_t)m[4] >> 4) & 0x03) == 0) return TrackerType::AppleDevice;
      return TrackerType::AppleFindMySeparated;
    }
  }

  const uint8_t n = d->getServiceDataCount();
  for (uint8_t i = 0; i < n; i++) {
    const NimBLEUUID u = d->getServiceDataUUID(i);
    if (u == UUID_SAMSUNG) return TrackerType::SamsungSmartTag;
    if (u == UUID_TILE || u == UUID_TILE_2) return TrackerType::Tile;
    if (u == UUID_CHIPOLO) return TrackerType::Chipolo;
    if (u == UUID_DULT) {
      const std::string s = d->getServiceData(i);
      if (s.size() >= 2 && ((uint8_t)s[1] & 0x01) == 0) return TrackerType::Dult;
    }
    if (u == UUID_GOOGLE_FHN) {
      // 0xFEAA 也是 Eddystone 信標，只認尋找我的裝置網路的 frame type
      const std::string s = d->getServiceData(i);
      if (!s.empty() && ((uint8_t)s[0] == 0x40 || (uint8_t)s[0] == 0x41)) {
        return TrackerType::GoogleFindHub;
      }
    }
  }

  if (d->isAdvertisingService(UUID_TILE)) return TrackerType::Tile;
  if (d->isAdvertisingService(UUID_PEBBLEBEE)) return TrackerType::Pebblebee;
  return TrackerType::None;
}

// ---- 出現紀錄 ------------------------------------------------------------

static std::vector<Sighting> table;
static std::vector<DeviceSeen> devices;
static uint32_t nowCount[KIND_COUNT];
static uint32_t persistCount[KIND_COUNT];
static uint32_t totalCount[KIND_COUNT];
static bool statsMode = STATS_MODE;  // 目前是否在統計模式，執行中可切換
static Preferences prefs;
static NimBLEScan* scan = nullptr;
static uint32_t lastScanAt = 0;
static uint32_t scanCount = 0;
static uint16_t placeId = 0;              // 目前所在的地點編號，0 代表還沒掃描過 Wi-Fi
static uint32_t lastPlaceChangeScan = 0;  // 最近一次換地點是第幾次掃描
// 記憶體記錄：每 10 分鐘、開機與每次網址更新開始時寫一行 MEM，找出哪個功能吃記憶體、跑多久開始切碎
static const uint32_t MEM_LOG_MS = 10UL * 60 * 1000;
static uint32_t memLoggedAt = 0;
// 周圍手機洩漏的 Wi-Fi 名稱（probes.h），程式在「周圍手機洩漏的 Wi-Fi 名稱」一節
static const uint32_t PROBE_MS = 5UL * 60 * 1000;
// 環形緩衝區存原始的封包（最多 PROBE_RAW bytes），loop() 再解析名稱、指紋與型號，回呼裡不配置記憶體。
// 64 × 384 bytes 約 24 KB，第一次開始監聽時放在 PSRAM
struct ProbeRec {
  uint16_t len;
  int8_t rssi;
};
static const uint8_t PROBE_RING = 64;
static const uint16_t PROBE_RAW = 384;
static ProbeRec probeRing[PROBE_RING];
static uint8_t* probeRaw = nullptr;
static volatile uint8_t probeHead = 0, probeTail = 0;
static volatile uint32_t probeDrop = 0;
static portMUX_TYPE probeLock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool probeOn = false;
static probes::Survey probeSurvey;
static uint32_t probeStartAt = 0, probeStopAt = 0, probePubAt = 0, probeVer = 0;
static uint8_t probeCh = 0;
static bool probeOwnMode = false;  // 熱點沒開，為了監聽才打開 Station 模式
static wifi_mode_t probePrevMode = WIFI_MODE_NULL;

// 會議室檢查（meeting.h），程式在「會議室檢查」一節
static const uint32_t MEET_SCAN_MS = 10000;  // 檢查期間藍牙每 10 秒掃描一次
static meeting::Watch meet;
static uint32_t meetSalt = 0, meetVer = 0, meetPubAt = 0;
static bool meetOwnProbes = false;  // 監聽探測請求是會議室檢查打開的
static bool meetWasAlert = false;
static uint16_t bleNowCount = 0;  // 上一次掃描收到幾個藍牙裝置（只計數，不記位址），面板顯示周圍環境
static std::vector<uint64_t> placeAps;    // 目前地點看過的基地台（最多 200 個）
// 地點指紋（places.h）：回到存過的地點時沿用同一個編號，使用者可以命名。存在 flash
static places::Book placeBook;
static const char* PLACES_PATH = "/places.txt";
static const uint32_t PLACES_SAVE_MS = 10UL * 60 * 1000;
static uint32_t placesSavedAt = 0;

// 最近 10 次出現時的訊號強度
static void noteRssi(Sighting& s, int rssi) {
  const int8_t v = (int8_t)constrain(rssi, -127, 0);
  if (s.rhN < 10) {
    s.rh[s.rhN++] = v;
  } else {
    memmove(s.rh, s.rh + 1, 9);
    s.rh[9] = v;
  }
}

// 追蹤器出現過的地點，回到同一個地點不重複計算
static void notePlace(Sighting& s) {
  if (!placeId) return;
  s.lastPlace = placeId;
  for (uint8_t i = 0; i < s.nPl; i++) {
    if (s.seenPl[i] == placeId) return;
  }
  if (s.nPl < 8) {
    s.seenPl[s.nPl++] = placeId;
  } else {
    memmove(s.seenPl, s.seenPl + 1, sizeof(s.seenPl[0]) * 7);
    s.seenPl[7] = placeId;
  }
  if (s.places < 255) s.places++;
}

static void recordSighting(const std::string& addr, TrackerType type, int rssi,
                           uint32_t now) {
  const uint16_t near = rssi >= alertMinRssi ? 1 : 0;
  for (auto& s : table) {
    if (s.addr == addr) {
      s.lastSeen = now;
      s.windows++;
      s.nearWindows += near;
      s.rssi = rssi;
      noteRssi(s, rssi);
      notePlace(s);
      return;
    }
  }

  Sighting s{addr, type, now, now, 1, near, scanCount, rssi, false, false, placeId, 0};
  noteRssi(s, rssi);
  notePlace(s);
  if (table.size() < MAX_TRACKED) {
    table.push_back(s);
    return;
  }
  // 滿了就取代最久沒出現的那筆，先從 Apple 自家裝置裡找，避免真正的追蹤器被擠掉
  int oldest = -1;
  for (int pass = 0; pass < 2 && oldest < 0; pass++) {
    for (size_t i = 0; i < table.size(); i++) {
      if (pass == 0 && table[i].type != TrackerType::AppleDevice) continue;
      if (oldest < 0 || table[i].lastSeen < table[oldest].lastSeen) oldest = i;
    }
  }
  table[oldest] = s;
}

// ---- 警示歷史 ------------------------------------------------------------
//
// 追蹤器 30 分鐘沒出現就從 table 移除，警示也跟著消失。這裡另外保留自開機以來
// 每個追蹤器的出現時段，給手機面板的警示歷史、時間軸與摘要使用，重新開機後清除。
// 人流計數模式承諾不保留原始位址，所以不記錄

static const size_t HIST_MAX = 24;
static const size_t MARKS_MAX = 60;
static std::vector<TrackHist> history;
static std::vector<MarkRec> allMarks;
static bool histDirty = true;

// 警示歷史存在 flash，重新開機（例如接上電腦讀資料）後還看得到。這次開機的存在 HIST_CUR，
// 開機時移進 HIST_PREV（之前每一次開機的 JSON 陣列，新的在前）
static const char* HIST_CUR = "/hist_cur.json";
static const char* HIST_CUR_TMP = "/hist_cur.json.tmp";
static const char* HIST_PREV = "/hist_prev.json";
static const size_t HIST_KEEP = 8;               // 最多保留幾次開機
static const size_t HIST_PREV_MAX = 96 * 1024;   // 之前的紀錄合計的上限
static const uint32_t HIST_SAVE_MS = 5UL * 60 * 1000;  // 只有出現時段變長時，最久隔這麼久存一次
static uint32_t histSavedAt = 0, histSavedSig = 0;

static TrackHist* findHistory(const std::string& addr) {
  for (auto& h : history) {
    if (h.addr == addr) return &h;
  }
  return nullptr;
}

// 歷史記錄裡依序經過的地點，連續相同的只記一次
static void notePath(TrackHist& h) {
  if (!placeId || (h.nPl && h.pl[h.nPl - 1] == placeId)) return;
  if (h.nPl == 6) {
    memmove(h.pl, h.pl + 1, sizeof(h.pl[0]) * 5);
    h.nPl--;
  }
  h.pl[h.nPl++] = placeId;
}

static void updateHistory(const std::string& addr, TrackerType type, int rssi, uint32_t now) {
  if (CROWD_MODE) return;
  histDirty = true;
  // 相隔不到 2.5 次掃描間隔算同一段，中間漏掉一兩次掃描不會被切開
  const uint32_t gap = scanIntervalMs * 5 / 2;
  if (TrackHist* h = findHistory(addr)) {
    h->lastMs = now;
    h->scans++;
    notePath(*h);
    if (rssi > h->maxRssi) h->maxRssi = rssi;
    if (h->nIv && now - h->ivEnd[h->nIv - 1] <= gap) {
      h->ivEnd[h->nIv - 1] = now;
      return;
    }
    if (h->nIv == HIST_MAX_IV) {
      // 時段用完時把最早的兩段合併
      h->ivEnd[0] = h->ivEnd[1];
      for (uint8_t i = 1; i + 1 < HIST_MAX_IV; i++) {
        h->ivStart[i] = h->ivStart[i + 1];
        h->ivEnd[i] = h->ivEnd[i + 1];
      }
      h->nIv--;
    }
    h->ivStart[h->nIv] = h->ivEnd[h->nIv] = now;
    h->nIv++;
    return;
  }

  TrackHist h{};
  h.addr = addr;
  h.type = type;
  h.firstMs = h.lastMs = now;
  h.maxRssi = rssi;
  h.scans = 1;
  h.sound = -1;
  h.nIv = 1;
  h.ivStart[0] = h.ivEnd[0] = now;
  notePath(h);
  if (history.size() >= HIST_MAX) {
    // 滿了就移除最久沒出現、沒有警示過的那筆，全部都警示過才移除最舊的
    int victim = -1;
    for (size_t i = 0; i < history.size(); i++) {
      if (history[i].alerted) continue;
      if (victim < 0 || history[i].lastMs < history[victim].lastMs) victim = i;
    }
    if (victim < 0) {
      victim = 0;
      for (size_t i = 1; i < history.size(); i++) {
        if (history[i].lastMs < history[victim].lastMs) victim = i;
      }
    }
    history.erase(history.begin() + victim);
  }
  history.push_back(h);
}

static void forgetStale(uint32_t now) {
  for (size_t i = 0; i < table.size();) {
    if (now - table[i].lastSeen > FORGET_AFTER_MS) {
      table.erase(table.begin() + i);
    } else {
      i++;
    }
  }
}

// ---- flash 記錄 ----------------------------------------------------------

static Tee out;
static bool logMounted = false;  // FAT 分割區已掛載
static bool logReady = false;    // 正在寫入 flash 記錄
static bool logFull = false;
static bool buzzerOn = BUZZER_ENABLED;  // false 時為安靜模式
static uint32_t markCount = 0;

// 手機連線的狀態
static bool linkWifi = false, linkBle = false;
static String deviceName, devicePass;  // 熱點與藍牙的名稱、熱點密碼
static bool ledOn = true;              // false 時燈號全關
static uint32_t blePin = 0;            // 藍牙配對碼，第一次使用時隨機產生，存在 NVS
static uint32_t otaWaitUntil = 0;      // 等待使用者短按 BOOT 的期限，0 代表沒在等
// 等 BOOT 確認的是哪件事：0 開放更新韌體、1 打開自動更新、2 打開接收測試版。
// 後兩個會讓板子自己換上網路上的韌體，知道熱點密碼的人不能只靠面板打開
static uint8_t bootWhat = 0;
// 掃描在背景進行，loop() 不會停住，按鍵與面板隨時有反應
static bool scanRunning = false, scanIsFind = false;
// 按鍵與切換的燈號回饋
static uint32_t buttonPressedAt = 0;   // 按下 BOOT 的時間，放開時歸零
static uint32_t linkFlashUntil = 0;    // 切換連線後，用新狀態的顏色恆亮到這個時間
static uint8_t linkFlashR = 0, linkFlashG = 0, linkFlashB = 0;
// 讓追蹤器發出聲音的最近一次結果
static bool soundBusy = false;
static std::string soundAddr;
static String soundMsg;
static uint32_t soundAt = 0;
// 發現時自動發聲：觸發警示時自動走 DULT 發聲流程。預設關閉，存在 NVS
// 自動發聲：0 關閉（只能手動）、1 跟著我移動時（要開移動判斷）、2 也協助周圍的人
// 2026-11-26 實測，敏感門檻又沒開移動判斷時，辦公室裡不動的 DULT 追蹤器觸發警示，自動響了兩次，
// 旁人聽到也不知道是誰的。所以只有確認一直跟著你（經過好幾個地點）才自動發聲。
// 協助周圍的人要自己打開：DULT 追蹤器與主人分開、在很近的距離出現一段時間，也讓它響一次
static uint8_t autoMode = 0;
static const int HELP_RSSI = -70;                      // 協助周圍的人：訊號要強於這個值
static const uint32_t HELP_SPAN_MS = 10UL * 60 * 1000;  // 協助周圍的人：至少出現這麼久
static const uint8_t AUTO_SOUND_TRIES = 3;
static std::vector<AutoSoundRec> autoSoundList;
static uint32_t linkLastActive = 0;  // 最後一次有手機連著，或剛開啟的時間

// 尋找模式：持續做 1 秒的短掃描，回報指定追蹤器的訊號強度
static const uint32_t FIND_TIMEOUT_MS = 5UL * 60 * 1000;
static bool findOn = false;
static std::string findAddr;
static int findRssi = 0;
static uint32_t findSeenAt = 0, findStartAt = 0;
static uint32_t findTickAt = 0;  // 尋找時最近一次嗶聲（與燈號同步）
// 尋找房間檢查找到的裝置：顯示名稱不顯示位址。Wi-Fi 裝置改用 Wi-Fi 監聽取得訊號
static String findLabel;
static bool findQuiet = false;  // startFind() 不寫記錄
static bool findWifi = false;
static uint8_t findWifiMac[6], findWifiCh = 0, findWifiStage = 0;
static uint32_t findWifiAt = 0;
static wifi_mode_t findWifiPrevMode = WIFI_MODE_NULL;
static uint32_t oledHoldUntil = 0;  // oled demo 的畫面展示到這個時間

// 房間檢查（roomcheck.h）。藍牙主動掃描 → 關熱點 → Wi-Fi 監聽 → 恢復，期間暫停追蹤器偵測
static bool roomPending = false;    // 等目前的掃描結束再開始
static uint8_t roomStage = 0;       // 0 沒在檢查、1 藍牙掃描、2 等熱點關閉、3 Wi-Fi 監聽
static uint32_t roomStageAt = 0;
static bool roomHadWifi = false;    // 開始時熱點開著，結束後重新打開
static uint32_t roomDoneUntil = 0;  // 檢查完成後燈號綠色連閃到這個時間
static bool roomLog = false;        // 把房間檢查收到的 MAC 位址寫進 flash 記錄，預設關閉
static uint32_t roomVer = 0;

static size_t logSize() {
  if (!logMounted) return 0;
  File f = FFat.open(LOG_PATH, FILE_READ);
  if (!f) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

// 掛載 FAT 分割區。第一次使用時分割區還沒格式化，format 為 true 時會自動格式化
static bool mountLog(bool format) {
  if (logMounted) return true;
  if (!FFat.begin(format)) return false;
  logMounted = true;
  return true;
}

// 記錄暫停時也先試著掛載（不格式化），舊的記錄仍可下載
static void setupFlashLog(bool enabled) {
  if (!mountLog(enabled)) {
    if (enabled) Serial.println("flash 記錄：無法掛載 FAT 分割區，確認 Partition Scheme 有 FATFS");
    return;
  }
  if (!enabled) return;
  logReady = true;
  logFull = logSize() >= LOG_MAX_BYTES;
  out.captureToBuffer(!logFull);
}

// 每次掃描完呼叫，把暫存的輸出附加到記錄檔
static void flushLog() {
  std::string& b = out.buffer();
  if (b.empty()) return;
  if (logReady && !logFull) {
    File f = FFat.open(LOG_PATH, FILE_APPEND);
    if (f) {
      f.write((const uint8_t*)b.data(), b.size());
      const size_t n = f.size();
      f.close();
      if (n >= LOG_MAX_BYTES) {
        logFull = true;
        out.captureToBuffer(false);
        Serial.println("flash 記錄：已達上限，停止寫入");
      }
    }
  }
  b.clear();
}

static void dumpLog() {
  flushLog();
  Serial.println("=== FLASH LOG BEGIN ===");
  File f = logMounted ? FFat.open(LOG_PATH, FILE_READ) : File();
  if (f) {
    uint8_t chunk[512];
    size_t n;
    while ((n = f.read(chunk, sizeof(chunk))) > 0) Serial.write(chunk, n);
    f.close();
  }
  Serial.println("=== FLASH LOG END ===");
}

static void setStatsMode(bool on);
static void changePlace(const std::vector<uint64_t>& current, int overlapPct, const std::vector<uint32_t>& keys);
static void printCfg();
static void setIdentity(const String& name, const String& pw);
static void setLedOn(bool on);
static void factoryReset();
static void otaSelfTest();
static void setNetWifi(const String& ssid, const String& pass);
static bool requestNetUpdate(bool needArm, bool automatic);
static void setAutoUpdate(bool on);
static void setTestChannel(bool test);
static bool startProbes();
static void stopProbes();
static void drainProbes();
static bool startMeeting(int threshold);
static void stopMeeting();
static void pollNetUpdate();
static void startFind(const char* addr);
static void newBlePin();
static void unpairAll();
static trackersound::Result soundTracker(const std::string& addr);
static void soundTest();
static void publishSnapshot();
static void publishHistory();
static void markEvent(const char* label, uint32_t atMs);
static bool acknowledgeAlerts();
static void setPreset(uint8_t idx);
static void setScanTiming(uint32_t intervalSec, uint32_t durSec);
static void setIdle(uint32_t minutes);
static void setWifiScan(bool on);
static void setAutoSound(uint8_t mode);
static void setLogEnabled(bool on);
static void setQuiet(bool quiet);
static void cycleLink();
static void applyLink(bool wifi, bool ble);
static void printStatsHeader();
static String wifiQrText();
static void pollScreen(bool force);
static void toggleFind();
static bool requestRoomCheck();
static void cancelRoomCheck();
static void printRoomReport(bool toLog);
static void setRoomLog(bool on);

// 刪除 flash 記錄。統計模式下補寫一行欄位說明，新的記錄檔才能直接分析
static void clearLog() {
  out.buffer().clear();
  if (logMounted) FFat.remove(LOG_PATH);
  logFull = false;
  out.captureToBuffer(logReady);
  Serial.println("flash 記錄：已清空");
  if (statsMode) printStatsHeader();
  flushLog();
}

static void handleCommand(const String& cmd) {
  if (cmd == "dump") {
    dumpLog();
  } else if (cmd == "clear") {
    clearLog();
  } else if (cmd.startsWith("time ")) {
    const long epoch = cmd.substring(5).toInt();
    if (epoch > 1700000000L) setTime((uint32_t)epoch);
  } else if (cmd == "link") {
    cycleLink();
  } else if (cmd == "link off") {
    applyLink(false, false);
  } else if (cmd == "link wifi" || cmd == "link ble" || cmd == "link both") {
    applyLink(cmd != "link ble", cmd != "link wifi");
  } else if (cmd == "json") {
    publishSnapshot();
    Serial.println(webpanel::statusJson());
  } else if (cmd.startsWith("name ")) {
    String n = cmd.substring(5);
    n.trim();
    if (webpanel::validName(n)) setIdentity(n, "");
    else Serial.println("名稱最多 29 bytes，不能有控制字元");
  } else if (cmd.startsWith("pass ")) {
    const String p = cmd.substring(5);
    if (webpanel::validPass(p)) setIdentity("", p);
    else Serial.println("密碼要 8 到 63 個可列印的 ASCII 字元");
  } else if (cmd == "led on" || cmd == "led off") {
    setLedOn(cmd == "led on");
  } else if (cmd == "soundtest") {
    soundTest();
  } else if (cmd.startsWith("sound ")) {
    soundTracker(cmd.substring(6).c_str());
    Serial.println(soundMsg);
  } else if (cmd == "blepin") {
    Serial.printf("藍牙配對碼：%06lu，已配對 %d 支手機\n", (unsigned long)blePin,
                  phonelink::server ? phonelink::bondCount() : 0);
  } else if (cmd == "blepin new") {
    newBlePin();
  } else if (cmd == "ble unpair") {
    unpairAll();
  } else if (cmd == "ota selftest") {
    otaSelfTest();
  } else if (cmd.startsWith("netwifi ")) {
    setNetWifi(cmd.substring(8), netPass);
  } else if (cmd.startsWith("netpass ")) {
    setNetWifi(netSsid, cmd.substring(8));
  } else if (cmd == "netupdate") {
    if (!requestNetUpdate(false, false)) Serial.println(netSsid.length() ? "正在更新中" : "先用 netwifi 設定可以上網的 Wi-Fi");
  } else if (cmd == "autoupdate on" || cmd == "autoupdate off") {
    setAutoUpdate(cmd == "autoupdate on");
  } else if (cmd == "autoupdate") {
    Serial.printf("自動更新：%s，更新用的 Wi-Fi：%s，下次檢查約 %ld 秒後\n", autoUpdate ? "開" : "關",
                  netSsid.length() ? netSsid.c_str() : "（未設定）",
                  autoUpdate ? (long)((int32_t)(autoNextAt - millis()) / 1000) : -1L);
  } else if (cmd == "channel test" || cmd == "channel stable") {
    setTestChannel(cmd == "channel test");
  } else if (cmd == "channel") {
    Serial.printf("更新通道：%s（%s）\n", testChannel ? "測試" : "正式", netupdate::BASE_URL);
  } else if (cmd == "autoupdate now") {
    autoNextAt = millis();  // 照自動更新的條件檢查一次，延後的條件仍然有效
  } else if (cmd == "factory reset") {
    factoryReset();
  } else if (cmd == "places") {
    printPlaces();
  } else if (cmd.startsWith("place name ")) {
    // place name 編號 名稱，編號 0 代表目前所在的地點
    const String rest = cmd.substring(11);
    const int sp = rest.indexOf(' ');
    if (sp < 0 || !renamePlace(rest.substring(0, sp).toInt(), rest.substring(sp + 1))) {
      Serial.println("用法：place name 編號 名稱（編號 0 是目前的地點，名稱最多 30 bytes）");
    }
  } else if (cmd.startsWith("place forget ")) {
    if (!forgetPlace(cmd.substring(13).toInt())) Serial.println("沒有這個地點");
  } else if (cmd == "place next") {
    changePlace(placeAps, -1, std::vector<uint32_t>());  // 測試用：當作換了地點
  } else if (cmd == "history prev") {
    Serial.println(logMounted ? readFileText(HIST_PREV) : String(""));
  } else if (cmd == "history clear") {
    clearHistoryArchive();
  } else if (cmd == "history") {
    histDirty = true;
    publishHistory();
    Serial.println(webpanel::histJson);
  } else if (cmd == "mark" || cmd.startsWith("mark ")) {
    markEvent(cmd.length() > 5 ? cmd.substring(5).c_str() : "", 0);
  } else if (cmd == "ack") {
    Serial.println(acknowledgeAlerts() ? "已確認警示" : "目前沒有警示");
  } else if (cmd == "fake on" || cmd == "fake off") {
    fakeTracker = cmd == "fake on";
    out.printf("FAKE,%lu,%s,%s\n", (unsigned long)scanCount, fakeTracker ? "on" : "off", FAKE_ADDR);
  } else if (cmd == "cfg") {
    printCfg();
  } else if (cmd.startsWith("preset ")) {
    setPreset(cmd.substring(7).toInt());
  } else if (cmd.startsWith("interval ")) {
    setScanTiming(cmd.substring(9).toInt(), scanDurationMs / 1000);
  } else if (cmd.startsWith("duration ")) {
    setScanTiming(scanIntervalMs / 1000, cmd.substring(9).toInt());
  } else if (cmd.startsWith("idle ")) {
    setIdle(cmd.substring(5).toInt());
  } else if (cmd == "autosound on" || cmd == "autosound off") {
    setAutoSound(cmd == "autosound on" ? 1 : 0);
  } else if (cmd == "autosound 0" || cmd == "autosound 1" || cmd == "autosound 2") {
    setAutoSound(cmd[10] - '0');
  } else if (cmd == "wifiscan on" || cmd == "wifiscan off") {
    setWifiScan(cmd == "wifiscan on");
  } else if (cmd == "captive on" || cmd == "captive off") {
    webpanel::setCaptive(cmd == "captive on");
    prefs.putBool("captive", webpanel::captive);
    Serial.printf("自動跳出面板：%s\n", webpanel::captive ? "開啟" : "關閉");
  } else if (cmd == "log on" || cmd == "log off") {
    setLogEnabled(cmd == "log on");
  } else if (cmd == "quiet on" || cmd == "quiet off") {
    setQuiet(cmd == "quiet on");
  } else if (cmd == "stats on") {
    setStatsMode(true);
  } else if (cmd == "stats off") {
    setStatsMode(false);
  } else if (cmd == "stats") {
    Serial.printf("統計模式：%s\n", statsMode ? "開啟" : "關閉");
  } else if (cmd == "find") {
    toggleFind();
  } else if (cmd.startsWith("find ")) {
    startFind(cmd.substring(5) == "off" ? "" : cmd.substring(5).c_str());
    publishSnapshot();
  } else if (cmd == "oled dump") {
    oledMirrorOn();
    oledHoldUntil = 0;
    pollScreen(true);
    oledDump();
  } else if (cmd.startsWith("oled demo")) {
    oledDemo(cmd.length() > 10 ? cmd.substring(10) : String(""));
  } else if (cmd == "meeting stop") {
    stopMeeting();
  } else if (cmd.startsWith("meeting start")) {
    const int thr = cmd.length() > 14 ? cmd.substring(14).toInt() : -70;
    if (!startMeeting(thr)) Serial.println("現在不能開始（房間檢查、尋找或更新韌體中）");
  } else if (cmd == "meeting") {
    // 序列埠只印總數，不印位址
    Serial.printf("會議室檢查：%s，門檻 %d dBm，基準 %lu 個，多出 %lu 個，近距離探測請求每分鐘 %.1f 個（基準 %.1f）\n",
                  meet.phase == meeting::BASELINE ? "建立基準中" : meet.phase == meeting::WATCH ? "監看中" : "沒在檢查",
                  meet.rssiMin, (unsigned long)meet.baseCount(millis()), (unsigned long)meet.newCount(millis()),
                  meet.probeRate(millis()), meet.baseRate());
    // 多出來的裝置只印類型、訊號與時間，不印位址與名稱
    for (const auto& d : meet.devs) {
      if (d.base || !meet.present(d, millis())) continue;
      Serial.printf("  多出：%s，%d dBm，開始後 %lu 秒出現\n", d.label, d.rssi, (unsigned long)((d.first - meet.startedAt) / 1000));
    }
  } else if (cmd == "probes start") {
    if (!startProbes()) Serial.println("現在不能開始（房間檢查、尋找或更新韌體中）");
  } else if (cmd == "probes stop") {
    stopProbes();
  } else if (cmd == "probes") {
    // 序列埠只印總數，不印名稱：A 板的序列埠會被記錄下來
    drainProbes();
    Serial.printf("探測請求：%s，頻道 %u，請求 %lu 個，來源 %u 個（固定位址 %lu 個），帶名稱 %lu 個，不同的名稱 %u 個\n",
                  probeOn ? "監聽中" : "沒在監聽", probeCh, (unsigned long)probeSurvey.frames,
                  (unsigned)probeSurvey.sources.size(), (unsigned long)probeSurvey.fixedSources,
                  (unsigned long)probeSurvey.directed, (unsigned)probeSurvey.ssids.size());
  } else if (cmd == "room") {
    if (!requestRoomCheck()) Serial.println("現在不能開始房間檢查（正在檢查、更新韌體或發聲）");
  } else if (cmd.startsWith("room find ")) {
    startRoomFind(cmd.substring(10).toInt());
  } else if (cmd == "room stop") {
    cancelRoomCheck();
  } else if (cmd == "room report") {
    printRoomReport(false);
  } else if (cmd == "room json") {
    Serial.println(webpanel::roomJson);
  } else if (cmd == "roomlog on" || cmd == "roomlog off") {
    setRoomLog(cmd == "roomlog on");
  } else if (cmd == "mem") {
    logMem("cmd");
  } else if (cmd == "info") {
    Serial.printf("flash 記錄：%s，%u bytes，分割區剩餘 %u bytes\n",
                  logReady ? (logFull ? "已滿" : "記錄中") : "暫停",
                  (unsigned)logSize(),
                  logMounted ? (unsigned)(FFat.totalBytes() - FFat.usedBytes()) : 0);
    Serial.printf("自動跳出面板：%s，安靜模式：%s，統計模式：%s\n", webpanel::captive ? "開啟" : "關閉",
                  buzzerOn ? "關閉" : "開啟", statsMode ? "開啟" : "關閉");
  }
}

static void pollSerialCommand() {
  static String line;
  bool wrote = false;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) handleCommand(line);
      line = "";
      wrote = true;
    } else if (line.length() < 96) {
      line += c;
    }
  }
  if (wrote) flushLog();
}

static_assert(!(CROWD_MODE && STATS_MODE), "CROWD_MODE 與 STATS_MODE 擇一使用");

// ---- 人流計數模式 --------------------------------------------------------

static uint8_t crowdSalt[16];
static std::vector<CrowdSeen> crowd;
static uint32_t crowdNow = 0, crowdNew = 0, crowdNear = 0;
static uint32_t crowdKind[KIND_COUNT];
static uint32_t crowdWinUnique = 0, crowdWinNew = 0, crowdWinStay = 0;

static void setupCrowd() {
  if (!CROWD_MODE) return;
  for (size_t i = 0; i < sizeof(crowdSalt); i += 4) {
    const uint32_t r = esp_random();
    memcpy(crowdSalt + i, &r, 4);
  }
}

static uint64_t hashAddr(const uint8_t* addr) {
  uint8_t buf[sizeof(crowdSalt) + 6];
  memcpy(buf, crowdSalt, sizeof(crowdSalt));
  memcpy(buf + sizeof(crowdSalt), addr, 6);
  uint8_t digest[32];
  mbedtls_sha256(buf, sizeof(buf), digest, 0);
  uint64_t h;
  memcpy(&h, digest, sizeof(h));
  return h;
}

static void beginCrowdScan() {
  crowdNow = crowdNew = crowdNear = 0;
  for (int k = 0; k < KIND_COUNT; k++) crowdKind[k] = 0;
}

static void recordCrowd(const NimBLEAdvertisedDevice* d) {
  const NimBLEAddress& a = d->getAddress();
  const uint64_t h = hashAddr(a.getVal());
  crowdNow++;
  crowdKind[addrKind(a)]++;
  if (d->getRSSI() >= -70) crowdNear++;

  for (auto& e : crowd) {
    if (e.h == h) {
      e.lastScan = scanCount;
      return;
    }
  }
  crowdNew++;
  const CrowdSeen e{h, scanCount, scanCount};
  if (crowd.size() < MAX_CROWD) {
    crowd.push_back(e);
    return;
  }
  size_t oldest = 0;
  for (size_t i = 1; i < crowd.size(); i++) {
    if (crowd[i].lastScan < crowd[oldest].lastScan) oldest = i;
  }
  crowd[oldest] = e;
}

static void endCrowdScan() {
  // CROWD,掃描次數,看到幾個,第一次出現幾個,訊號強於 -70 dBm 幾個,公開,隨機固定,RPA,NRPA
  out.printf("CROWD,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n", (unsigned long)scanCount,
             (unsigned long)crowdNow, (unsigned long)crowdNew, (unsigned long)crowdNear,
             (unsigned long)crowdKind[KIND_PUBLIC], (unsigned long)crowdKind[KIND_STATIC],
             (unsigned long)crowdKind[KIND_RPA], (unsigned long)crowdKind[KIND_NRPA]);

  if (scanCount % CROWD_WINDOW_SCANS == 0) {
    const uint32_t from = scanCount - CROWD_WINDOW_SCANS + 1;
    crowdWinUnique = crowdWinNew = crowdWinStay = 0;
    for (const auto& e : crowd) {
      if (e.lastScan >= from) crowdWinUnique++;
      if (e.firstScan >= from) crowdWinNew++;
      // 整段時間都在：這段開始前就出現，這次掃描還在
      if (e.firstScan < from && e.lastScan == scanCount) crowdWinStay++;
    }
    // CROWDW,掃描次數,這段時間的不同裝置,其中新出現的,整段時間都在的
    out.printf("CROWDW,%lu,%lu,%lu,%lu\n", (unsigned long)scanCount,
               (unsigned long)crowdWinUnique, (unsigned long)crowdWinNew,
               (unsigned long)crowdWinStay);
  }

  for (size_t i = 0; i < crowd.size();) {
    if (scanCount - crowd[i].lastScan > CROWD_FORGET_SCANS) {
      crowd.erase(crowd.begin() + i);
    } else {
      i++;
    }
  }
}

// ---- 時間 ----------------------------------------------------------------
//
// 板子沒有時鐘，接上電腦時由電腦送出 time <Unix 秒數> 對時。
// 換電源會重新開機，時間就不見了，要等之後接上 DS3231 才能保留

static bool timeKnown = false;
static uint32_t epochAtBoot = 0;  // 開機那一刻的 Unix 秒數

static uint32_t epochNow() { return epochAtBoot + millis() / 1000; }

static void setTime(uint32_t epoch) {
  epochAtBoot = epoch - millis() / 1000;
  timeKnown = true;
  out.printf("TIME,%lu,%lu\n", (unsigned long)epoch, (unsigned long)(millis() / 1000));
}

// ---- Wi-Fi 基地台 --------------------------------------------------------

static std::vector<ApSeen> aps;
static uint32_t wifiNow = 0;   // 這次掃描看到幾個基地台
static uint32_t wifiNew = 0;   // 其中第一次出現的
static uint32_t wifiKept = 0;  // 其中上一次掃描也看得到的

static void setupWifiScan() {
  if (!wifiScanOn) return;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);  // 不連線，也清掉任何儲存的網路設定
}

static void publishPlaces();

// 換了地點，或開機後第一次掃描（overlapPct 為 -2）。跟存過的指紋比對，認得的話沿用原本的編號
static void changePlace(const std::vector<uint64_t>& current, int overlapPct, const std::vector<uint32_t>& keys) {
  bool revisit = false;
  int score = 0;
  placeId = placeBook.enter(keys, &revisit, &score);
  if (overlapPct != -2) lastPlaceChangeScan = scanCount;
  placeAps = current;
  // PLACE,掃描次數,地點編號,與上個地點重疊的比例,基地台數,new 或 revisit,跟最像的指紋重疊的比例。
  // 名稱只存在板子上，不寫進記錄
  out.printf("PLACE,%lu,%u,%d,%u,%s,%d\n", (unsigned long)scanCount, placeId, overlapPct, (unsigned)current.size(),
             revisit ? "revisit" : "new", score);
  publishPlaces();
}

static void updatePlace(const std::vector<uint64_t>& current, const std::vector<uint32_t>& keys) {
  if (current.size() < PLACE_MIN_APS) return;
  if (placeAps.empty()) {
    changePlace(current, -2, keys);
    placeBook.observe(keys);
    return;
  }
  size_t same = 0;
  for (uint64_t k : current) {
    for (uint64_t p : placeAps) {
      if (p == k) { same++; break; }
    }
  }
  const int pct = same * 100 / current.size();
  if (pct < PLACE_MIN_OVERLAP_PCT) {
    changePlace(current, pct, keys);
    placeBook.observe(keys);
    return;
  }
  const size_t before = placeBook.list.size();
  placeBook.observe(keys);
  if (placeBook.list.size() != before) publishPlaces();  // 停留夠久，剛存成指紋
  for (uint64_t k : current) {
    if (std::find(placeAps.begin(), placeAps.end(), k) == placeAps.end()) placeAps.push_back(k);
  }
  if (placeAps.size() > 200) placeAps.erase(placeAps.begin(), placeAps.begin() + (placeAps.size() - 200));
}

static void scanWifi() {
  if (!wifiScanOn || findWifi || probeOn) return;  // 尋找 Wi-Fi 裝置、監聽探測請求時射頻要停在同一個頻道
  const int16_t n = WiFi.scanNetworks(false, true, true, WIFI_MS_PER_CHANNEL);
  wifiNow = wifiNew = wifiKept = 0;
  if (n < 0) {
    out.printf("WIFI,%lu,error,%d\n", (unsigned long)scanCount, n);
    return;
  }

  std::vector<uint64_t> current;
  std::vector<uint32_t> placeKeys;  // 地點指紋用：排除隨機位址、加鹽雜湊過的 BSSID
  for (int i = 0; i < n; i++) {
    const uint8_t* b = WiFi.BSSID(i);
    if (!b) continue;
    wifiNow++;
    uint64_t key = 0;
    memcpy(&key, b, 6);
    current.push_back(key);
    if (!places::randomBssid(b)) placeKeys.push_back(places::keyOf(b, placeBook.salt));

    // SSID 裡的逗號與控制字元換成空白，避免破壞 CSV 的欄位
    String ssid = WiFi.SSID(i);
    for (size_t k = 0; k < ssid.length(); k++) {
      if (ssid[k] == ',' || (uint8_t)ssid[k] < 0x20) ssid.setCharAt(k, ' ');
    }
    // 人流計數模式不輸出個別基地台，手機開的個人熱點也算在裡面
    if (!CROWD_MODE) {
      out.printf("AP,%lu,%02x:%02x:%02x:%02x:%02x:%02x,%d,%d,%s\n",
                 (unsigned long)scanCount, b[0], b[1], b[2], b[3], b[4], b[5],
                 (int)WiFi.RSSI(i), (int)WiFi.channel(i), ssid.c_str());
    }

    bool found = false;
    for (auto& a : aps) {
      if (memcmp(a.bssid, b, 6) == 0) {
        if (a.lastScan == scanCount - 1) wifiKept++;
        a.lastScan = scanCount;
        found = true;
        break;
      }
    }
    if (found) continue;
    wifiNew++;
    ApSeen a;
    memcpy(a.bssid, b, 6);
    a.firstScan = a.lastScan = scanCount;
    if (aps.size() < MAX_APS) {
      aps.push_back(a);
    } else {
      size_t oldest = 0;
      for (size_t k = 1; k < aps.size(); k++) {
        if (aps[k].lastScan < aps[oldest].lastScan) oldest = k;
      }
      aps[oldest] = a;
    }
  }
  WiFi.scanDelete();

  for (size_t i = 0; i < aps.size();) {
    if (scanCount - aps[i].lastScan > WIFI_FORGET_SCANS) {
      aps.erase(aps.begin() + i);
    } else {
      i++;
    }
  }
  // WIFI,掃描次數,看到幾個,第一次出現幾個,上一次也看得到幾個
  out.printf("WIFI,%lu,%lu,%lu,%lu\n", (unsigned long)scanCount, (unsigned long)wifiNow,
             (unsigned long)wifiNew, (unsigned long)wifiKept);
  updatePlace(current, placeKeys);
}

// ---- 統計模式 ------------------------------------------------------------

// 依藍牙規格，隨機位址最高的兩個位元決定它的類型
static uint8_t addrKind(const NimBLEAddress& a) {
  const uint8_t type = a.getType();
  if (type == BLE_ADDR_PUBLIC || type == BLE_ADDR_PUBLIC_ID) return KIND_PUBLIC;
  const uint8_t top = a.getVal()[5] >> 6;  // getVal() 是 little-endian，[5] 是最高位元組
  if (top == 0x3) return KIND_STATIC;
  if (top == 0x1) return KIND_RPA;
  return KIND_NRPA;
}

static const char* kindName(uint8_t k) {
  switch (k) {
    case KIND_PUBLIC: return "public";
    case KIND_STATIC: return "static";
    case KIND_RPA:    return "rpa";
    default:          return "nrpa";
  }
}

static void formatAddr(const uint8_t* a, char* out) {
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

// 每個裝置輸出一行 DEV，記錄檔可以拿來追蹤每個位址何時出現、何時消失：
//   DEV,掃描次數,位址,類型,訊號,已持續秒數
static void printDevice(const uint8_t* v, uint8_t kind, int rssi, uint32_t ageMs) {
  char addr[18];
  formatAddr(v, addr);
  out.printf("DEV,%lu,%s,%s,%d,%lu\n", (unsigned long)scanCount, addr,
                kindName(kind), rssi, (unsigned long)(ageMs / 1000));
}

static void recordDevice(const NimBLEAdvertisedDevice* d, uint32_t now) {
  const NimBLEAddress& a = d->getAddress();
  const uint8_t* v = a.getVal();
  const uint8_t kind = addrKind(a);
  nowCount[kind]++;

  for (auto& e : devices) {
    if (memcmp(e.addr, v, 6) == 0) {
      e.lastSeen = now;
      e.rssi = d->getRSSI();
      if (now - e.firstSeen >= PERSIST_MS) persistCount[kind]++;
      printDevice(v, kind, e.rssi, now - e.firstSeen);
      return;
    }
  }

  printDevice(v, kind, d->getRSSI(), 0);

  totalCount[kind]++;
  DeviceSeen e;
  memcpy(e.addr, v, 6);
  e.kind = kind;
  e.rssi = d->getRSSI();
  e.firstSeen = now;
  e.lastSeen = now;
  if (devices.size() < MAX_DEVICES) {
    devices.push_back(e);
    return;
  }
  size_t oldest = 0;
  for (size_t i = 1; i < devices.size(); i++) {
    if (devices[i].lastSeen < devices[oldest].lastSeen) oldest = i;
  }
  devices[oldest] = e;
}

static void forgetStaleDevices(uint32_t now) {
  for (size_t i = 0; i < devices.size();) {
    if (now - devices[i].lastSeen > STATS_FORGET_MS) {
      devices.erase(devices.begin() + i);
    } else {
      i++;
    }
  }
}

static uint32_t sum(const uint32_t* c) {
  uint32_t s = 0;
  for (int k = 0; k < KIND_COUNT; k++) s += c[k];
  return s;
}

static void printStatsHeader() {
  out.println(
      "STATS,scan,uptime_s,now,now_public,now_static,now_rpa,now_nrpa,"
      "persist,persist_public,persist_static,persist_rpa,persist_nrpa,"
      "total,total_public,total_static,total_rpa,total_nrpa");
}

static void printStatsRow(uint32_t now) {
  out.printf("STATS,%lu,%lu", (unsigned long)scanCount, (unsigned long)(now / 1000));
  for (const uint32_t* c : {nowCount, persistCount, totalCount}) {
    out.printf(",%lu", (unsigned long)sum(c));
    for (int k = 0; k < KIND_COUNT; k++) out.printf(",%lu", (unsigned long)c[k]);
  }
  out.println();
}

// 執行中切換統計模式。開啟時從頭計數，關閉時釋放記錄的位址，
// 記錄裡寫一行 MODE,掃描次數,stats,on 或 off，分析時用來切出統計的區段
static void setStatsMode(bool on) {
  if (on && CROWD_MODE) {
    out.println("人流計數模式下不能開啟統計模式");
    return;
  }
  if (on == statsMode) return;
  statsMode = on;
  prefs.putBool("stats", on);
  out.printf("MODE,%lu,stats,%s\n", (unsigned long)scanCount, on ? "on" : "off");
  if (on) {
    devices.clear();
    for (int k = 0; k < KIND_COUNT; k++) nowCount[k] = persistCount[k] = totalCount[k] = 0;
    printStatsHeader();
  } else {
    devices.clear();
    devices.shrink_to_fit();
  }
  flushLog();
}

// 執行中開關 flash 記錄的寫入，記錄裡寫一行 LOG,掃描次數,on 或 off
static void setLogEnabled(bool on) {
  prefs.putBool("log", on);
  if (on == logReady) return;
  if (on) {
    if (!mountLog(true)) {
      Serial.println("flash 記錄：無法掛載 FAT 分割區");
      return;
    }
    webpanel::logPath = LOG_PATH;
    logReady = true;
    logFull = logSize() >= LOG_MAX_BYTES;
    out.captureToBuffer(!logFull);
    out.printf("LOG,%lu,on\n", (unsigned long)scanCount);
    if (statsMode) printStatsHeader();
  } else {
    out.printf("LOG,%lu,off\n", (unsigned long)scanCount);
    flushLog();
    logReady = false;
    out.captureToBuffer(false);
  }
  flushLog();
}

static void setQuiet(bool quiet) {
  buzzerOn = !quiet;
  prefs.putBool("buzzer", buzzerOn);
  out.printf("安靜模式：%s\n", quiet ? "開啟" : "關閉");
}

// 列出用同一個位址持續出現超過門檻的裝置，可以拿來比對是不是自己的裝置
static void printPersistent(uint32_t now) {
  char addr[18];
  for (const auto& e : devices) {
    if (e.lastSeen != now || now - e.firstSeen < PERSIST_MS) continue;
    formatAddr(e.addr, addr);
    out.printf("  持續出現 %s  %-6s  %lu 分鐘  RSSI %d\n", addr, kindName(e.kind),
                  (unsigned long)((now - e.firstSeen) / 60000), e.rssi);
  }
}

// ---- 輸出 ----------------------------------------------------------------

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE,
                                                OLED_SCL_GPIO, OLED_SDA_GPIO);
static bool oledReady = false;
// 沒接 OLED 時，序列埠的 oled dump、oled demo 仍在記憶體裡的畫面緩衝區繪製，印出來檢查
static bool oledMirror = false;
static bool oledOn() { return oledReady || oledMirror; }
static void oledSend() {
  if (oledReady) oled.sendBuffer();
}

// 這塊板子的藍牙識別位址，用來分辨工作坊裡的每一塊板子。偵測時只做被動掃描，不送出任何封包；
// 打開藍牙連線時，對外廣播的是定期更換的隨機位址（RPA），這個識別位址也不會被周圍的裝置收到
static std::string selfAddr;

// 依序試 OLED 常見的兩個位址，回傳找到的位址，找不到回傳 0
static uint8_t probeOled() {
  Wire.begin(OLED_SDA_GPIO, OLED_SCL_GPIO);
  for (uint8_t addr : {0x3C, 0x3D}) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return addr;
  }
  return 0;
}

static void setupOled() {
  const uint8_t addr = probeOled();
  if (addr == 0) {
    out.printf("沒有偵測到 OLED（SDA=GPIO%d、SCL=GPIO%d），略過顯示\n",
                  OLED_SDA_GPIO, OLED_SCL_GPIO);
    return;
  }
  out.printf("偵測到 OLED，I2C 位址 0x%02X\n", addr);
  oled.setI2CAddress(addr * 2);  // U8g2 使用 8 位元的位址
  oled.begin();
  oled.setFont(u8g2_font_6x10_tf);
  oled.setFontPosTop();
  oledReady = true;
}

// OLED 只顯示 ASCII，類型用縮寫
static const char* trackerShort(TrackerType t) {
  switch (t) {
    case TrackerType::AppleFindMySeparated: return "Apple";
    case TrackerType::SamsungSmartTag:      return "SmTag";
    case TrackerType::Tile:                 return "Tile";
    case TrackerType::GoogleFindHub:        return "Googl";
    case TrackerType::Dult:                 return "DULT";
    case TrackerType::AppleDevice:          return "AplDv";
    case TrackerType::Chipolo:              return "Chipo";
    case TrackerType::Pebblebee:            return "Pebbl";
    default:                                return "?";
  }
}

static void showMessage(const char* line1, const char* line2) {
  if (!oledReady) return;
  oled.clearBuffer();
  oled.drawStr(0, 0, line1);
  oled.drawStr(0, 14, line2);
  oled.sendBuffer();
}

// 人流計數模式的畫面
static void drawCrowd(uint32_t now) {
  if (!oledOn()) return;
  char buf[32];
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tf);
  snprintf(buf, sizeof(buf), "CROWD #%lu  %lu min", (unsigned long)scanCount,
           (unsigned long)(now / 60000));
  oled.drawStr(0, 0, buf);
  oled.drawHLine(0, 10, 128);
  snprintf(buf, sizeof(buf), "now  %3lu   near %3lu", (unsigned long)crowdNow,
           (unsigned long)crowdNear);
  oled.drawStr(0, 14, buf);
  snprintf(buf, sizeof(buf), "new this scan  %3lu", (unsigned long)crowdNew);
  oled.drawStr(0, 24, buf);
  snprintf(buf, sizeof(buf), "5min uniq %4lu new %4lu", (unsigned long)crowdWinUnique,
           (unsigned long)crowdWinNew);
  oled.drawStr(0, 36, buf);
  snprintf(buf, sizeof(buf), "stayed 5min+ %4lu", (unsigned long)crowdWinStay);
  oled.drawStr(0, 46, buf);
  if (wifiScanOn) {
    snprintf(buf, sizeof(buf), "wifi %lu  kept %lu", (unsigned long)wifiNow,
             (unsigned long)wifiKept);
    oled.drawStr(0, 56, buf);
  }
  oledSend();
  oled.setFont(u8g2_font_6x10_tf);
}

// 數字縮寫成最多 4 個字：999、3.1k、12k
static void fmtCount(uint32_t n, char* out, size_t len) {
  if (n < 1000) {
    snprintf(out, len, "%lu", (unsigned long)n);
  } else if (n < 10000) {
    snprintf(out, len, "%lu.%luk", (unsigned long)(n / 1000), (unsigned long)(n % 1000 / 100));
  } else {
    snprintf(out, len, "%luk", (unsigned long)(n / 1000));
  }
}

// 統計模式的畫面：P 公開位址、S 隨機固定位址、R 可解析的隨機位址、N 不可解析的隨機位址
static void drawStats(uint32_t now) {
  if (!oledOn()) return;
  char buf[64];
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tf);
  if (logReady) {
    snprintf(buf, sizeof(buf), "#%lu %lum log %s%uK", (unsigned long)scanCount,
             (unsigned long)(now / 60000), logFull ? "FULL " : "",
             (unsigned)(logSize() / 1024));
  } else {
    snprintf(buf, sizeof(buf), "STATS #%lu  %lu min", (unsigned long)scanCount,
             (unsigned long)(now / 60000));
  }
  oled.drawStr(0, 0, buf);
  oled.drawHLine(0, 10, 128);
  // 表格用 4x6 的小字，一列放得下 32 個字。每欄 5 個字寬，數字最多 4 個字，
  // 欄位之間一定留有空白，四位數以上才不會黏在一起
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 14, "       all    P    S    R    N");

  const char* labels[] = {"now", "20m+", "total"};
  const uint32_t* rows[] = {nowCount, persistCount, totalCount};
  for (int r = 0; r < 3; r++) {
    const uint32_t* c = rows[r];
    const uint32_t vals[] = {sum(c), c[KIND_PUBLIC], c[KIND_STATIC], c[KIND_RPA], c[KIND_NRPA]};
    char cell[5][12];
    for (int k = 0; k < 5; k++) fmtCount(vals[k], cell[k], sizeof(cell[k]));
    snprintf(buf, sizeof(buf), "%-5s%5s%5s%5s%5s%5s", labels[r], cell[0], cell[1], cell[2],
             cell[3], cell[4]);
    oled.drawStr(0, 23 + r * 9, buf);
  }
  oled.setFont(u8g2_font_5x8_tf);
  if (wifiScanOn) {
    snprintf(buf, sizeof(buf), "trk %s  wifi %lu new %lu", table.empty() ? "none" : "FOUND",
             (unsigned long)wifiNow, (unsigned long)wifiNew);
    oled.drawStr(0, 56, buf);
  } else {
    oled.drawStr(0, 56, table.empty() ? "trackers: none" : "trackers: FOUND");
  }
  oledSend();
  oled.setFont(u8g2_font_6x10_tf);
}

// ---- OLED：依情境切換畫面 -------------------------------------------------
//
// 只在 loop() 裡畫。每 200 毫秒依目前的狀態挑一個畫面、填好資料，跟上一次畫的不同才重畫。
// 優先順序：韌體更新 > 發聲中 > 房間檢查中 > 尋找 > 剛打開 Wi-Fi 的 QR code（60 秒）>
// 發聲結果（8 秒）> 房間檢查完成 > 統計模式、人流計數 > 平常的狀態

static const uint32_t FIND_STALE_MS = 8000;   // 尋找時超過這麼久沒收到，當成可能換了位址或離開
static const uint32_t FIND_TICK_MS = 12;      // 尋找時每一聲嗶的長度，聽不清楚時調長
static const uint32_t DEMO_HOLD_MS = 5000;    // oled demo 的畫面在螢幕上停留的時間
static const uint32_t WIFI_QR_MS = 60000;
static const uint32_t SOUND_SHOW_MS = 8000;
static const uint32_t TOAST_MS = 2000;
static uint32_t wifiQrUntil = 0, soundShowUntil = 0, toastUntil = 0;
static char toastText[24] = "";
static int8_t soundResult = -1;
static bool screenDirty = true;

using screen::Scr;
using screen::View;
using namespace screen;  // SCR_NORMAL 這些常數

static bool before(uint32_t until, uint32_t now) { return until && (int32_t)(until - now) > 0; }

static void showToast(const char* t) {
  strlcpy(toastText, t, sizeof(toastText));
  toastUntil = millis() + TOAST_MS;
}

static void tailOf(const std::string& a, char* out, size_t len) {
  strlcpy(out, a.size() >= 5 ? a.c_str() + a.size() - 5 : a.c_str(), len);
}

// 離警示的進度：各條件的進度取最低的，跟手機面板的進度條算法相同
static uint8_t trackerPct(const Sighting& t) {
  if (t.alerted) return 100;
  const uint32_t since = scanCount - t.firstScan + 1;
  const uint32_t pres = since ? std::min<uint32_t>(100, t.windows * 100 / since) : 0;
  uint32_t p = (uint64_t)(t.lastSeen - t.firstSeen) * 100 / std::max<uint32_t>(alertSpanMs, 1);
  p = std::min<uint32_t>(p, (uint32_t)t.nearWindows * 100 / std::max<uint16_t>(alertMinWindows, 1));
  p = std::min<uint32_t>(p, pres * 100 / std::max<uint8_t>(alertMinPresence, 1));
  if (wifiScanOn) p = std::min<uint32_t>(p, (uint32_t)t.places * 100 / MOVE_MIN_PLACES);
  return std::min<uint32_t>(p, 100);
}

static bool unackedAlert() {
  for (const auto& t : table) {
    if (t.alerted && !t.acknowledged) return true;
  }
  return false;
}

static screen::Scr pickScreen(uint32_t now) {
  // 自動更新在背景檢查時不換畫面，真的開始下載才顯示進度
  const bool quietCheck = netAuto && netupdate::state != netupdate::DOWNLOADING && netupdate::state != netupdate::DONE;
  if (otaWaitUntil || (!quietCheck && (netPending || netupdate::busy() || webpanel::otaActive))) return SCR_FW;
  if (soundBusy) return SCR_SOUND;
  if (roomPending || roomStage) return SCR_ROOM;
  if (findOn) return SCR_FIND;
  if (before(wifiQrUntil, now) && linkWifi && !unackedAlert()) return SCR_WIFI;
  if (before(soundShowUntil, now)) return SCR_SOUND;
  if (before(roomDoneUntil, now)) return SCR_ROOM;
  if (statsMode) return SCR_STATS;
  if (CROWD_MODE) return SCR_CROWD;
  return SCR_NORMAL;
}

static void fillView(screen::View& v, uint32_t now) {
  switch (v.scr) {
    case SCR_NORMAL: {
      screen::Normal& n = v.n;
      std::vector<const Sighting*> rows;
      bool acked = false;
      for (const auto& t : table) {
        if (t.type == TrackerType::AppleDevice && !t.alerted) continue;  // 路上到處都是，不列出
        if (t.alerted) acked = true;
        rows.push_back(&t);
      }
      std::sort(rows.begin(), rows.end(), [](const Sighting* a, const Sighting* b) {
        const bool aa = a->alerted && !a->acknowledged, bb = b->alerted && !b->acknowledged;
        if (aa != bb) return aa;
        return a->rssi > b->rssi;
      });
      n.state = unackedAlert() ? 2 : acked ? 3 : rows.empty() ? 0 : 1;
      n.count = rows.size();
      n.scan = scanCount;
      n.scanning = scanRunning && !scanIsFind;
      for (const Sighting* t : rows) {
        if (n.nRows >= 3) break;
        screen::Row& r = n.rows[n.nRows++];
        strlcpy(r.type, trackerShort(t->type), sizeof(r.type));
        tailOf(t->addr, r.tail, sizeof(r.tail));
        r.rssi = t->rssi;
        r.pct = trackerPct(*t);
        r.alert = t->alerted && !t->acknowledged;
      }
      if (before(toastUntil, now)) strlcpy(n.toast, toastText, sizeof(n.toast));
      break;
    }
    case SCR_FIND: {
      screen::Find& f = v.f;
      strlcpy(f.type, findWifi ? "WiFi" : findLabel.length() ? "BLE" : findAddr == FAKE_ADDR ? "Apple" : "?",
              sizeof(f.type));
      for (const auto& t : table) {
        if (t.addr == findAddr) strlcpy(f.type, trackerShort(t.type), sizeof(f.type));
      }
      tailOf(findAddr, f.tail, sizeof(f.tail));
      f.rssi = findRssi;
      f.seen = findSeenAt != 0;
      f.agoSec = f.seen ? (now - findSeenAt) / 1000 : 0;
      f.stale = f.seen && now - findSeenAt > FIND_STALE_MS;
      const uint32_t gone = now - findStartAt;
      f.leftSec = gone < FIND_TIMEOUT_MS ? (FIND_TIMEOUT_MS - gone) / 1000 : 0;
      break;
    }
    case SCR_ROOM: {
      screen::Room& r = v.r;
      r.done = !roomPending && !roomStage;
      r.stage = roomPending ? 0 : roomStage;
      if (!r.done) {
        const uint32_t gone = roomStage ? (now - roomcheck::startedAt) / 1000 : 0;
        r.pct = std::min<uint32_t>(99, gone * 100 / roomcheck::EST_SEC);
        r.leftSec = gone < roomcheck::EST_SEC ? roomcheck::EST_SEC - gone : 0;
      } else {
        for (const auto& it : roomcheck::items) {
          r.lv2 += it.level == 2;
          r.lv1 += it.level == 1;
        }
        r.aps = roomcheck::apCount;
        r.stas = roomcheck::staCount;
        r.ble = roomcheck::bleTotal;
      }
      break;
    }
    case SCR_WIFI:
      strlcpy(v.w.name, deviceName.c_str(), sizeof(v.w.name));
      v.w.ble = linkBle;
      snprintf(v.w.pin, sizeof(v.w.pin), "%06lu", (unsigned long)blePin);
      v.w.leftSec = (wifiQrUntil - now) / 1000;
      break;
    case SCR_SOUND:
      tailOf(soundAddr, v.s.tail, sizeof(v.s.tail));
      v.s.busy = soundBusy;
      v.s.result = soundResult;
      break;
    case SCR_FW:
      if (otaWaitUntil) {
        v.fw.kind = 0;
        v.fw.leftSec = (int32_t)(otaWaitUntil - now) > 0 ? (otaWaitUntil - now) / 1000 : 0;
      } else if (netPending || netupdate::busy()) {
        v.fw.kind = 2;
        v.fw.got = netupdate::got;
        v.fw.total = netupdate::total;
      } else {
        v.fw.kind = 1;
      }
      break;
    default:
      v.scan = scanCount;
      break;
  }
}

static void drawView(const screen::View& v, uint32_t now) {
  if (v.scr == SCR_STATS) {
    drawStats(now);
    return;
  }
  if (v.scr == SCR_CROWD) {
    drawCrowd(now);
    return;
  }
  oled.clearBuffer();
  switch (v.scr) {
    case SCR_FIND: screen::drawFind(oled, v.f); break;
    case SCR_ROOM: screen::drawRoom(oled, v.r); break;
    case SCR_WIFI: screen::drawWifi(oled, v.w); break;
    case SCR_SOUND: screen::drawSound(oled, v.s); break;
    case SCR_FW: screen::drawFw(oled, v.fw); break;
    default: screen::drawNormal(oled, v.n); break;
  }
  oled.setFont(u8g2_font_6x10_tf);
  oledSend();
}

// force：馬上重畫，例如發聲這種會讓 loop() 停住好幾秒的動作開始前
static void pollScreen(bool force) {
  if (!oledOn()) return;
  static uint32_t lastBuild = 0;
  static View last;
  static bool have = false;
  const uint32_t now = millis();
  if (!force && before(oledHoldUntil, now)) return;  // oled demo 的畫面還在展示
  if (!force && !screenDirty && now - lastBuild < 200) return;
  lastBuild = now;
  View v;
  memset(&v, 0, sizeof(v));
  v.scr = pickScreen(now);
  fillView(v, now);
  if (!force && !screenDirty && have && memcmp(&v, &last, sizeof(v)) == 0) return;
  screenDirty = false;
  last = v;
  have = true;
  drawView(v, now);
}

// 序列埠用：把畫面緩衝區印成 1024 bytes 的十六進位（SSD1306 的格式，8 列 × 128 欄，
// 每個 byte 是直的 8 個點，最低位元在上）。內容可能有熱點密碼的 QR code，只印在序列埠
static void oledDump() {
  Serial.print("OLEDBUF,");
  const uint8_t* b = oled.getBufferPtr();
  for (int i = 0; i < 1024; i++) Serial.printf("%02x", b[i]);
  Serial.println();
}

static void oledMirrorOn() {
  if (oledReady || oledMirror) return;
  oledMirror = true;
  oled.setFont(u8g2_font_6x10_tf);
  oled.setFontPosTop();
}

// 序列埠用：用假資料畫指定的畫面，不改變板子的狀態
static void oledDemo(const String& name) {
  oledMirrorOn();
  View v;
  memset(&v, 0, sizeof(v));
  auto row = [](screen::Row& r, const char* type, const char* tail, int rssi, uint8_t pct, bool alert) {
    strlcpy(r.type, type, sizeof(r.type));
    strlcpy(r.tail, tail, sizeof(r.tail));
    r.rssi = rssi;
    r.pct = pct;
    r.alert = alert;
  };
  if (name == "normal" || name == "alert" || name == "empty") {
    v.scr = SCR_NORMAL;
    v.n.scan = 412;
    if (name != "empty") {
      v.n.state = name == "alert" ? 2 : 1;
      v.n.count = 3;
      v.n.nRows = 3;
      row(v.n.rows[0], "Apple", "a1:b2", -58, name == "alert" ? 100 : 72, name == "alert");
      row(v.n.rows[1], "SmTag", "47:27", -79, 35, false);
      row(v.n.rows[2], "Tile", "a3:c3", -93, 8, false);
      if (name == "normal") strlcpy(v.n.toast, "MARK 3", sizeof(v.n.toast));
      v.n.scanning = name == "normal";
    }
  } else if (name == "find" || name == "lost" || name == "wait") {
    v.scr = SCR_FIND;
    strlcpy(v.f.type, "Apple", sizeof(v.f.type));
    strlcpy(v.f.tail, "a1:b2", sizeof(v.f.tail));
    v.f.rssi = -62;
    v.f.seen = name != "wait";
    v.f.stale = name == "lost";
    v.f.agoSec = name == "lost" ? 12 : 1;
    v.f.leftSec = 252;
  } else if (name == "room" || name == "roomdone") {
    v.scr = SCR_ROOM;
    v.r.done = name == "roomdone";
    v.r.stage = 3;
    v.r.pct = 46;
    v.r.leftSec = 70;
    v.r.lv2 = 2;
    v.r.lv1 = 1;
    v.r.aps = 17;
    v.r.stas = 11;
    v.r.ble = 18;
  } else if (name == "wifi" || name == "wifilong") {
    v.scr = SCR_WIFI;
    const char* n = name == "wifi" ? "BLE-Tracker-0000" : "A-very-long-hotspot-name-29by";
    strlcpy(v.w.name, n, sizeof(v.w.name));
    v.w.ble = true;
    strlcpy(v.w.pin, "123456", sizeof(v.w.pin));
    v.w.leftSec = 57;
    // 預設密碼是 README 公開的那組，畫出來的 QR code 不含任何秘密
    screen::makeQr((String("WIFI:T:WPA;S:") + n + ";P:" +
                    (name == "wifi" ? "anoni-tracker" : "a-long-custom-password-for-test") + ";;")
                       .c_str());
  } else if (name == "sound" || name == "soundok" || name == "refused") {
    v.scr = SCR_SOUND;
    strlcpy(v.s.tail, "00:01", sizeof(v.s.tail));
    v.s.busy = name == "sound";
    v.s.result = name == "refused" ? trackersound::REFUSED : trackersound::OK;
  } else if (name == "fw" || name == "fwwait") {
    v.scr = SCR_FW;
    v.fw.kind = name == "fw" ? 2 : 0;
    v.fw.got = 612000;
    v.fw.total = 1320687;
    v.fw.leftSec = 48;
  } else {
    Serial.println("oled demo normal|alert|empty|find|lost|wait|room|roomdone|wifi|wifilong|sound|soundok|refused|fw|fwwait");
    return;
  }
  drawView(v, millis());
  oledDump();
  oledHoldUntil = millis() + DEMO_HOLD_MS;  // 接著 OLED 的板子停留幾秒，看得清楚
  screenDirty = true;  // 之後畫回真正的畫面
  if (v.scr == SCR_WIFI && before(wifiQrUntil, millis())) screen::makeQr(wifiQrText().c_str());
}

// 直接寫入 LED，不管燈號是否全關。恢復原廠設定的倒數用它提示
static void writeLed(uint8_t r, uint8_t g, uint8_t b) {
  if (RGB_LED_GPIO < 0) return;
  // 燈號每 20 毫秒更新一次，顏色沒變就不重送
  static uint32_t last = 0xFFFFFFFF;
  const uint32_t rgb = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
  if (rgb == last) return;
  last = rgb;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(RGB_LED_GPIO, r, g, b);
#else
  neopixelWrite(RGB_LED_GPIO, r, g, b);
#endif
}

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  if (!ledOn) r = g = b = 0;
  writeLed(r, g, b);
}

static void beep(uint32_t ms) {
  if (BUZZER_GPIO < 0 || !buzzerOn) return;
  digitalWrite(BUZZER_GPIO, BUZZER_ON);
  delay(ms);
  digitalWrite(BUZZER_GPIO, !BUZZER_ON);
}

// ---- LED 燈號 ------------------------------------------------------------
//
// 綠：恆亮，附近沒有追蹤器
// 黃：慢慢呼吸，附近有追蹤器，還沒達到警示門檻
// 紅：快閃三下、停一下，重複到按 BOOT 確認為止，節奏跟蜂鳴器的三聲對應
// 藍：恆亮，掃描中。掃描在背景進行，這段時間按鍵與手機面板照常有反應

enum class LedMode : uint8_t { Clear, Present, Alert };
static LedMode ledMode = LedMode::Clear;

// 在 loop() 裡每 20 毫秒呼叫一次，依目前的模式算出這一刻的顏色
static void updateLed(uint32_t now) {
  // 韌體更新等待按 BOOT：快閃紫色。使用者正在操作板子，燈號全關時也顯示
  if (otaWaitUntil) {
    const bool on = (now / 150) % 2;
    writeLed(on ? 32 : 0, 0, on ? 32 : 0);
    return;
  }
  // 按著 BOOT：先亮白色表示有讀到，滿 2 秒切換連線，5 秒起快閃黃色、10 秒起快閃白色
  // （恢復原廠設定的倒數）。使用者正在操作板子，燈號全關時也顯示
  if (buttonPressedAt) {
    const uint32_t held = now - buttonPressedAt;
    const bool on = (now / 100) % 2;
    if (held >= FACTORY_PRESS_MS) {
      writeLed(on ? 40 : 0, on ? 40 : 0, on ? 40 : 0);
      return;
    }
    if (held >= 5000) {
      writeLed(on ? 40 : 0, on ? 24 : 0, 0);
      return;
    }
    if (held < LONG_PRESS_MS) {
      writeLed(12, 12, 12);
      return;
    }
  }
  // 剛切換連線：新狀態的顏色恆亮 2 秒
  if ((int32_t)(linkFlashUntil - now) > 0) {
    writeLed(linkFlashR, linkFlashG, linkFlashB);
    return;
  }
  // 房間檢查中：白色慢慢呼吸。完成後綠色連閃兩下，提示可以連回熱點看報告。
  // 使用者正在等結果，燈號全關時也顯示
  if (roomPending || roomStage) {
    const uint32_t t = now % 3000;
    const uint8_t v = 2 + (t < 1500 ? t : 3000 - t) * 30 / 1500;
    writeLed(v, v, v);
    return;
  }
  // 尋找：跟嗶聲同步閃白色，越近閃得越快，平常留一點微亮表示在尋找（青色是 Wi-Fi 開著，不用）。
  // 太久沒收到（可能換了位址或離開）改成藍色慢慢呼吸，跟「離很遠」分得出來
  if (findOn) {
    if (findSeenAt && now - findSeenAt <= FIND_STALE_MS) {
      const bool flash = now - findTickAt < 50;
      setLed(flash ? 48 : 2, flash ? 48 : 2, flash ? 48 : 2);
    } else {
      const uint32_t t = now % 2400;
      const uint8_t v = 1 + (t < 1200 ? t : 2400 - t) * 15 / 1200;
      setLed(0, 0, v);
    }
    return;
  }
  if (roomDoneUntil && (int32_t)(roomDoneUntil - now) > 0) {
    const uint32_t t = now % 1200;
    writeLed(0, t < 500 && t % 250 < 120 ? 48 : 0, 0);
    return;
  }
  roomDoneUntil = 0;
  if (scanRunning && !scanIsFind) {
    setLed(0, 0, 16);  // 藍：掃描中
    return;
  }
  // 手機連線開著時，每 3 秒亮 0.5 秒連線的顏色：Wi-Fi 青色、藍牙紫色、兩者白色
  if (ledMode != LedMode::Alert && (linkWifi || linkBle) && now % 3000 < 500) {
    const uint8_t r = linkBle ? 24 : 0, g = linkWifi ? 24 : 0, b = 24;
    setLed(r, g, b);
    return;
  }
  switch (ledMode) {
    case LedMode::Clear:
      setLed(0, 8, 0);
      break;
    case LedMode::Present: {
      // 2 秒一個循環的三角波，亮度在 2 到 32 之間
      const uint32_t t = now % 2000;
      const uint32_t level = t < 1000 ? t : 2000 - t;
      const uint8_t r = 2 + level * 30 / 1000;
      setLed(r, r / 2, 0);
      break;
    }
    case LedMode::Alert: {
      // 1.5 秒一個循環：亮 150、暗 100，重複三次，之後暗 750 毫秒
      const uint32_t t = now % 1500;
      const bool on = t < 750 && (t % 250) < 150;
      setLed(on ? 64 : 0, 0, 0);
      break;
    }
  }
}

// 警示：蜂鳴器響三聲，LED 同步閃三下。沒接蜂鳴器或關掉時只有燈號
static void alarm() {
  for (int i = 0; i < 3; i++) {
    setLed(64, 0, 0);
    if (BUZZER_GPIO >= 0 && buzzerOn) {
      beep(150);
    } else {
      delay(150);
    }
    setLed(0, 0, 0);
    delay(100);
  }
}

static void printHex(const std::string& s) {
  for (uint8_t c : s) out.printf("%02X", c);
}

static void printVerbose(const NimBLEAdvertisedDevice* d, TrackerType t) {
  // conn=1 代表這則廣播接受連線，發聲要先連得上
  out.printf("  %s  %s  RSSI %d  conn=%d  ", d->getAddress().toString().c_str(),
                trackerName(t), d->getRSSI(), d->isConnectable() ? 1 : 0);
  if (d->haveManufacturerData()) {
    out.print("mfg=");
    printHex(d->getManufacturerData());
    out.print(" ");
  }
  for (uint8_t i = 0; i < d->getServiceDataCount(); i++) {
    out.printf("svc[%s]=", d->getServiceDataUUID(i).toString().c_str());
    printHex(d->getServiceData(i));
    out.print(" ");
  }
  out.println();
}

static void announce(const Sighting& s, uint32_t now) {
  // 藍牙連線開著時直接推送，LED 與蜂鳴器都關閉時也能從手機知道
  phonelink::send(String(TR("!!! 可能有追蹤器一直跟著你：")) + trackerNameUi(s.type) + " " + s.addr.c_str() +
                  TR("，輸入 ack 確認\n"));
  if (TrackHist* h = findHistory(s.addr)) {
    if (!h->alerted) {
      h->alerted = true;
      h->alertMs = now;
      h->acked = false;
      h->autoOff = !(autoMode >= 1 && wifiScanOn);
      histDirty = true;
    }
  }
  out.println();
  out.println("!!! 可能有追蹤器一直跟著你");
  out.printf("    類型：%s\n", trackerName(s.type));
  out.printf("    位址：%s\n", s.addr.c_str());
  out.printf("    持續：%lu 分鐘，出現在 %u 次掃描（其中 %u 次訊號夠強），最近訊號 %d dBm\n",
                (unsigned long)((now - s.firstSeen) / 60000), s.windows,
                s.nearWindows, s.rssi);
  out.println("    訊號越強（數字越接近 0）代表越近，可以拿著偵測器走動找出位置");
  out.println("    按 BOOT 鍵確認後警示燈會熄滅");
  out.println();
}

// 加進自動發聲的佇列。每個位址只排一次，重新開機前不會再自動響第二次
static void queueAutoSound(const std::string& addr, bool help) {
  for (const auto& a : autoSoundList) {
    if (a.addr == addr) return;
  }
  autoSoundList.push_back({addr, 0, false, help});
}

static void evaluate(uint32_t now) {
  bool anyPresent = false;
  bool anyActiveAlert = false;

  for (auto& s : table) {
    // 這一輪有出現才算在附近
    if (s.lastSeen == now) anyPresent = true;

    const uint32_t scansSinceFirst = scanCount - s.firstScan + 1;
    const bool persistent =
        (s.lastSeen - s.firstSeen) >= alertSpanMs &&
        s.nearWindows >= alertMinWindows &&
        s.windows * 100 >= scansSinceFirst * alertMinPresence &&
        // 打開 Wi-Fi 移動判斷時，要在好幾個地點都出現。Apple 自家裝置（iPhone、Mac 等）
        // 路上到處都是，iOS 也不警示，只有移動判斷確認一直跟著你才警示
        (wifiScanOn ? s.places >= MOVE_MIN_PLACES : s.type != TrackerType::AppleDevice);
    if (persistent && !s.alerted) {
      s.alerted = true;
      announce(s, now);
      // 開著移動判斷時，警示就代表追蹤器跟著你經過好幾個地點
      if (autoMode >= 1 && wifiScanOn) queueAutoSound(s.addr, false);
    }
    if (s.alerted && !s.acknowledged) anyActiveAlert = true;
    // 協助周圍的人：與主人分開的 DULT 追蹤器在很近的距離出現夠久
    if (autoMode == 2 && s.lastSeen == now && s.rssi >= HELP_RSSI && s.lastSeen - s.firstSeen >= HELP_SPAN_MS) {
      const TrackHist* h = findHistory(s.addr);
      if (h && h->dult) queueAutoSound(s.addr, true);
    }
  }

  if (anyActiveAlert) {
    ledMode = LedMode::Alert;
    alarm();
  } else if (anyPresent) {
    ledMode = LedMode::Present;
  } else {
    ledMode = LedMode::Clear;
  }
  updateLed(millis());
}

// 最近的記號，給手機面板顯示
static webpanel::MarkInfo recentMarks[webpanel::RECENT_MARKS];
static int recentMarkCount = 0;

// atMs 是按下的那一刻；從手機面板送來的記號會晚一點才處理，用按下的時間記錄。
// 有文字時多一個欄位：MARK,編號,掃描次數,開機後秒數,文字
static void markEvent(const char* label = "", uint32_t atMs = 0) {
  if (atMs == 0) atMs = millis();
  markCount++;
  String text = label;
  for (size_t i = 0; i < text.length(); i++) {
    if (text[i] == ',' || text[i] == '\n' || text[i] == '\r') text.setCharAt(i, ' ');
  }
  if (text.length()) {
    out.printf("MARK,%lu,%lu,%lu,%s\n", (unsigned long)markCount, (unsigned long)scanCount,
               (unsigned long)(atMs / 1000), text.c_str());
  } else {
    out.printf("MARK,%lu,%lu,%lu\n", (unsigned long)markCount,
               (unsigned long)scanCount, (unsigned long)(atMs / 1000));
  }
  flushLog();

  webpanel::MarkInfo m = {};
  m.n = markCount;
  m.uptime = atMs / 1000;
  m.epoch = timeKnown ? epochAtBoot + atMs / 1000 : 0;
  strlcpy(m.label, text.c_str(), sizeof(m.label));
  if (recentMarkCount < webpanel::RECENT_MARKS) {
    recentMarks[recentMarkCount++] = m;
  } else {
    for (int i = 1; i < webpanel::RECENT_MARKS; i++) recentMarks[i - 1] = recentMarks[i];
    recentMarks[webpanel::RECENT_MARKS - 1] = m;
  }
  if (allMarks.size() >= MARKS_MAX) allMarks.erase(allMarks.begin());
  allMarks.push_back({markCount, atMs / 1000, text.c_str()});
  histDirty = true;

  setLed(48, 48, 48);  // 白燈閃一下
  beep(30);
  delay(150);
  updateLed(millis());
  showToast((String("MARK ") + markCount).c_str());
}

// 確認所有警示，沒有待確認的警示時回傳 false
static bool acknowledgeAlerts() {
  bool any = false;
  for (auto& s : table) {
    if (s.alerted && !s.acknowledged) any = true;
    if (s.alerted) s.acknowledged = true;
    if (s.alerted) {
      if (TrackHist* h = findHistory(s.addr)) {
        h->acked = true;
        histDirty = true;
      }
    }
  }
  if (!any) return false;
  out.println("已確認目前的警示");
  // 追蹤器可能還在附近，確認之後改成黃燈呼吸，下一次掃描再重新判斷
  if (ledMode == LedMode::Alert) ledMode = LedMode::Present;
  updateLed(millis());
  screenDirty = true;
  return true;
}

// 按 BOOT：有警示就確認，沒有警示就在記錄裡打一個記號
static void acknowledgeAll() {
  if (!acknowledgeAlerts()) markEvent();
}

// 警示歷史的 JSON，內容有變更才重組並交給手機面板
static String readFileText(const char* path) {
  File f = FFat.open(path, FILE_READ);
  if (!f) return "";
  String t = f.readString();
  f.close();
  return t;
}

// 先寫到暫存檔再換名，寫到一半斷電時舊的檔案還在
static bool writeFileAtomic(const char* path, const char* tmp, const String& text) {
  File f = FFat.open(tmp, FILE_WRITE);
  if (!f) return false;
  const size_t n = f.print(text);
  f.close();
  if (n != text.length()) {
    FFat.remove(tmp);
    return false;
  }
  FFat.remove(path);
  return FFat.rename(tmp, path);
}

// 把 JSON 陣列拆成每一次開機的物件
static std::vector<String> splitSessions(const String& a) {
  std::vector<String> list;
  int depth = 0, start = -1;
  bool inStr = false, esc = false;
  for (int i = 0; i < (int)a.length(); i++) {
    const char c = a[i];
    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') {
      inStr = true;
    } else if (c == '{') {
      if (depth++ == 0) start = i;
    } else if (c == '}' && depth > 0 && --depth == 0 && start >= 0) {
      list.push_back(a.substring(start, i + 1));
      start = -1;
    }
  }
  return list;
}

// 有警示或記號的那次開機，超過上限時最後才丟
static bool sessionImportant(const String& j) {
  if (j.indexOf("\"marks\":[{") >= 0) return true;
  for (int i = j.indexOf("\"alert\":"); i >= 0; i = j.indexOf("\"alert\":", i + 8)) {
    if (j[i + 8] != '-') return true;
  }
  return false;
}

// 開機時呼叫：上一次開機存下的歷史移進 HIST_PREV
static void archiveHistory() {
  if (!logMounted) return;
  String cur = readFileText(HIST_CUR);
  if (!cur.length()) cur = readFileText(HIST_CUR_TMP);
  FFat.remove(HIST_CUR);
  FFat.remove(HIST_CUR_TMP);
  if (!cur.startsWith("{") || (cur.indexOf("\"trk\":[{") < 0 && cur.indexOf("\"marks\":[{") < 0)) return;
  std::vector<String> list = splitSessions(readFileText(HIST_PREV));
  list.insert(list.begin(), cur);
  size_t total = 0;
  for (const auto& j : list) total += j.length() + 1;
  while (list.size() > HIST_KEEP || (total > HIST_PREV_MAX && list.size() > 1)) {
    int drop = -1;
    for (int i = list.size() - 1; i >= 1 && drop < 0; i--) {
      if (!sessionImportant(list[i])) drop = i;
    }
    if (drop < 0) drop = list.size() - 1;
    total -= list[drop].length() + 1;
    list.erase(list.begin() + drop);
  }
  String a = "[";
  for (size_t i = 0; i < list.size(); i++) {
    if (i) a += ",";
    a += list[i];
  }
  a += "]";
  const bool ok = writeFileAtomic(HIST_PREV, "/hist_prev.json.tmp", a);
  // HIST,掃描次數,archived,保留幾次開機,bytes
  out.printf("HIST,0,%s,%u,%u\n", ok ? "archived" : "archive_failed", (unsigned)list.size(), (unsigned)a.length());
}

static void clearHistoryArchive() {
  if (logMounted) FFat.remove(HIST_PREV);
  out.printf("HIST,%lu,cleared\n", (unsigned long)scanCount);
}

// 警示、確認、發聲結果、記號的摘要值，變了就要立刻存檔
static uint32_t histSig() {
  uint32_t sig = history.size() * 7919 + allMarks.size() * 104729;
  for (const auto& h : history) sig = sig * 31 + h.alerted + h.acked * 2 + (h.sound + 1) * 4;
  return sig;
}

// 摘要值變了立刻存，只有出現時段變長時每 5 分鐘存一次
static void saveHistory(const String& j) {
  if (!logMounted || (history.empty() && allMarks.empty())) return;
  const uint32_t sig = histSig();
  if (sig == histSavedSig && histSavedAt && millis() - histSavedAt < HIST_SAVE_MS) return;
  // 寫入失敗也記下時間，等下一次變化或 5 分鐘後再試，不在每一圈重試
  if (!writeFileAtomic(HIST_CUR, HIST_CUR_TMP, j)) out.printf("HIST,%lu,save_failed\n", (unsigned long)scanCount);
  histSavedAt = millis();
  histSavedSig = sig;
}

static void publishHistory() {
  if (!histDirty || !webpanel::events || CROWD_MODE) return;
  histDirty = false;
  String j;
  j.reserve(4096);
  j = "{\"name\":\"" + webpanel::jsonEscape(deviceName.c_str()) + "\"";
  j += ",\"epoch\":" + String(timeKnown ? epochNow() : 0) + ",\"uptime\":" + String(millis() / 1000);
  j += ",\"trk\":[";
  for (size_t i = 0; i < history.size(); i++) {
    const TrackHist& h = history[i];
    if (i) j += ",";
    j += "{\"addr\":\"" + String(h.addr.c_str()) + "\",\"type\":\"" + trackerShort(h.type) + "\"";
    j += ",\"first\":" + String(h.firstMs / 1000) + ",\"last\":" + String(h.lastMs / 1000);
    j += ",\"alert\":" + String(h.alerted ? (long)(h.alertMs / 1000) : -1L);
    j += ",\"acked\":" + String(h.acked ? "true" : "false") + ",\"maxRssi\":" + String(h.maxRssi);
    j += ",\"scans\":" + String(h.scans) + ",\"sound\":" + String(h.sound) +
         ",\"autoOff\":" + String(h.autoOff ? "true" : "false") + ",\"iv\":[";
    for (uint8_t k = 0; k < h.nIv; k++) {
      if (k) j += ",";
      j += "[" + String(h.ivStart[k] / 1000) + "," + String(h.ivEnd[k] / 1000) + "]";
    }
    j += "],\"pl\":[";
    for (uint8_t k = 0; k < h.nPl; k++) {
      if (k) j += ",";
      j += String(h.pl[k]);
    }
    j += "]}";
  }
  // 有名稱的地點。存進之前開機的紀錄時一起保存，之後改名也看得到當時的名稱
  j += "],\"places\":{";
  bool firstPl = true;
  for (const auto& p : placeBook.list) {
    if (p.name.empty()) continue;
    if (!firstPl) j += ",";
    firstPl = false;
    j += "\"" + String(p.id) + "\":\"" + webpanel::jsonEscape(p.name.c_str()) + "\"";
  }
  j += "},\"marks\":[";
  for (size_t i = 0; i < allMarks.size(); i++) {
    if (i) j += ",";
    j += "{\"n\":" + String(allMarks[i].n) + ",\"t\":" + String(allMarks[i].t) + ",\"label\":\"" +
         webpanel::jsonEscape(allMarks[i].label.c_str()) + "\"}";
  }
  j += "]}";
  webpanel::publishHistory(j);
  saveHistory(j);
}

// ---- 執行中調整設定 ------------------------------------------------------

// 目前的設定寫一行 CFG 到記錄，分析時用來對照當時的門檻與掃描節奏
static void printCfg() {
  out.printf("CFG,%lu,preset=%s,span_s=%lu,near=%u,rssi=%d,presence=%u,interval_s=%lu,duration_s=%lu,"
             "idle_min=%lu,wifiscan=%d,autosound=%u\n",
             (unsigned long)scanCount, PRESETS[presetIdx].name, (unsigned long)(alertSpanMs / 1000),
             alertMinWindows, alertMinRssi, alertMinPresence, (unsigned long)(scanIntervalMs / 1000),
             (unsigned long)(scanDurationMs / 1000), (unsigned long)(linkIdleMs / 60000), wifiScanOn, autoMode);
}

static void usePreset(uint8_t idx) {
  presetIdx = idx;
  alertSpanMs = PRESETS[idx].spanMs;
  alertMinWindows = PRESETS[idx].windows;
  alertMinRssi = PRESETS[idx].rssi;
  alertMinPresence = PRESETS[idx].presence;
}

static bool validInterval(uint32_t s) { return s == 15 || s == 30 || s == 60; }
static bool validDuration(uint32_t s) { return s == 3 || s == 5; }
static bool validIdle(uint32_t m) { return m == 0 || m == 5 || m == 10 || m == 30; }

static void loadConfig() {
  const uint8_t p = prefs.getUChar("preset", DEFAULT_PRESET);
  usePreset(p < PRESET_COUNT ? p : DEFAULT_PRESET);
  const uint32_t iv = prefs.getUInt("interval", SCAN_INTERVAL_MS / 1000);
  const uint32_t du = prefs.getUInt("duration", SCAN_DURATION_MS / 1000);
  const uint32_t idle = prefs.getUInt("idle", LINK_IDLE_OFF_MS / 60000);
  if (validInterval(iv)) scanIntervalMs = iv * 1000;
  if (validDuration(du)) scanDurationMs = du * 1000;
  if (validIdle(idle)) linkIdleMs = idle * 60000;
  wifiScanOn = prefs.getBool("wifiscan", WIFI_SCAN);
  // 2026-11-26 以前存的是開關，開著的換成「跟著我移動時」
  autoMode = prefs.isKey("automode") ? prefs.getUChar("automode", 0) : (prefs.getBool("autosound", false) ? 1 : 0);
  if (autoMode > 2) autoMode = 0;
  roomLog = prefs.getBool("roomlog", false);
  autoUpdate = prefs.getBool("autoupd", false);
  testChannel = prefs.getBool("testchan", false);
  netupdate::setTestChannel(testChannel);
}

// 門檻改變時清空追蹤器的紀錄重新累計，避免同一個追蹤器的次數混用兩套門檻
static void setPreset(uint8_t idx) {
  if (idx >= PRESET_COUNT) return;
  usePreset(idx);
  prefs.putUChar("preset", idx);
  table.clear();
  out.println("警示門檻已變更，追蹤器的紀錄重新累計");
  printCfg();
  flushLog();
}

static void setScanTiming(uint32_t intervalSec, uint32_t durSec) {
  if (!validInterval(intervalSec) || !validDuration(durSec)) return;
  scanIntervalMs = intervalSec * 1000;
  scanDurationMs = durSec * 1000;
  prefs.putUInt("interval", intervalSec);
  prefs.putUInt("duration", durSec);
  printCfg();
  flushLog();
}

static void setIdle(uint32_t minutes) {
  if (!validIdle(minutes)) return;
  linkIdleMs = minutes * 60000;
  linkLastActive = millis();
  prefs.putUInt("idle", minutes);
  printCfg();
  flushLog();
}

static void setAutoSound(uint8_t mode) {
  if (mode > 2) return;
  autoMode = mode;
  prefs.putUChar("automode", mode);
  if (mode == 0) autoSoundList.clear();
  printCfg();
  flushLog();
}

static void setWifiScan(bool on) {
  if (on == wifiScanOn) return;
  wifiScanOn = on;
  prefs.putBool("wifiscan", on);
  webpanel::keepStaCfg = on;
  if (linkWifi) {
    WiFi.mode(on ? WIFI_AP_STA : WIFI_AP);
  } else {
    WiFi.mode(on ? WIFI_STA : WIFI_OFF);
  }
  if (on) {
    WiFi.disconnect(false, true);  // 不連線，也清掉任何儲存的網路設定
  } else {
    aps.clear();
    wifiNow = wifiNew = wifiKept = 0;
  }
  printCfg();
  flushLog();
}

// ---- 隱蔽與安全 ----------------------------------------------------------

// 變更熱點與藍牙名稱、熱點密碼。空字串代表不改，不合規定的值直接忽略，
// 避免存了錯誤的設定之後熱點開不起來
static void setIdentity(const String& name, const String& pw) {
  bool changed = false;
  if (name.length() && webpanel::validName(name) && name != deviceName) {
    deviceName = name;
    prefs.putString("apname", name);
    changed = true;
  }
  if (pw.length() && webpanel::validPass(pw) && pw != devicePass) {
    devicePass = pw;
    prefs.putString("appass", pw);
    changed = true;
  }
  if (!changed) return;
  webpanel::requestIdentity(deviceName, devicePass);
  phonelink::rename(deviceName);
  out.printf("IDENTITY,%lu,%s\n", (unsigned long)scanCount, deviceName.c_str());  // 不記密碼
  histDirty = true;
  flushLog();
}

static void setLedOn(bool on) {
  ledOn = on;
  prefs.putBool("led", on);
  if (!on) writeLed(0, 0, 0);
  out.printf("LED：%s\n", on ? "開啟" : "全關");
}

// 清除 NVS 裡所有的設定後重新開機，flash 記錄保留
static void factoryReset() {
  out.printf("FACTORY,%lu,reset\n", (unsigned long)scanCount);
  flushLog();
  prefs.clear();
  NimBLEDevice::deleteAllBonds();  // 配對紀錄由 NimBLE 另外存在 NVS
  if (logMounted) {
    FFat.remove(PLACES_PATH);  // 地點的指紋與名稱（家、公司）也是個人設定
    // 警示歷史是舉證用的資料，保留，但裡面存了當時的地點名稱，一併清掉
    for (const char* f : {HIST_CUR, HIST_CUR_TMP, HIST_PREV}) {
      if (!FFat.exists(f)) continue;
      const String t = readFileText(f);
      const std::string c = places::stripNames(std::string(t.c_str()));
      if (c.size() != t.length()) writeFileAtomic(f, "/hist_strip.tmp", String(c.c_str()));
    }
  }
  for (int i = 0; i < 6; i++) {
    writeLed(i % 2 ? 0 : 40, i % 2 ? 0 : 40, i % 2 ? 0 : 40);
    delay(120);
  }
  ESP.restart();
}

// ---- 藍牙配對 ------------------------------------------------------------

static void newBlePin() {
  blePin = 100000 + esp_random() % 900000;
  prefs.putUInt("blepin", blePin);
  phonelink::setPasskey(blePin);
  out.printf("BLEPIN,%lu,new\n", (unsigned long)scanCount);  // 不記配對碼本身
  Serial.printf("新的藍牙配對碼：%06lu\n", (unsigned long)blePin);
}

static void unpairAll() {
  phonelink::clearBonds();
  out.printf("BLEPIN,%lu,unpair_all\n", (unsigned long)scanCount);
}

// 熱點的連線資訊。QR 是手機相機看得懂的 Wi-Fi 格式，名稱與密碼裡的特殊字元要跳脫
static String wifiQrText() {
  auto escape = [](const String& v) {
    String o;
    for (size_t i = 0; i < v.length(); i++) {
      const char c = v[i];
      if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') o += '\\';
      o += c;
    }
    return o;
  };
  return "WIFI:T:WPA;S:" + escape(deviceName) + ";P:" + escape(devicePass) + ";;";
}

// ---- 韌體更新 ------------------------------------------------------------

// 面板按下「開放更新」（自動更新開著時叫「開放上傳」），等使用者短按 BOOT
static void otaRequestArm() {
  otaWaitUntil = millis() + OTA_ARM_WAIT_MS;
  bootWhat = 0;
  out.printf("OTA,%lu,wait_button\n", (unsigned long)scanCount);
  publishSnapshot();
}

// 面板要打開自動更新（what 1）或接收測試版（what 2），等使用者短按 BOOT 確認。關閉不必確認
static void requestBootConfirm(uint8_t what) {
  otaWaitUntil = millis() + OTA_ARM_WAIT_MS;
  bootWhat = what;
  out.printf("OTA,%lu,wait_button,%s\n", (unsigned long)scanCount, what == 1 ? "autoupd" : "testchan");
  publishSnapshot();
}

// 等待期間短按了 BOOT：開放更新，或打開等待確認的設定
static void bootConfirmed() {
  const uint8_t what = bootWhat;
  bootWhat = 0;
  if (what == 0) {
    otaArm();
    return;
  }
  otaWaitUntil = 0;
  beep(40);
  if (what == 1) setAutoUpdate(true);
  else setTestChannel(true);
}

// 使用者在期限內短按了 BOOT，開放上傳
static void otaArm() {
  otaWaitUntil = 0;
  webpanel::otaArmedUntil = millis() + OTA_OPEN_MS;
  out.printf("OTA,%lu,armed\n", (unsigned long)scanCount);
  beep(40);
  flushLog();
  publishSnapshot();  // 面板馬上換成可以更新的畫面，不等下一次掃描
}

// 韌體已寫入另一個程式槽，重新開機。openPanel：開機後自動打開 Wi-Fi 面板確認版本。
// 自動更新時旁邊可能沒有人，不打開熱點
static void otaFinish(uint32_t size, bool openPanel) {
  out.printf("OTA,%lu,done,%lu bytes,from %s\n", (unsigned long)scanCount, (unsigned long)size, BUILD_ID);
  flushLog();
  if (openPanel) prefs.putBool("otaboot", true);
  delay(1500);  // 讓面板先收到回應
  ESP.restart();
}

// 測試用：把執行中的韌體複製到另一個程式槽後從那裡開機，不用 Wi-Fi 也能驗證
// 寫入、驗證與切換程式槽的流程
// 從網址更新用的 Wi-Fi。密碼只存在 NVS，不寫進記錄，也不出現在面板的狀態
static void setNetWifi(const String& ssid, const String& pass) {
  String n = ssid;
  n.trim();
  if (n.length() > 32 || pass.length() > 63) {
    Serial.println("Wi-Fi 名稱最多 32 bytes，密碼最多 63 個字元");
    return;
  }
  netSsid = n;
  netPass = pass;
  prefs.putString("netssid", netSsid);
  prefs.putString("netpass", netPass);
  out.printf("OTA,%lu,net_wifi,%s\n", (unsigned long)scanCount, netSsid.length() ? "set" : "cleared");
  publishSnapshot();
}

// 面板要先按「開放更新」再短按 BOOT（needArm），序列埠代表有實體接觸，不必。
// 開啟自動更新後，從網址更新只會裝上社群發布、SHA-256 相符而且比較新的版本，也不必按。
// automatic：自動更新發起，先被動掃描，附近沒有這個 Wi-Fi 就不連線
static bool requestNetUpdate(bool needArm, bool automatic) {
  const bool armed = webpanel::otaArmedUntil && (int32_t)(webpanel::otaArmedUntil - millis()) > 0;
  if (needArm && !armed && !autoUpdate) return false;
  // 上傳與下載共用同一個 Update，上傳進行中就不開始
  if (netSsid.length() == 0 || netPending || netupdate::busy() || webpanel::otaActive) return false;
  stopMeeting();
  stopProbes();  // 連上基地台會換頻道
  netPending = true;
  netAuto = automatic;
  webpanel::otaActive = true;  // 不再開始新的掃描，射頻留給 Wi-Fi
  if (!automatic) out.printf("OTA,%lu,net_start,%s\n", (unsigned long)scanCount, netupdate::BASE_URL);
  logMem(automatic ? "auto" : "net");
  publishSnapshot();
  return true;
}

static void setTestChannel(bool test) {
  testChannel = test;
  netupdate::setTestChannel(test);
  prefs.putBool("testchan", test);
  out.printf("OTA,%lu,channel_%s\n", (unsigned long)scanCount, test ? "test" : "stable");
  publishSnapshot();
}

static void setAutoUpdate(bool on) {
  autoUpdate = on;
  prefs.putBool("autoupd", on);
  if (on) autoNextAt = millis() + AUTO_FIRST_MS;
  autoNonetLogged = false;
  out.printf("OTA,%lu,auto_%s\n", (unsigned long)scanCount, on ? "on" : "off");
  publishSnapshot();
}

// 自動更新：時間到了就在背景檢查一次。有人正在用板子、或重新開機會讓偵測中斷時延後
static void pollAutoUpdate(uint32_t now) {
  if (!autoUpdate || netSsid.length() == 0 || before(autoNextAt, now)) return;
  bool wait = probeOn || netPending || netupdate::busy() || webpanel::otaActive || otaWaitUntil ||
              (webpanel::otaArmedUntil && (int32_t)(webpanel::otaArmedUntil - now) > 0) || roomPending || roomStage ||
              findOn || findWifi || soundBusy || unackedAlert();
  // 手機連著熱點時，連上基地台會換頻道讓手機斷線，重新開機也會打斷正在看的面板
  if (!wait && linkWifi && WiFi.softAPgetStationNum() > 0) wait = true;
  for (const auto& t : table) {
    if (wait) break;
    if (!t.alerted && trackerPct(t) >= AUTO_DEFER_PCT) wait = true;
  }
  if (wait) {
    autoNextAt = now + AUTO_DEFER_MS;
    return;
  }
  autoNextAt = now + AUTO_FAIL_MS;  // 萬一沒有開始，也不要每一圈都重試
  requestNetUpdate(false, true);
}

static void pollNetUpdate() {
  if (netPending && !scanRunning) {
    netPending = false;
    if (findOn) startFind("");
    if (!netupdate::start(netSsid, netPass, BUILD_ID, netAuto, netAuto ? autoSkip : String(""))) {
      webpanel::otaActive = false;
      netAuto = false;
    }
    publishSnapshot();
  }
  static netupdate::State last = netupdate::IDLE;
  const netupdate::State st = netupdate::state;
  if (st == last) return;
  last = st;
  if (netupdate::busy()) {
    publishSnapshot();
    return;
  }
  netDoneAt = millis();
  netLastAuto = netAuto;
  // OTA,掃描次數,tls,加密連線配置在 PSRAM 的 bytes,配置在內部 RAM 的 bytes（累計）
  out.printf("OTA,%lu,tls,%lu,%lu\n", (unsigned long)scanCount, (unsigned long)netupdate::tlsPsBytes,
             (unsigned long)netupdate::tlsIntBytes);
  if (st == netupdate::DONE) {
    out.printf("OTA,%lu,%s,%s\n", (unsigned long)scanCount, netAuto ? "auto_done" : "net_done", netupdate::newBuild);
    if (netAuto) {
      // 開機後比對建置時間，確認換上的是剛下載的版本
      prefs.putString("autoto", netupdate::newBuild);
      prefs.putString("autofrom", FW_VERSION);
    }
    publishSnapshot();
    otaFinish(netupdate::total, !netAuto);
  } else if (st == netupdate::LATEST || st == netupdate::FAILED || st == netupdate::NONET) {
    const char* tag = st == netupdate::LATEST ? "latest" : st == netupdate::NONET ? "nonet" : "fail";
    if (netAuto) {
      autoNextAt = millis() + (st == netupdate::LATEST ? AUTO_EVERY_MS : st == netupdate::NONET ? AUTO_NONET_MS : AUTO_FAIL_MS);
      // OTA,掃描次數,auto_latest、auto_fail 或 auto_nonet,說明。不在家時每 30 分鐘掃描一次，
      // 附近沒有 Wi-Fi 連續發生時只記第一次
      if (st != netupdate::NONET || !autoNonetLogged) {
        out.printf("OTA,%lu,auto_%s,%s\n", (unsigned long)scanCount, tag, netupdate::msg);
      }
      autoNonetLogged = st == netupdate::NONET;
    } else {
      // OTA,掃描次數,net_latest 或 net_fail,說明
      out.printf("OTA,%lu,net_%s,%s\n", (unsigned long)scanCount, tag, netupdate::msg);
    }
    netAuto = false;
    webpanel::otaActive = false;
    flushLog();
    publishSnapshot();
  }
}

// ---- 房間檢查 ------------------------------------------------------------

// ---- 周圍手機洩漏的 Wi-Fi 名稱 ------------------------------------------

// 監聽 Wi-Fi 探測請求（probes.h）。熱點開著時停在熱點的頻道，面板可以一直連著。手機找 Wi-Fi 時會
// 輪流掃過每個頻道，停在一個頻道也收得到。收封包的回呼在 Wi-Fi 的工作裡執行，只把資料放進環形
// 緩衝區，loop() 再取出來統計，回呼裡不配置記憶體。位址只用雜湊計數，名稱只放在記憶體
static void onProbePacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT || !probeOn || !probeRaw) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  // sig_len 含結尾 4 bytes 的 FCS，去掉，不然指紋每個封包都不同
  const uint16_t len = p->rx_ctrl.sig_len > 4 ? p->rx_ctrl.sig_len - 4 : 0;
  if (len < 26 || p->payload[0] != 0x40) return;  // 只要探測請求
  const uint16_t n = len < PROBE_RAW ? len : PROBE_RAW;
  portENTER_CRITICAL(&probeLock);
  const uint8_t next = (probeHead + 1) % PROBE_RING;
  if (next == probeTail) {
    probeDrop = probeDrop + 1;
  } else {
    memcpy(probeRaw + (size_t)probeHead * PROBE_RAW, p->payload, n);
    probeRing[probeHead].len = n;
    probeRing[probeHead].rssi = p->rx_ctrl.rssi;
    probeHead = next;
  }
  portEXIT_CRITICAL(&probeLock);
}

static void publishProbes() {
  const uint32_t now = millis();
  const uint32_t end = probeOn ? now : probeStopAt;
  const uint32_t sec = probeStartAt ? (end - probeStartAt) / 1000 : 0;
  const uint32_t left = probeOn && now - probeStartAt < PROBE_MS ? (PROBE_MS - (now - probeStartAt)) / 1000 : 0;
  probeVer++;
  webpanel::publishProbes(String(probeSurvey.json(probeOn, sec, left, probeCh, probeVer).c_str()));
  probePubAt = now;
}

static bool startProbes() {
  if (probeOn) return true;
  if (roomPending || roomStage || findWifi || netPending || netupdate::busy() || webpanel::otaActive) return false;
  if (!probeRaw) probeRaw = (uint8_t*)heap_caps_malloc((size_t)PROBE_RING * PROBE_RAW, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!probeRaw) return false;
  probeSurvey.reset(esp_random());
  probeHead = probeTail = 0;
  probeDrop = 0;
  probeOwnMode = !linkWifi;
  if (probeOwnMode) {
    probePrevMode = WiFi.getMode();
    if (probePrevMode == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
  }
  uint8_t prim = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&prim, &second);
  probeCh = prim;
  wifi_promiscuous_filter_t flt = {};
  flt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(&onProbePacket);
  probeStartAt = millis();
  probeOn = true;
  esp_wifi_set_promiscuous(true);
  out.printf("PROBE,%lu,start,%u\n", (unsigned long)scanCount, probeCh);
  publishProbes();
  publishSnapshot();
  return true;
}

static void drainProbes() {
  static uint8_t f[PROBE_RAW];
  while (probeRaw) {
    ProbeRec r;
    portENTER_CRITICAL(&probeLock);
    const bool empty = probeTail == probeHead;
    if (!empty) {
      r = probeRing[probeTail];
      memcpy(f, probeRaw + (size_t)probeTail * PROBE_RAW, r.len);
      probeTail = (probeTail + 1) % PROBE_RING;
    }
    portEXIT_CRITICAL(&probeLock);
    if (empty) break;
    const uint8_t *mac, *ssid = nullptr;
    uint8_t sl = 0;
    if (!probes::parse(f, r.len, &mac, &ssid, &sl)) continue;
    probeSurvey.add(mac, ssid, sl, r.rssi);
    if (meet.running()) {
      const uint32_t now = millis();
      const probes::ProbeInfo pi = probes::describe(f, r.len);
      meet.probeDev(pi.fp, pi.model, pi.name, pi.apple, r.rssi, now);
      meet.probe(r.rssi, now);
    }
  }
}

static void stopProbes() {
  if (!probeOn) return;
  probeOn = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  drainProbes();
  probeStopAt = millis();
  if (probeOwnMode && !linkWifi) WiFi.mode(probePrevMode);
  // PROBE,掃描次數,done,秒數,頻道,請求數,來源數,固定位址數,帶名稱的請求數,不同的名稱數,丟掉的封包數。
  // 不寫位址也不寫名稱
  out.printf("PROBE,%lu,done,%lu,%u,%lu,%u,%lu,%lu,%u,%lu\n", (unsigned long)scanCount,
             (unsigned long)((probeStopAt - probeStartAt) / 1000), probeCh, (unsigned long)probeSurvey.frames,
             (unsigned)probeSurvey.sources.size(), (unsigned long)probeSurvey.fixedSources,
             (unsigned long)probeSurvey.directed, (unsigned)probeSurvey.ssids.size(), (unsigned long)probeDrop);
  flushLog();
  publishProbes();
  publishSnapshot();
}

// loop() 呼叫：取出收到的封包統計，每 2 秒更新面板，時間到或熱點關了就停止
static void pollProbes(uint32_t now) {
  if (!probeOn) return;
  drainProbes();
  // 會議室檢查開著時不限 5 分鐘
  if ((!meet.running() && now - probeStartAt >= PROBE_MS) || (!probeOwnMode && !linkWifi)) {
    stopProbes();
    return;
  }
  if (now - probePubAt >= 2000) publishProbes();
}

// ---- 會議室檢查 ----------------------------------------------------------

// 收走手機之後監看房間（meeting.h）。藍牙用掃描的結果，Wi-Fi 用探測請求的監聽。
// 只在面板提醒，不嗶、不亮警示燈。位址只用雜湊比對，原始位址只在記憶體給「尋找」用
static void publishMeeting() {
  meetVer++;
  webpanel::publishMeeting(String(meet.json(millis(), meetVer).c_str()));
  meetPubAt = millis();
}

static bool startMeeting(int threshold) {
  if (roomPending || roomStage || findWifi || netPending || netupdate::busy() || webpanel::otaActive) return false;
  if (threshold > -50 || threshold < -90) threshold = -70;
  meetSalt = esp_random();
  meet.start(millis(), threshold);
  meetWasAlert = false;
  meetOwnProbes = !probeOn;
  if (meetOwnProbes && !startProbes()) meetOwnProbes = false;
  out.printf("MEETING,%lu,start,%d\n", (unsigned long)scanCount, threshold);
  lastScanAt = 0;  // 馬上掃描一次
  publishMeeting();
  publishSnapshot();
  return true;
}

static void stopMeeting() {
  if (!meet.running()) return;
  const uint32_t now = millis();
  // MEETING,掃描次數,done,分鐘,門檻,同時多出的裝置最多幾個,近距離的探測請求數。不寫位址
  out.printf("MEETING,%lu,done,%lu,%d,%lu,%lu\n", (unsigned long)scanCount, (unsigned long)((now - meet.startedAt) / 60000),
             meet.rssiMin, (unsigned long)meet.maxNew, (unsigned long)meet.probesTotal);
  meet.stop();
  if (meetOwnProbes) stopProbes();
  meetOwnProbes = false;
  flushLog();
  publishMeeting();
  publishSnapshot();
}

// finishScanWindow() 對每個收到的藍牙裝置呼叫
static void meetingBle(const NimBLEAdvertisedDevice* d, uint32_t now) {
  if (!meet.running() || d->getRSSI() < meet.floorRssi()) return;
  const std::string mfr = d->haveManufacturerData() ? d->getManufacturerData() : std::string();
  std::string fastPair;
  for (int i = 0; i < d->getServiceDataCount(); i++) {
    if (d->getServiceDataUUID(i) == NimBLEUUID((uint16_t)0xFE2C)) {
      fastPair = d->getServiceData(i);
      if (fastPair.empty()) fastPair = std::string(1, '\0');  // 有這個服務、但不是可配對時的型號編號
      break;
    }
  }
  devinfo::Info in = devinfo::parse(mfr, fastPair);
  // 廣播的名稱常帶主人的名字，只放在記憶體、顯示在面板
  if (d->haveName()) {
    const std::string nm = d->getName();
    in.name = probes::printable((const uint8_t*)nm.data(), nm.size() > 40 ? 40 : nm.size());
  }
  const uint8_t* a = d->getAddress().getVal();
  meet.ble(probes::hashMac(a, meetSalt), a, in, d->getRSSI(), now);
}

// 會議室檢查列出的裝置：用原始位址尋找，記錄不寫位址
static void startMeetingFind(uint32_t idx) {
  if (!meet.running() || idx >= meet.devs.size()) return;
  const uint8_t* m = meet.devs[idx].addr;  // NimBLE 的位址是 little-endian
  char addr[18];
  snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", m[5], m[4], m[3], m[2], m[1], m[0]);
  findQuiet = true;
  startFind(addr);
  findQuiet = false;
  findLabel = String(meet.devs[idx].label) + TR("（會議室檢查）");
  out.printf("FIND,%lu,meeting,%lu\n", (unsigned long)scanCount, (unsigned long)idx);
  publishSnapshot();
}

static void pollMeeting(uint32_t now) {
  if (!meet.running()) return;
  meet.tick(now);
  const bool a = meet.alert(now);
  if (a != meetWasAlert) {
    meetWasAlert = a;
    // MEETING,掃描次數,alert 或 clear,多出的裝置數,每分鐘的近距離探測請求
    out.printf("MEETING,%lu,%s,%lu,%.1f\n", (unsigned long)scanCount, a ? "alert" : "clear",
               (unsigned long)meet.newCount(now), meet.probeRate(now));
    publishMeeting();
    publishSnapshot();
  } else if (now - meetPubAt >= 2000) {
    publishMeeting();
  }
}

static bool requestRoomCheck() {
  stopMeeting();
  stopProbes();  // 房間檢查也要用監聽，先停掉
  if (roomPending || roomStage || netPending || netupdate::busy() || webpanel::otaActive || soundBusy) return false;
  roomPending = true;
  publishSnapshot();
  return true;
}

static void finishRoomCheck() {
  if (roomcheck::running()) roomcheck::state = roomcheck::CANCELLED;  // 藍牙階段就取消
  roomStage = 0;
  if (roomHadWifi) {
    webpanel::setEnabled(true);
    linkLastActive = millis();  // 手機還沒連回來，閒置計時從現在算
  }
  roomVer++;
  const bool done = roomcheck::state == roomcheck::DONE;
  if (done) roomDoneUntil = millis() + 120000;
  int lv2 = 0, lv1 = 0;
  for (const auto& it : roomcheck::items) {
    lv2 += it.level == 2;
    lv1 += it.level == 1;
  }
  // ROOM,掃描次數,done 或 cancelled,基地台,連網裝置,藍牙,建議找出來,可以留意,秒數。不含任何位址
  out.printf("ROOM,%lu,%s,%d,%d,%d,%d,%d,%lu\n", (unsigned long)scanCount, done ? "done" : "cancelled",
             roomcheck::apCount, (int)roomcheck::staCount, roomcheck::bleTotal, lv2, lv1,
             (unsigned long)((millis() - roomcheck::startedAt) / 1000));
  if (done) printRoomReport(roomLog);
  webpanel::publishRoom(roomcheck::json(roomVer));
  flushLog();
  publishSnapshot();
}

static void cancelRoomCheck() {
  if (roomPending) {
    roomPending = false;
    out.printf("ROOM,%lu,cancelled\n", (unsigned long)scanCount);
    publishSnapshot();
    return;
  }
  if (roomStage) roomcheck::cancelReq = true;
}

// 逐一列出裝置只印在序列埠，不寫進 flash 記錄。toLog（設定「記錄裝置的位址」開著）時，
// 另外用 ROOMDEV 把位址寫進記錄
static void printRoomReport(bool toLog) {
  if (roomcheck::state != roomcheck::DONE) {
    Serial.println(roomStage ? "房間檢查進行中" : "還沒有房間檢查的結果");
    return;
  }
  Serial.printf("房間檢查：基地台 %d 個、連網裝置 %d 個、藍牙 %d 個，監聽頻道", roomcheck::apCount,
                (int)roomcheck::staCount, roomcheck::bleTotal);
  for (int i = 0; i < roomcheck::chanCount; i++) Serial.printf(" %u", roomcheck::chans[i]);
  Serial.println();
  for (const auto& it : roomcheck::items) {
    String why = it.why;
    why.trim();
    why.replace("\n", " | ");
    // ROOMITEM,等級,種類,名稱,補充,訊號,頻道,上傳 B/s,理由
    Serial.printf("ROOMITEM,%u,%s,%s,%s,%d,%u,%lu,%s\n", it.level, roomcheck::KIND[it.kind], it.name.c_str(),
                  it.sub.c_str(), it.rssi, it.ch, (unsigned long)it.upBps, why.c_str());
    if (toLog) {
      const uint8_t* m = it.mac;
      // 藍牙的位址在 NimBLE 裡是 little-endian，反過來印才是一般的寫法
      if (it.kind == 2) {
        out.printf("ROOMDEV,%lu,%s,%02x:%02x:%02x:%02x:%02x:%02x,%u,%d,%u,%lu\n", (unsigned long)scanCount,
                   roomcheck::KIND[it.kind], m[5], m[4], m[3], m[2], m[1], m[0], it.level, it.rssi, it.ch,
                   (unsigned long)it.upBps);
      } else {
        out.printf("ROOMDEV,%lu,%s,%02x:%02x:%02x:%02x:%02x:%02x,%u,%d,%u,%lu\n", (unsigned long)scanCount,
                   roomcheck::KIND[it.kind], m[0], m[1], m[2], m[3], m[4], m[5], it.level, it.rssi, it.ch,
                   (unsigned long)it.upBps);
      }
    }
  }
}

// ---- 記憶體記錄 ----------------------------------------------------------

// MEM,掃描次數,原因,內部剩餘,內部最大連續,內部開機以來最低,PSRAM 剩餘,PSRAM 最大連續,追蹤器筆數,歷史筆數,存過的地點
// 單位 bytes。原因：boot 開機、tick 每 10 分鐘、net 或 auto 網址更新開始、cmd 序列埠 mem
// TLS 交握要約 40 KB 的連續空間，內部最大連續低於這個數字時，網址更新改用 PSRAM（netupdate.h）
static void logMem(const char* why) {
  memLoggedAt = millis();
  out.printf("MEM,%lu,%s,%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long)scanCount, why,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)table.size(),
             (unsigned)history.size(), (unsigned)placeBook.list.size());
}

// ---- 地點 ----------------------------------------------------------------

static void publishPlaces() {
  String j = "{\"cur\":" + String(placeId) + ",\"curSaved\":" + String(placeBook.cur >= 0 ? "true" : "false") +
             ",\"max\":" + String(places::MAX_PLACES) + ",\"list\":[";
  for (size_t i = 0; i < placeBook.list.size(); i++) {
    const places::Place& p = placeBook.list[i];
    if (i) j += ",";
    j += "{\"id\":" + String(p.id) + ",\"name\":\"" + webpanel::jsonEscape(p.name.c_str()) + "\",\"visits\":" +
         String(p.visits) + ",\"aps\":" + String(p.fp.size()) + "}";
  }
  j += "]}";
  webpanel::publishPlaces(j);
}

static void savePlaces(bool force) {
  if (!placeBook.dirty || !logMounted) return;
  if (!force && millis() - placesSavedAt < PLACES_SAVE_MS) return;
  placesSavedAt = millis();
  placeBook.dirty = false;
  if (!writeFileAtomic(PLACES_PATH, "/places.tmp", String(placeBook.serialize().c_str()))) {
    out.printf("PLACE,%lu,save_failed\n", (unsigned long)scanCount);
  }
}

static void loadPlaces() {
  if (logMounted && FFat.exists(PLACES_PATH)) {
    const String t = readFileText(PLACES_PATH);
    if (!placeBook.parse(std::string(t.c_str()))) out.printf("PLACE,0,load_failed\n");
  }
  if (!placeBook.salt) {
    placeBook.salt = esp_random() | 1;  // 每塊板子自己的鹽，同一個基地台在不同板子上的雜湊不同
    placeBook.dirty = true;
  }
  // PLACE,0,loaded,存了幾個地點
  out.printf("PLACE,0,loaded,%u\n", (unsigned)placeBook.list.size());
  publishPlaces();
}

// 名稱是 UTF-8，最多 30 bytes，不能有控制字元
static bool validPlaceName(const String& n) {
  if (n.length() > places::NAME_BYTES) return false;
  for (size_t i = 0; i < n.length(); i++) {
    const uint8_t c = n[i];
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

// id 為 0 代表目前所在的地點。名稱不寫進記錄
static bool renamePlace(uint16_t id, String name) {
  name.trim();
  if (!id) id = placeId;
  if (!id || !validPlaceName(name)) return false;
  if (!placeBook.rename(id, std::string(name.c_str()))) return false;
  out.printf("PLACE,%lu,named,%u\n", (unsigned long)scanCount, id);
  savePlaces(true);
  publishPlaces();
  histDirty = true;
  publishSnapshot();
  return true;
}

static bool forgetPlace(uint16_t id) {
  if (!placeBook.forget(id)) return false;
  if (id == placeId) placeId = placeBook.curId;  // 目前所在的地點被刪除，改用新的編號
  out.printf("PLACE,%lu,forgot,%u\n", (unsigned long)scanCount, id);
  savePlaces(true);
  publishPlaces();
  histDirty = true;
  publishSnapshot();
  return true;
}

static void printPlaces() {
  Serial.printf("目前在地點 #%u%s，存了 %u 個地點（最多 %u 個）\n", placeId, placeBook.cur >= 0 ? "" : "（還沒存成指紋）",
                (unsigned)placeBook.list.size(), places::MAX_PLACES);
  for (const auto& p : placeBook.list) {
    Serial.printf("  #%u %s，來過 %u 次，指紋 %u 個基地台\n", p.id, p.name.empty() ? "（未命名）" : p.name.c_str(),
                  p.visits, (unsigned)p.fp.size());
  }
}

static void setRoomLog(bool on) {
  roomLog = on;
  prefs.putBool("roomlog", on);
  out.printf("ROOM,%lu,log_%s\n", (unsigned long)scanCount, on ? "on" : "off");
  publishSnapshot();
}

// 在 loop() 裡呼叫，依序推進：藍牙主動掃描 → 關熱點 → Wi-Fi 監聽 → 恢復
static void pollRoomCheck() {
  if (roomPending && !scanRunning) {
    roomPending = false;
    if (findOn) startFind("");
    roomcheck::beginBle();
    roomHadWifi = linkWifi;
    out.printf("ROOM,%lu,start\n", (unsigned long)scanCount);
    // 主動掃描才收得到放在掃描回應裡的裝置名稱。送出的掃描請求只帶板子的隨機位址
    scan->clearResults();
    scan->setActiveScan(true);
    scan->setWindow(100);
    roomStage = 1;
    roomStageAt = millis();
    if (!scan->start(roomcheck::BLE_MS, false)) out.printf("ROOM,%lu,ble_scan_failed\n", (unsigned long)scanCount);
    publishSnapshot();
    return;
  }
  if (roomStage == 1) {
    if (scan->isScanning() && !roomcheck::cancelReq) return;
    if (scan->isScanning()) scan->stop();
    NimBLEScanResults r = scan->getResults();
    for (int i = 0; i < r.getCount(); i++) roomcheck::addBle(r.getDevice(i));
    scan->clearResults();
    scan->setActiveScan(false);
    scan->setWindow(linkWifi ? SCAN_WINDOW_WIFI_MS : 100);
    if (roomcheck::cancelReq) {
      finishRoomCheck();
      return;
    }
    webpanel::setEnabled(false);  // 熱點由面板的工作關閉，等它關好再切到監聽模式
    roomStage = 2;
    roomStageAt = millis();
    publishSnapshot();
    return;
  }
  if (roomStage == 2) {
    if (webpanel::active && millis() - roomStageAt < 5000) return;
    roomStage = 3;
    if (!roomcheck::startWifi()) finishRoomCheck();
    publishSnapshot();
    return;
  }
  if (roomStage == 3 && !roomcheck::running()) finishRoomCheck();
}

static void otaSelfTest() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  const size_t size = ESP.getSketchSize();
  Serial.printf("OTA 自我測試：從 %s 複製 %u bytes\n", run->label, (unsigned)size);
  if (!Update.begin(size)) {
    Serial.printf("Update.begin 失敗：%s\n", Update.errorString());
    return;
  }
  static uint8_t buf[4096];
  for (size_t off = 0; off < size; off += sizeof(buf)) {
    const size_t n = std::min(sizeof(buf), size - off);
    esp_partition_read(run, off, buf, n);
    if (Update.write(buf, n) != n) {
      Serial.printf("Update.write 失敗：%s\n", Update.errorString());
      Update.abort();
      return;
    }
  }
  if (!Update.end(true)) {
    Serial.printf("Update.end 失敗：%s\n", Update.errorString());
    return;
  }
  otaFinish(size, true);
}

// ---- 手機面板 ------------------------------------------------------------

// 把目前的狀態複製給網頁讀取
static void publishSnapshot() {
  if (!webpanel::events) return;
  webpanel::Snapshot s;
  s.scan = scanCount;
  s.uptime = millis() / 1000;
  s.trackers = table.size();
  for (const auto& t : table) {
    if (t.alerted && !t.acknowledged) s.alert = true;
  }
  s.marks = markCount;
  s.timeKnown = timeKnown;
  s.epoch = timeKnown ? epochNow() : 0;
  s.logMounted = logMounted;
  s.logOn = logReady;
  s.logFull = logFull;
  s.quiet = !buzzerOn;
  s.logKB = logSize() / 1024;
  s.wifiNow = wifiNow;
  s.statsOn = statsMode;
  s.statsAllowed = !CROWD_MODE;
  s.statNow = sum(nowCount);
  s.statPersist = sum(persistCount);
  s.statTotal = sum(totalCount);
  s.bleOn = linkBle;
  s.bleConnected = phonelink::connected;
  strlcpy(s.mode, statsMode ? "stats" : CROWD_MODE ? "crowd" : "detect", sizeof(s.mode));
  for (int i = 0; i < recentMarkCount; i++) s.recent[i] = recentMarks[i];
  s.recentCount = recentMarkCount;

  // 追蹤器：警示中的排前面，其次是訊號強的
  const uint32_t now = millis();
  std::vector<const Sighting*> rows;
  for (const auto& t : table) {
    if (t.type == TrackerType::AppleDevice && !t.alerted) {
      s.appleDevices++;
      continue;
    }
    rows.push_back(&t);
  }
  std::sort(rows.begin(), rows.end(), [](const Sighting* a, const Sighting* b) {
    const bool aa = a->alerted && !a->acknowledged, bb = b->alerted && !b->acknowledged;
    if (aa != bb) return aa;
    return a->rssi > b->rssi;
  });
  for (const Sighting* t : rows) {
    if (s.trkCount >= webpanel::MAX_SHOWN) break;
    webpanel::TrackerInfo& o = s.trk[s.trkCount++];
    strlcpy(o.addr, t->addr.c_str(), sizeof(o.addr));
    strlcpy(o.type, trackerShort(t->type), sizeof(o.type));
    o.rssi = t->rssi;
    o.spanSec = (t->lastSeen - t->firstSeen) / 1000;
    o.agoSec = (now - t->lastSeen) / 1000;
    o.windows = t->windows;
    o.nearWindows = t->nearWindows;
    const uint32_t since = scanCount - t->firstScan + 1;
    o.presence = since ? std::min<uint32_t>(100, t->windows * 100 / since) : 0;
    o.alerted = t->alerted;
    o.acked = t->acknowledged;
    o.places = t->places;
    o.rhN = t->rhN;
    memcpy(o.rh, t->rh, sizeof(o.rh));
  }
  s.needSpanSec = alertSpanMs / 1000;
  s.needNear = alertMinWindows;
  s.needPresence = alertMinPresence;
  s.needRssi = alertMinRssi;
  s.needPlaces = wifiScanOn ? MOVE_MIN_PLACES : 0;
  s.placeId = placeId;
  s.lastScanAt = lastScanAt;
  s.memFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s.memBig = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  s.memMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  s.psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s.tlsPs = netupdate::tlsPsBytes;
  s.tlsInt = netupdate::tlsIntBytes;
  s.bleNow = bleNowCount;
  strlcpy(s.placeName, placeBook.nameOf(placeId), sizeof(s.placeName));
  s.placeCount = placeBook.list.size();
  s.moving = wifiScanOn && lastPlaceChangeScan && scanCount - lastPlaceChangeScan <= 3;
  s.preset = presetIdx;
  s.intervalSec = scanIntervalMs / 1000;
  s.everySec = (meet.running() ? MEET_SCAN_MS : scanIntervalMs) / 1000;
  s.meetPhase = meet.phase;
  s.meetVer = meetVer;
  s.meetAlert = meet.running() && meet.level(now) == 2;
  s.meetCaution = meet.running() && meet.level(now) == 1;
  s.durSec = scanDurationMs / 1000;
  s.idleMin = linkIdleMs / 60000;
  s.wifiScan = wifiScanOn;
  s.autoMode = autoMode;
  s.roomState = roomPending ? 1 : roomStage;
  s.probeOn = probeOn;
  s.probeVer = probeVer;
  s.roomVer = roomVer;
  s.roomLog = roomLog;
  s.roomEst = roomcheck::EST_SEC;
  if (roomStage) {
    const uint32_t gone = (millis() - roomcheck::startedAt) / 1000;
    s.roomLeftSec = gone < roomcheck::EST_SEC ? roomcheck::EST_SEC - gone : 0;
  }
  strlcpy(s.apName, deviceName.c_str(), sizeof(s.apName));
  s.defaultPass = devicePass == WEB_PANEL_PASS;
  s.ledOn = ledOn;
  s.blePin = blePin;
  s.bleBonds = phonelink::server ? phonelink::bondCount() : 0;
  s.bleSecure = phonelink::secure;
  s.soundBusy = soundBusy;
  strlcpy(s.soundAddr, soundAddr.c_str(), sizeof(s.soundAddr));
  strlcpy(s.soundMsg, soundMsg.c_str(), sizeof(s.soundMsg));
  s.soundAgoSec = soundAt ? (now - soundAt) / 1000 : 0;
  strlcpy(s.build, BUILD_ID, sizeof(s.build));
  strlcpy(s.rev, FW_VERSION, sizeof(s.rev));
  strlcpy(s.netRev, netupdate::newRev, sizeof(s.netRev));
  const esp_partition_t* run = esp_ota_get_running_partition();
  strlcpy(s.part, run ? run->label : "?", sizeof(s.part));
  strlcpy(s.netSsid, netSsid.c_str(), sizeof(s.netSsid));
  s.netState = netPending ? netupdate::CONNECTING : netupdate::state;
  s.netNote = netupdate::latestNote;
  strlcpy(s.netMsg, netupdate::msg, sizeof(s.netMsg));
  strlcpy(s.netBuild, netupdate::newBuild, sizeof(s.netBuild));
  s.netGot = netupdate::got;
  s.netTotal = netupdate::total;
  s.autoUpd = autoUpdate;
  s.testChan = testChannel;
  s.netLastAuto = netLastAuto;
  s.netAgoSec = netDoneAt ? (now - netDoneAt) / 1000 : 0;
  s.autoNextSec = autoUpdate && before(autoNextAt, now) ? (autoNextAt - now) / 1000 : 0;
  strlcpy(s.autoFrom, autoFrom.c_str(), sizeof(s.autoFrom));
  if (otaWaitUntil && (int32_t)(otaWaitUntil - now) > 0) {
    s.otaState = 1;
    s.bootWhat = bootWhat;
    s.otaLeftSec = (otaWaitUntil - now) / 1000;
  } else if (webpanel::otaArmedUntil && (int32_t)(webpanel::otaArmedUntil - now) > 0) {
    s.otaState = 2;
    s.otaLeftSec = (webpanel::otaArmedUntil - now) / 1000;
  }

  s.findOn = findOn;
  if (findOn) {
    // 房間檢查的裝置只顯示名稱，位址留在板子上
    if (findLabel.length()) strlcpy(s.findLabel, findLabel.c_str(), sizeof(s.findLabel));
    else strlcpy(s.findAddr, findAddr.c_str(), sizeof(s.findAddr));
    s.findWifi = findWifi;
    s.findRssi = findRssi;
    s.findSeen = findSeenAt != 0;
    s.findAgoSec = findSeenAt ? (now - findSeenAt) / 1000 : 0;
    s.findLeftSec = (FIND_TIMEOUT_MS - std::min(FIND_TIMEOUT_MS, now - findStartAt)) / 1000;
  }
  webpanel::publish(s);
}

static void applyLink(bool wifi, bool ble);

// 開始尋找指定的追蹤器，空字串代表結束。記錄裡寫一行 FIND,掃描次數,位址 或 off
static void stopWifiFind();

static void startFind(const char* addr) {
  if (findWifi) stopWifiFind();
  findLabel = "";
  findAddr = addr;
  findOn = !findAddr.empty();
  findRssi = 0;
  findSeenAt = 0;
  findStartAt = millis();
  // 剛掃到的話先帶入最近一次的訊號
  for (const auto& t : table) {
    if (findOn && t.addr == findAddr) {
      findRssi = t.rssi;
      findSeenAt = t.lastSeen;
    }
  }
  // 房間檢查的裝置由 startRoomFind() 另外記錄，不寫位址
  if (!findQuiet) out.printf("FIND,%lu,%s\n", (unsigned long)scanCount, findOn ? findAddr.c_str() : "off");
}

// 雙擊 BOOT（或序列埠 find）：尋找中就結束，否則尋找警示中的追蹤器，沒有警示時找訊號最強的
static void toggleFind() {
  if (findOn) {
    startFind("");
  } else if (!roomPending && !roomStage && !soundBusy) {
    // 追蹤器 30 分鐘沒出現才移除，所以只挑最近兩次掃描還在的。早就離開的只有警示過才列入，
    // 排在現在還在的後面
    const uint32_t now = millis(), recent = scanIntervalMs * 2 + scanDurationMs;
    const Sighting* best = nullptr;
    int bestScore = -100000;
    for (const auto& t : table) {
      if (t.type == TrackerType::AppleDevice && !t.alerted) continue;
      const bool here = now - t.lastSeen <= recent;
      if (!here && !t.alerted) continue;
      const int score = (here ? 4000 : 0) + (t.alerted && !t.acknowledged ? 2000 : t.alerted ? 1000 : 0) + t.rssi;
      if (score > bestScore) {
        bestScore = score;
        best = &t;
      }
    }
    if (!best) {
      // 附近沒有追蹤器：紅燈閃兩下、一聲短的低音，OLED 顯示提示，不打記號
      out.printf("FIND,%lu,none\n", (unsigned long)scanCount);
      showToast("nothing to find");
      for (int i = 0; i < 2; i++) {
        setLed(32, 0, 0);
        delay(100);
        setLed(0, 0, 0);
        delay(100);
      }
      beep(120);
      return;
    }
    startFind(best->addr.c_str());
  }
  flushLog();
  publishSnapshot();
}

// 尋找房間檢查報告裡的裝置。藍牙裝置用一般的尋找，Wi-Fi 裝置要讓射頻停在它的頻道：
// 熱點開著就把熱點換到那個頻道，再打開監聽，只記下這個位址送出的封包的訊號
static void startRoomFind(uint32_t idx) {
  stopProbes();
  if (roomcheck::state != roomcheck::DONE || idx >= roomcheck::items.size()) return;
  if (roomPending || roomStage || soundBusy || netPending || netupdate::busy() || webpanel::otaActive) return;
  const roomcheck::Item& it = roomcheck::items[idx];
  const uint8_t* m = it.mac;
  char addr[18];
  if (it.kind == 2) {
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", m[5], m[4], m[3], m[2], m[1], m[0]);
    findQuiet = true;
    startFind(addr);
    findQuiet = false;
    findLabel = it.name + (it.sub.length() ? TR("（") + it.sub + TR("）") : String("")) + TR("，藍牙");
  } else {
    if (it.ch < 1 || it.ch > 13) return;
    startFind("");
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    findAddr = addr;
    findOn = true;
    findStartAt = millis();
    findWifi = true;
    memcpy(findWifiMac, m, 6);
    findWifiCh = it.ch;
    findWifiStage = 1;
    findLabel = it.name + (it.sub.length() ? TR("，") + it.sub : String("")) + TR("，Wi-Fi 頻道 ") + it.ch;
  }
  // 記錄裡不寫位址，只寫是報告裡的第幾個
  out.printf("FIND,%lu,room_%s,%lu\n", (unsigned long)scanCount, roomcheck::KIND[it.kind], (unsigned long)idx);
  flushLog();
  publishSnapshot();
}

static void stopWifiFind() {
  roomcheck::wifiFindStop();
  if (findWifiStage >= 3 && !linkWifi) WiFi.mode(findWifiPrevMode);
  findWifi = false;
  findWifiStage = 0;
}

// loop() 呼叫：換好頻道後打開監聽，之後每 0.6 秒取一次訊號
static void pollWifiFind(uint32_t now) {
  if (!findWifi) return;
  if (findWifiStage == 1) {
    findWifiAt = now;
    if (linkWifi) {
      webpanel::setApChannel(findWifiCh);
      findWifiStage = 2;
    } else {
      findWifiPrevMode = WiFi.getMode();
      if (findWifiPrevMode == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
      esp_wifi_set_channel(findWifiCh, WIFI_SECOND_CHAN_NONE);
      roomcheck::wifiFindStart(findWifiMac);
      findWifiStage = 3;
    }
    return;
  }
  if (findWifiStage == 2) {
    if (!webpanel::channelDone && now - findWifiAt < 8000) return;
    roomcheck::wifiFindStart(findWifiMac);
    findWifiStage = 4;  // 熱點開著，結束時不改 Wi-Fi 模式
    return;
  }
  if (now - findWifiAt < 600) return;
  findWifiAt = now;
  int rssi;
  if (roomcheck::wifiFindTake(rssi)) {
    findRssi = rssi;
    findSeenAt = now;
    publishSnapshot();
  }
}

// 尋找模式的 1 秒短掃描，不計入偵測的掃描次數。掃描在背景進行，結束後由 loop() 呼叫
static void startFindScan() {
  scanIsFind = true;
  scanRunning = scan->start(1000, false);
}

static void finishFindScan() {
  NimBLEScanResults results = scan->getResults();
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    if (d->getAddress().toString() == findAddr) {
      findRssi = d->getRSSI();
      findSeenAt = millis();
    }
  }
  scan->clearResults();
  if (fakeTracker && findAddr == FAKE_ADDR) {
    findRssi = -50 - (int)(esp_random() % 20);
    findSeenAt = millis();
  }
  publishSnapshot();
}

static void pollWebEvents() {
  if (!webpanel::events) return;
  webpanel::Event e;
  bool any = false;
  while (webpanel::nextEvent(e)) {
    any = true;
    switch (e.type) {
      case webpanel::EV_MARK:
        markEvent(e.label, e.atMs);
        break;
      case webpanel::EV_TIME:
        // 手機送出的是按下那一刻的時間，扣掉排隊等待的秒數
        setTime(e.value + (millis() - e.atMs) / 1000);
        break;
      case webpanel::EV_ACK:
        acknowledgeAlerts();
        break;
      case webpanel::EV_CAPTIVE:
        prefs.putBool("captive", e.value == 1);
        Serial.printf("手機面板：自動跳出面板%s\n", e.value == 1 ? "開啟" : "關閉");
        break;
      case webpanel::EV_CLEAR:
        clearLog();
        break;
      case webpanel::EV_LINKOFF:
        applyLink(false, false);
        break;
      case webpanel::EV_LINK:
        applyLink(e.value & 1, e.value & 2);
        break;
      case webpanel::EV_STATS:
        setStatsMode(e.value == 1);
        break;
      case webpanel::EV_LOG:
        setLogEnabled(e.value == 1);
        break;
      case webpanel::EV_QUIET:
        setQuiet(e.value == 1);
        break;
      case webpanel::EV_CFG: {
        const String key = e.label;
        if (key == "preset") setPreset(e.value);
        else if (key == "interval") setScanTiming(e.value, scanDurationMs / 1000);
        else if (key == "duration") setScanTiming(scanIntervalMs / 1000, e.value);
        else if (key == "idle") setIdle(e.value);
        else if (key == "wifiscan") setWifiScan(e.value == 1);
        else if (key == "autosound") setAutoSound(e.value);
        else if (key == "roomlog") setRoomLog(e.value == 1);
        else if (key == "autoupd") e.value == 1 ? requestBootConfirm(1) : setAutoUpdate(false);
        else if (key == "testchan") e.value == 1 ? requestBootConfirm(2) : setTestChannel(false);
        break;
      }
      case webpanel::EV_SOUND:
        soundTracker(e.label);
        break;
      case webpanel::EV_BLEPIN_NEW:
        newBlePin();
        break;
      case webpanel::EV_BLE_UNPAIR:
        unpairAll();
        break;
      case webpanel::EV_OTA_ARM:
        otaRequestArm();
        break;
      case webpanel::EV_OTA_DONE:
        otaFinish(e.value, true);
        break;
      case webpanel::EV_NET_WIFI:
        setNetWifi(e.label, e.extra);
        break;
      case webpanel::EV_NET_UPDATE:
        requestNetUpdate(true, false);
        break;
      case webpanel::EV_IDENTITY:
        setIdentity(e.label, e.extra);
        break;
      case webpanel::EV_LED:
        setLedOn(e.value == 1);
        break;
      case webpanel::EV_FACTORY:
        factoryReset();
        break;
      case webpanel::EV_FIND:
        startFind(e.label);
        break;
      case webpanel::EV_PLACE_NAME:
        renamePlace(e.value, e.label);
        break;
      case webpanel::EV_PLACE_FORGET:
        forgetPlace(e.value);
        break;
      case webpanel::EV_MEET:
        if (e.value) startMeeting(-(int)e.value);
        else stopMeeting();
        break;
      case webpanel::EV_MEET_FIND:
        startMeetingFind(e.value);
        break;
      case webpanel::EV_PROBES:
        if (e.value == 1) startProbes();
        else stopProbes();
        break;
      case webpanel::EV_ROOM_FIND:
        startRoomFind(e.value);
        break;
      case webpanel::EV_HIST_CLEAR:
        clearHistoryArchive();
        break;
      case webpanel::EV_ROOM:
        if (e.value) requestRoomCheck();
        else cancelRoomCheck();
        break;
    }
  }
  if (any) {
    flushLog();
    histDirty = true;
    publishHistory();
    publishSnapshot();
  }
}

// ---- 手機連線 ------------------------------------------------------------

static void applyLink(bool wifi, bool ble) {
  if (wifi == linkWifi && ble == linkBle) return;
  // 房間檢查關掉熱點、射頻在監聽，結束後才切換
  if (roomStage >= 2 && wifi != linkWifi) {
    out.println("房間檢查中，結束後再切換 Wi-Fi");
    return;
  }
  if (wifi != linkWifi) {
    webpanel::setEnabled(wifi);
    scan->setWindow(wifi ? SCAN_WINDOW_WIFI_MS : 100);
    wifiQrUntil = 0;
    if (wifi && oledOn()) {
      // OLED 顯示 60 秒熱點的 QR code，手機相機掃描就能加入
      screen::makeQr(wifiQrText().c_str());
      wifiQrUntil = millis() + WIFI_QR_MS;
    }
    if (wifi) {
      // 自訂密碼不寫進記錄，記錄檔可能會下載或轉給別人
      out.printf("手機面板：熱點 %s，http://192.168.4.1/\n", deviceName.c_str());
      if (devicePass == WEB_PANEL_PASS) Serial.printf("熱點密碼是預設的 %s\n", WEB_PANEL_PASS);
    }
  }
  if (ble != linkBle) {
    if (ble) {
      phonelink::enable(deviceName, blePin);
      out.printf("藍牙連線：名稱 %s，配對後用藍牙終端機 App 輸入 help\n", deviceName.c_str());
      Serial.printf("藍牙配對碼：%06lu\n", (unsigned long)blePin);  // 不寫進記錄
    } else {
      phonelink::disable();
    }
  }
  linkWifi = wifi;
  linkBle = ble;
  linkLastActive = millis();
  out.printf("LINK,%lu,wifi,%s,ble,%s\n", (unsigned long)scanCount, wifi ? "on" : "off",
             ble ? "on" : "off");
  flushLog();

  // 新狀態的顏色恆亮 2 秒：Wi-Fi 青色、兩者白色、藍牙紫色、全部關閉綠色。
  // 蜂鳴器依新狀態響 1 到 3 聲，全部關閉時響一聲長音
  linkFlashR = ble ? 40 : 0;
  linkFlashG = (wifi || !ble) ? 40 : 0;
  linkFlashB = (wifi || ble) ? 40 : 0;
  linkFlashUntil = millis() + 2000;
  const int beeps = wifi && !ble ? 1 : wifi && ble ? 2 : ble ? 3 : 0;
  if (beeps == 0) {
    beep(300);
  } else {
    for (int i = 0; i < beeps; i++) {
      beep(40);
      delay(80);
    }
  }
  updateLed(millis());
  publishSnapshot();
}

// 長按 BOOT：開 Wi-Fi → 開藍牙 → 關 Wi-Fi → 關藍牙，循環
// 長按 BOOT：Wi-Fi 沒開就打開（藍牙維持原狀），開著就全部關閉
static void cycleLink() {
  if (!linkWifi) applyLink(true, linkBle);
  else applyLink(false, false);
}

// 開啟後一段時間都沒有手機連著就全部關閉
static void checkLinkIdle() {
  if (!linkWifi && !linkBle) return;
  if (roomStage) return;  // 房間檢查時熱點關著，手機連不上
  const uint32_t now = millis();
  if (webpanel::stationCount() > 0 || phonelink::connected) {
    linkLastActive = now;
    return;
  }
  if (linkIdleMs && now - linkLastActive >= linkIdleMs) {
    out.println("手機連線閒置太久，自動關閉");
    applyLink(false, false);
  }
}

static String phoneStatus() {
  bool alert = false;
  for (const auto& t : table) {
    if (t.alerted && !t.acknowledged) alert = true;
  }
  String r = TR("掃描 #") + String(scanCount) + TR("，記錄中的追蹤器 ") + String(table.size()) + TR(" 個\n");
  if (alert) r += TR("!!! 有未確認的警示，輸入 ack 確認\n");
  r += String(TR("統計模式：")) + (statsMode ? TR("開啟") : TR("關閉")) + "\n";
  if (logReady) r += TR("flash 記錄：") + String(logSize() / 1024) + " KB" + (logFull ? TR("（已滿）") : "") + "\n";
  r += timeKnown ? TR("已對時\n") : TR("還沒對時，輸入 time 加上 Unix 秒數\n");
  return r;
}

// 藍牙文字通道的指令說明。每一行各自翻譯，指令本身（status、mark…）不翻譯
static String phoneHelp() {
  return String(TR("指令：\n"))
      + TR("status  目前狀態\n")
      + TR("mark 文字  打記號，例如 mark 上車\n")
      + TR("ack  確認警示\n")
      + TR("trackers  附近的追蹤器與離警示門檻多遠\n")
      + TR("sound 位址  讓追蹤器發出聲音，省略位址時選尋找中、警示中或訊號最強的\n")
      + TR("autosound 0、1、2  自動發聲：關閉、跟著我移動時、也協助周圍的人\n")
      + TR("history  自開機以來觸發過警示的追蹤器\n")
      + TR("stats on、stats off  切換統計模式\n")
      + TR("log on、log off  flash 記錄的寫入\n")
      + TR("quiet on、quiet off  安靜模式\n")
      + TR("time 秒數  用 Unix 秒數對時\n")
      + TR("wifi  熱點的名稱、密碼與 QR 文字\n")
      + TR("wifi on、wifi off  開關 Wi-Fi 熱點與手機面板\n")
      + TR("room  房間檢查，約 2 分鐘，期間 Wi-Fi 熱點關閉\n")
      + TR("room report  上一次房間檢查的結果\n")
      + TR("off  關閉藍牙與 Wi-Fi，回到單純偵測\n");
}

static void pollPhoneCommand() {
  static bool wasSecure = false;
  if (phonelink::secure != wasSecure) {
    wasSecure = phonelink::secure;
    if (wasSecure) {
      out.printf("BLE,%lu,paired_connected\n", (unsigned long)scanCount);
      phonelink::send(TR("已配對連線到 ") + deviceName + TR("，輸入 help 看指令\n"));
      publishSnapshot();
    }
  }
  String line;
  if (!phonelink::receive(line) || !line.length()) return;
  String cmd = line, arg;
  const int sp = line.indexOf(' ');
  if (sp > 0) {
    cmd = line.substring(0, sp);
    arg = line.substring(sp + 1);
    arg.trim();
  }
  cmd.toLowerCase();
  if (cmd == "help" || cmd == "?") {
    phonelink::send(phoneHelp());
  } else if (cmd == "status") {
    phonelink::send(phoneStatus());
  } else if (cmd == "mark") {
    markEvent(arg.c_str());
    phonelink::send(TR("已記錄 MARK #") + String(markCount) + (arg.length() ? " " + arg : String("")) + "\n");
  } else if (cmd == "ack") {
    phonelink::send(acknowledgeAlerts() ? TR("已確認警示\n") : TR("目前沒有警示\n"));
  } else if (cmd == "sound") {
    std::string target = arg.c_str();
    if (target.empty()) target = findOn ? findAddr : "";
    if (target.empty()) {
      int best = -1000;
      for (const auto& t : table) {
        const int score = (t.alerted && !t.acknowledged ? 1000 : 0) + t.rssi;
        if (score > best) { best = score; target = t.addr; }
      }
    }
    if (target.empty()) {
      phonelink::send(TR("附近沒有追蹤器\n"));
    } else {
      phonelink::send(String(TR("正在連線到 ")) + target.c_str() + "…\n");
      soundTracker(target);
    }
  } else if (cmd == "history") {
    String r;
    for (const auto& h : history) {
      if (!h.alerted) continue;
      r += String(trackerShort(h.type)) + " " + h.addr.c_str() + TR("：開機後 ") + h.firstMs / 60000 + TR(" 分出現，") +
           h.alertMs / 60000 + TR(" 分警示，") + h.lastMs / 60000 + TR(" 分最後一次，最強 ") + h.maxRssi + " dBm" +
           (h.acked ? TR("（已確認）") : "") + "\n";
    }
    phonelink::send(r.length() ? r : String(TR("自開機以來沒有觸發過警示\n")));
  } else if (cmd == "trackers") {
    if (table.empty()) {
      phonelink::send(TR("附近沒有追蹤器\n"));
    } else {
      String r;
      const uint32_t now = millis();
      for (const auto& t : table) {
        const uint32_t since = scanCount - t.firstScan + 1;
        r += String(t.alerted ? (t.acknowledged ? TR("[已確認] ") : TR("[警示] ")) : "") + trackerShort(t.type) + " " +
             t.addr.c_str() + " " + t.rssi + TR(" dBm，") + (t.lastSeen - t.firstSeen) / 60000 + TR(" 分，近距離 ") +
             t.nearWindows + "/" + alertMinWindows + TR("，比例 ") + t.windows * 100 / since + TR("%，") +
             (now - t.lastSeen) / 1000 + TR(" 秒前\n");
      }
      phonelink::send(r);
    }
  } else if (cmd == "autosound" && (arg == "on" || arg == "off" || arg == "0" || arg == "1" || arg == "2")) {
    setAutoSound(arg == "on" ? 1 : arg == "off" ? 0 : arg.toInt());
    static const char* const NAMES[] = {TR("關閉"), TR("跟著我移動時"), TR("也協助周圍的人")};
    phonelink::send(String(TR("自動發聲：")) + NAMES[autoMode] + (autoMode && !wifiScanOn ? TR("（移動判斷沒開，不會作用）") : "") + "\n");
  } else if ((cmd == "log" || cmd == "quiet") && (arg == "on" || arg == "off")) {
    if (cmd == "log") setLogEnabled(arg == "on");
    else setQuiet(arg == "on");
    phonelink::send(cmd + " " + arg + "\n");
  } else if (cmd == "stats" && (arg == "on" || arg == "off")) {
    setStatsMode(arg == "on");
    phonelink::send(String(TR("統計模式：")) + (statsMode ? TR("開啟") : TR("關閉")) + "\n");
  } else if (cmd == "time") {
    const long epoch = arg.toInt();
    if (epoch > 1700000000L) {
      setTime((uint32_t)epoch);
      phonelink::send(TR("已對時\n"));
    } else {
      phonelink::send(TR("用法：time 1795500000\n"));
    }
  } else if (cmd == "wifi" && arg.length() == 0) {
    // 只在加密的配對連線上送出密碼
    phonelink::send(TR("熱點名稱：") + deviceName + TR("\n密碼：") + devicePass + TR("\n目前") +
                    (linkWifi ? TR("開啟，網址 http://192.168.4.1") : TR("關閉，輸入 wifi on 開啟")) +
                    TR("\n用 QR code 產生器把下面這行做成 QR code，手機相機掃描就能加入：\n") + wifiQrText() + "\n");
  } else if (cmd == "wifi" && (arg == "on" || arg == "off")) {
    applyLink(arg == "on", linkBle);
    phonelink::send(arg == "on" ? TR("Wi-Fi 熱點與手機面板已開啟\n") : TR("Wi-Fi 熱點已關閉\n"));
  } else if (cmd == "room" && arg.length() == 0) {
    phonelink::send(requestRoomCheck() ? TR("開始房間檢查，約 2 分鐘，完成後輸入 room report 看結果\n")
                                       : TR("現在不能開始房間檢查（正在檢查、更新韌體或發聲）\n"));
  } else if (cmd == "room" && arg == "stop") {
    cancelRoomCheck();
    phonelink::send(TR("已取消房間檢查\n"));
  } else if (cmd == "room" && arg == "report") {
    String r;
    if (roomcheck::state != roomcheck::DONE) {
      r = roomStage ? TR("房間檢查進行中\n") : TR("還沒有房間檢查的結果\n");
    } else {
      r = String(TR("基地台 ")) + roomcheck::apCount + TR(" 個、連網裝置 ") + roomcheck::staCount + TR(" 個、藍牙 ") +
          roomcheck::bleTotal + TR(" 個\n");
      int n = 0;
      for (const auto& it : roomcheck::items) {
        if (it.level == 0) continue;
        String why = it.why;
        why.trim();
        why.replace("\n", TR("；"));
        r += String(it.level == 2 ? TR("[建議找出來] ") : TR("[可以留意] ")) + it.name + (it.sub.length() ? TR("，") + it.sub : "") +
             TR("，") + it.rssi + TR(" dBm：") + why + "\n";
        if (++n >= 10) break;
      }
      if (!n) r += TR("沒有找到符合規則的裝置\n");
    }
    phonelink::send(r);
  } else if (cmd == "off") {
    phonelink::send(TR("關閉連線，回到單純偵測\n"));
    delay(200);
    applyLink(false, false);
  } else {
    phonelink::send(TR("不認得的指令，輸入 help 看指令\n"));
  }
  flushLog();
  publishSnapshot();
}

// ---- 掃描 ----------------------------------------------------------------

static void finishScanWindow();
static void finishFindScan();

// ---- 讓追蹤器發出聲音 ----------------------------------------------------

// 連上追蹤器的 DULT 非擁有者服務送出 Sound_Start。過程中 loop() 會停住約 5 到 20 秒，
// 手機面板在另一個核心照常回應
// 測試用：主動掃描 5 秒，找到廣播四種發聲服務之一的裝置（例如用電腦模擬的追蹤器），
// 對它執行發聲流程。真的追蹤器不會在廣播裡列出這些服務，所以只會找到模擬的裝置
static void soundTest() {
  if (scanRunning) {
    scan->stop();
    scanRunning = false;
    if (scanIsFind) finishFindScan();
    else finishScanWindow();
  }
  static const NimBLEUUID uuids[] = {trackersound::SVC_UUID, trackersound::AIRTAG_SVC, trackersound::FINDMY_SVC,
                                     trackersound::PEBBLEBEE_SVC};
  scan->setActiveScan(true);
  NimBLEScanResults r = scan->getResults(5000, false);
  std::string addr;
  for (int i = 0; i < r.getCount() && addr.empty(); i++) {
    for (const auto& u : uuids) {
      if (r.getDevice(i)->isAdvertisingService(u)) addr = r.getDevice(i)->getAddress().toString();
    }
  }
  scan->clearResults();
  scan->setActiveScan(false);
  if (addr.empty()) {
    Serial.println("SOUNDTEST,none");
    return;
  }
  Serial.printf("SOUNDTEST,found,%s\n", addr.c_str());
  soundTracker(addr);
  Serial.printf("SOUNDTEST,result,%s,%s\n", trackersound::lastProtocol, soundMsg.c_str());
}

static trackersound::Result soundTracker(const std::string& addr) {
  if (addr.empty() || soundBusy) return trackersound::NOT_FOUND;
  soundAddr = addr;
  soundAt = millis();
  const char* logAddr = CROWD_MODE ? "-" : addr.c_str();  // 人流計數模式不寫原始位址
  if (addr == FAKE_ADDR) {
    soundResult = trackersound::OK;
    soundShowUntil = millis() + SOUND_SHOW_MS;
    soundMsg = TR("假追蹤器沒辦法真的發出聲音，這是測試用的模擬結果");
    out.printf("SOUND,%lu,%s,fake\n", (unsigned long)scanCount, logAddr);
    publishSnapshot();
    return trackersound::OK;
  }
  if (scanRunning) {
    scan->stop();
    scanRunning = false;
    if (scanIsFind) finishFindScan();
    else finishScanWindow();
  }
  soundBusy = true;
  soundMsg = TR("尋找並連線到追蹤器中…");
  publishSnapshot();
  pollScreen(true);  // 接下來 loop() 會停住幾秒，先畫好「連線中」
  writeLed(32, 0, 32);

  uint16_t status = 0;
  TrackHist* hist = findHistory(addr);
  const trackersound::Hint hint = hist ? (trackersound::Hint)hist->hint : trackersound::HINT_GENERAL;
  const trackersound::Result r = trackersound::ring(scan, addr, 4000, 12000, status, hint);
  soundBusy = false;
  soundAt = millis();
  soundResult = (int8_t)r;
  soundShowUntil = soundAt + SOUND_SHOW_MS;
  if (TrackHist* h = findHistory(addr)) {
    h->sound = (int8_t)r;
    histDirty = true;
  }
  switch (r) {
    case trackersound::OK:
      soundMsg = String(TR("已送出發聲指令（")) + trackersound::lastProtocol + TR(" 協定），追蹤器會響約 10 秒，循著聲音找找看");
      break;
    case trackersound::NOT_FOUND: soundMsg = TR("4 秒內沒有收到這個追蹤器的廣播，可能已經換了位址或離開範圍"); break;
    case trackersound::CONNECT_FAIL:
      soundMsg = TR("連不上這個追蹤器（錯誤碼 ") + String(trackersound::lastError) + TR("），靠近一點再試一次");
      break;
    case trackersound::NOT_CONNECTABLE: soundMsg = TR("這個追蹤器目前的廣播不接受連線，沒辦法讓它發聲"); break;
    case trackersound::SKIPPED: soundMsg = TR("不在自動發聲的範圍"); break;
    case trackersound::NO_SERVICE: soundMsg = TR("連上了，但沒有支援的發聲服務（DULT、AirTag、Find My、Pebblebee）"); break;
    case trackersound::WRITE_FAIL: soundMsg = TR("送出發聲指令失敗"); break;
    case trackersound::NO_RESPONSE: soundMsg = TR("送出指令後追蹤器沒有回覆"); break;
    case trackersound::REFUSED: soundMsg = String(TR("追蹤器拒絕：")) + trackersound::statusText(status); break;
  }
  // SOUND,掃描次數,位址,結果代碼,狀態碼,連線錯誤碼,協定
  out.printf("SOUND,%lu,%s,%d,0x%04X,%d,%s\n", (unsigned long)scanCount, logAddr, (int)r, status,
             trackersound::lastError, trackersound::lastProtocol);
  flushLog();
  phonelink::send(soundMsg + "\n");
  publishSnapshot();
  updateLed(millis());
  return r;
}

// 每次掃描完，對一個觸發過警示、這次也有出現的追蹤器自動發聲。找不到、連不上、沒回覆時
// 之後的掃描再試，最多 AUTO_SOUND_TRIES 次。成功、不支援或明確拒絕就不再嘗試。
// 一次掃描只處理一個（發聲會讓主程式停住數秒），優先挑嘗試次數最少的，
// 一直連不上的追蹤器不會卡住其他追蹤器
static void processAutoSound(uint32_t scanEndMs) {
  if (autoMode == 0) return;
  AutoSoundRec* pick = nullptr;
  for (auto& a : autoSoundList) {
    if (a.done || a.attempts >= AUTO_SOUND_TRIES) continue;
    bool present = false;
    for (const auto& t : table) {
      // 協助周圍的人，發聲時也要在很近的距離
      if (t.addr == a.addr && scanEndMs - t.lastSeen < 2000 && (!a.help || t.rssi >= HELP_RSSI)) present = true;
    }
    if (!present) continue;
    // 只對可能響得起來的追蹤器發聲：收過 DULT 廣播的、AirTag、Pebblebee，而且廣播接受連線。
    // 2026-11-25 實測，其他 Apple 裝置 336 次嘗試沒有一次成功，去連它們只會讓偵測停下來，
    // 又打擾到陌生人的裝置。AirPods 可能正戴在別人耳朵上，不自動發聲
    TrackHist* h = findHistory(a.addr);
    const bool capable = h && h->connectable &&
                         (h->dult || h->hint == trackersound::HINT_AIRTAG || h->type == TrackerType::Pebblebee);
    if (!capable) {
      a.done = true;
      if (h) {
        h->sound = trackersound::SKIPPED;
        histDirty = true;
      }
      out.printf("AUTOSOUND,%lu,%s,skip\n", (unsigned long)scanCount, CROWD_MODE ? "-" : a.addr.c_str());
      continue;
    }
    if (!pick || a.attempts < pick->attempts) pick = &a;  // 次數相同時維持觸發警示的先後
  }
  if (!pick) return;
  pick->attempts++;
  // AUTOSOUND,掃描次數,位址,try 次數,follow（跟著你）或 help（協助周圍的人）
  out.printf("AUTOSOUND,%lu,%s,try %u,%s\n", (unsigned long)scanCount, CROWD_MODE ? "-" : pick->addr.c_str(),
             pick->attempts, pick->help ? "help" : "follow");
  // 先提醒帶著板子的人：附近有追蹤器要響了，可以留意四周
  if (pick->attempts == 1) {
    for (int i = 0; i < 2; i++) {
      beep(60);
      delay(120);
    }
    phonelink::send(String(TR("自動發聲：讓附近的")) + (pick->help ? TR("追蹤器（協助周圍的人）") : TR("追蹤器")) + TR("響，留意四周\n"));
  }
  const trackersound::Result r = soundTracker(pick->addr);
  pick->done = r == trackersound::OK || r == trackersound::NO_SERVICE || r == trackersound::REFUSED ||
               r == trackersound::NOT_CONNECTABLE;
}

// 掃描在背景進行，loop() 照常處理按鍵、面板與燈號，掃描結束後呼叫 finishScanWindow()
static void startScanWindow() {
  scanCount++;
  scanIsFind = false;
  scanRunning = scan->start(scanDurationMs, false);
  if (!scanRunning) finishScanWindow();
}

static uint32_t dultSeparated = 0, dultNearSkipped = 0;  // 這次掃描的 DULT 統計

static void finishScanWindow() {
  dultSeparated = dultNearSkipped = 0;
  // 每次掃描內，同一個位址只回報一次（重複過濾），所以 windows 等於出現的掃描次數
  NimBLEScanResults results = scan->getResults();
  const uint32_t now = millis();

  if (VERBOSE) out.printf("[掃描 #%lu] 收到 %d 個裝置\n",
                             (unsigned long)scanCount, results.getCount());
  bleNowCount = results.getCount();

  if (statsMode) {
    for (int k = 0; k < KIND_COUNT; k++) nowCount[k] = persistCount[k] = 0;
  }
  if (CROWD_MODE) beginCrowdScan();

  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    if (statsMode) recordDevice(d, now);
    if (CROWD_MODE) recordCrowd(d);
    meetingBle(d, now);
    TrackerType t = classify(d);
    // 同一個位址最近的 DULT 廣播：主人在附近就不算追蹤器（Apple 自己的格式已經標示與主人
    // 分開時除外），與主人分開而自家格式認不出來時當成 DULT 追蹤器
    DultSeen ds;
    if (dultLookup(d->getAddress(), ds, now)) {
      if (ds.nearOwner && t != TrackerType::AppleFindMySeparated) {
        if (t != TrackerType::None) dultNearSkipped++;
        continue;
      }
      if (!ds.nearOwner) {
        dultSeparated++;
        if (t == TrackerType::None) t = TrackerType::Dult;
      }
    }
    if (t == TrackerType::None) continue;
    // 人流計數模式不印路過追蹤器的位址
    if (VERBOSE && !CROWD_MODE) printVerbose(d, t);
    recordSighting(d->getAddress().toString(), t, d->getRSSI(), now);
    updateHistory(d->getAddress().toString(), t, d->getRSSI(), now);
    if (TrackHist* h = findHistory(d->getAddress().toString())) {
      uint8_t net;
      bool nearOwner;
      if (t == TrackerType::Dult || parseDult(d, net, nearOwner) || dultLookup(d->getAddress(), ds, now)) h->dult = true;
      h->connectable = d->isConnectable();
      // Apple 廣播的狀態 byte（4C 00 12 19 之後那個）的位元 4、5 是裝置類型：
      // 0 Apple 自家裝置、1 AirTag、2 Find My 相容配件、3 AirPods（PETS 2023〈Track You〉表 4）
      if ((t == TrackerType::AppleFindMySeparated || t == TrackerType::AppleDevice) && d->haveManufacturerData()) {
        const std::string m = d->getManufacturerData();
        if (m.size() >= 5) {
          const uint8_t kind = ((uint8_t)m[4] >> 4) & 0x03;
          h->hint = kind == 1 ? trackersound::HINT_AIRTAG : kind == 3 ? trackersound::HINT_AIRPODS
                                                                     : trackersound::HINT_GENERAL;
        }
      }
    }
    if (findOn && !findWifi && d->getAddress().toString() == findAddr) {
      findRssi = d->getRSSI();
      findSeenAt = now;
    }
  }

  scan->clearResults();
  if (fakeTracker) {
    const int rssi = -55 - (int)(esp_random() % 15);
    recordSighting(FAKE_ADDR, TrackerType::AppleFindMySeparated, rssi, now);
    updateHistory(FAKE_ADDR, TrackerType::AppleFindMySeparated, rssi, now);
    if (findOn && findAddr == FAKE_ADDR) {
      findRssi = rssi;
      findSeenAt = now;
    }
    out.printf("FAKE,%lu,%s,%d\n", (unsigned long)scanCount, FAKE_ADDR, rssi);
  }
  scanWifi();
  if (timeKnown) out.printf("CLOCK,%lu,%lu\n", (unsigned long)scanCount, (unsigned long)epochNow());
  forgetStale(now);
  evaluate(now);
  if (statsMode) {
    forgetStaleDevices(now);
    printStatsRow(now);
    printPersistent(now);
  } else if (CROWD_MODE) {
    endCrowdScan();
  }
  screenDirty = true;

  // DULT,掃描次數,與主人分開的,因為主人在附近而略過的
  if (dultSeparated || dultNearSkipped) {
    out.printf("DULT,%lu,%lu,%lu\n", (unsigned long)scanCount, (unsigned long)dultSeparated,
               (unsigned long)dultNearSkipped);
  }
  out.printf("[掃描 #%lu] 記錄中的追蹤器 %u 個\n", (unsigned long)scanCount,
                (unsigned)table.size());
  flushLog();
  histDirty = true;  // 時間軸的「現在」每次掃描往前推
  publishHistory();
  publishSnapshot();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  prefs.begin("tracker", false);
  loadConfig();
  buzzerOn = prefs.getBool("buzzer", BUZZER_ENABLED);
  ledOn = prefs.getBool("led", true);
  blePin = prefs.getUInt("blepin", 0);
  if (blePin < 100000 || blePin > 999999) {
    blePin = 100000 + esp_random() % 900000;
    prefs.putUInt("blepin", blePin);
  }
  setupFlashLog(prefs.getBool("log", LOG_TO_FLASH));
  if (!logMounted) mountLog(true);  // 警示歷史也存在同一個 FAT 分割區，記錄關著也要掛載
  out.println("BLE 追蹤器偵測器啟動");
  if (logReady) {
    out.printf("flash 記錄：已有 %u bytes，新的紀錄接在後面\n", (unsigned)logSize());
  }
  statsMode = prefs.getBool("stats", STATS_MODE) && !CROWD_MODE;
  if (statsMode) {
    out.println("統計模式：開啟（送出 stats off 關閉）");
    printStatsHeader();
  }
  printCfg();

  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  if (BUZZER_GPIO >= 0) {
    // 先設成不響的電位再切成輸出，避免開機時誤響
    digitalWrite(BUZZER_GPIO, !BUZZER_ON);
    pinMode(BUZZER_GPIO, OUTPUT);
    beep(50);  // 開機短響一聲，確認蜂鳴器接對
  }

  setupOled();
  showMessage("BLE tracker detector", "starting...");

  setupCrowd();
  setupWifiScan();
  if (logMounted) {
    webpanel::logPath = LOG_PATH;
    webpanel::histPrevPath = HIST_PREV;
  }
  archiveHistory();
  loadPlaces();
  deviceName = prefs.getString("apname", "");
  String nameSuffix = prefs.getString("namesfx", "");
  if (nameSuffix.length() != 4) {
    char sfx[5];
    snprintf(sfx, sizeof(sfx), "%04X", (unsigned)(esp_random() & 0xFFFF));
    nameSuffix = sfx;
    prefs.putString("namesfx", nameSuffix);
  }
  if (!webpanel::validName(deviceName)) deviceName = webpanel::defaultName(WEB_PANEL_PREFIX, nameSuffix);
  devicePass = prefs.getString("appass", WEB_PANEL_PASS);
  netSsid = prefs.getString("netssid", "");
  netPass = prefs.getString("netpass", "");
  if (!webpanel::validPass(devicePass)) devicePass = WEB_PANEL_PASS;
  webpanel::configure(deviceName, devicePass, wifiScanOn, prefs.getBool("captive", true));
  NimBLEDevice::init("");
  // 對外一律用 RPA（可解析的隨機位址），約 15 分鐘換一次，跟手機的做法相同。配對時把解析
  // 金鑰（IRK）交給手機，配對過的手機仍認得出這塊板子。主動連線追蹤器發聲時也用 RPA
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  const bool rpaOk = NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT);
  selfAddr = NimBLEDevice::getAddress().toString();
  out.printf("這塊偵測器的藍牙識別位址：%s（對外%s）\n", selfAddr.c_str(),
             rpaOk ? "使用定期更換的隨機位址" : "的隨機位址設定失敗，改用這個固定位址");
  if (oledReady) {
    oled.clearBuffer();
    oled.drawStr(0, 0, "BLE tracker detector");
    oled.drawStr(0, 20, "this board:");
    oled.drawStr(0, 32, selfAddr.c_str());
    oled.sendBuffer();
    delay(3000);
  }
  scan = NimBLEDevice::getScan();
  scan->setActiveScan(false);  // 被動掃描，不送出掃描請求
  scan->setScanCallbacks(new DultScanCB(), true);  // 每一則廣播都回呼，DULT 格式才不會漏掉
  scan->setInterval(100);
  scan->setWindow(100);

  const esp_partition_t* runPart = esp_ota_get_running_partition();
  out.printf("BUILD,%s,%s,%s\n", BUILD_ID, runPart ? runPart->label : "?", FW_VERSION);
  out.printf("LANG,%s\n", FW_LANG);
  logMem("boot");
  // 上次是自動更新：換上的是剛下載的版本就記下來源，不是的話之後不再自動安裝那個版本
  const String autoTo = prefs.getString("autoto", "");
  if (autoTo.length()) {
    const String from = prefs.getString("autofrom", "");
    prefs.remove("autoto");
    prefs.remove("autofrom");
    if (autoTo == BUILD_ID) {
      autoFrom = from;
      prefs.remove("autoskip");
      out.printf("OTA,0,auto_ok,%s,from %s\n", FW_VERSION, from.c_str());
    } else {
      prefs.putString("autoskip", autoTo);
      out.printf("OTA,0,auto_mismatch,%s\n", autoTo.c_str());
    }
  }
  autoSkip = prefs.getString("autoskip", "");
  autoNextAt = millis() + AUTO_FIRST_MS;
  if (prefs.getBool("otaboot", false)) {
    prefs.remove("otaboot");
    out.println("韌體更新後的第一次開機，自動打開 Wi-Fi 面板確認版本");
    applyLink(true, false);
  } else if (WEB_PANEL) {
    applyLink(true, false);
  }
  out.println("長按 BOOT 2 秒開啟手機連線");
  updateLed(millis());
  flushLog();
  publishSnapshot();
}

// 尋找時的嗶聲：訊號 -45 dBm 以上每 0.1 秒一聲，-95 dBm 以下每 1.2 秒一聲，像蓋革計數器。
// 太久沒收到就不響，跟「離很遠」的慢節奏分開。安靜模式時 beep() 不響，只有燈號
static void pollFindTicks(uint32_t now) {
  if (!findOn || !findSeenAt || now - findSeenAt > FIND_STALE_MS) return;
  const int r = constrain(findRssi, -95, -45);
  const uint32_t interval = 100 + (uint32_t)(-45 - r) * 1100 / 50;
  if (now - findTickAt < interval) return;
  findTickAt = now;
  beep(FIND_TICK_MS);
}

void loop() {
  pollSerialCommand();
  pollWebEvents();
  pollPhoneCommand();

  // 短按：確認警示或打記號，雙擊：開始或結束尋找。短按要等 0.35 秒確定不是雙擊才執行，
  // 記號的時間用第一次按下的那一刻。長按 2 秒：切換手機連線，到時間就觸發，不必等放開
  // 長按 10 秒：恢復原廠設定。5 秒起燈號快閃黃色提示，到 10 秒改成白色快閃，放開才執行
  // 燈號回饋在 updateLed() 依 buttonPressedAt 顯示
  static bool longFired = false;
  static uint8_t clickCount = 0;
  static uint32_t clickPressAt = 0, clickReleaseAt = 0;
  if (digitalRead(BUTTON_GPIO) == LOW) {
    if (buttonPressedAt == 0) {
      buttonPressedAt = millis();
      longFired = false;
    } else if (!longFired && millis() - buttonPressedAt >= LONG_PRESS_MS) {
      longFired = true;
      if (!roomStage) cycleLink();
    }
  } else if (buttonPressedAt != 0) {
    const uint32_t held = millis() - buttonPressedAt;
    buttonPressedAt = 0;
    if (held >= FACTORY_PRESS_MS) factoryReset();
    else if (!longFired && held >= 30 && (roomPending || roomStage)) cancelRoomCheck();
    else if (!longFired && held >= 30 && otaWaitUntil && (int32_t)(otaWaitUntil - millis()) > 0) bootConfirmed();
    else if (!longFired && held >= 30) {
      if (clickCount++ == 0) clickPressAt = millis() - held;
      clickReleaseAt = millis();
    }
  }

  if (clickCount && buttonPressedAt == 0 && millis() - clickReleaseAt >= DOUBLE_CLICK_MS) {
    if (clickCount >= 2) {
      toggleFind();
    } else {
      roomDoneUntil = 0;
      if (!acknowledgeAlerts()) markEvent("", clickPressAt);
    }
    clickCount = 0;
  }

  static uint32_t lastIdleCheck = 0;
  if (millis() - lastIdleCheck >= 1000) {
    lastIdleCheck = millis();
    checkLinkIdle();
  }

  const uint32_t now = millis();
  // 手機剛連入熱點，先讓射頻給 Wi-Fi
  const bool joining = linkWifi && webpanel::lastJoinMs && now - webpanel::lastJoinMs < JOIN_GRACE_MS;
  if (otaWaitUntil && (int32_t)(otaWaitUntil - now) <= 0) {
    otaWaitUntil = 0;
    bootWhat = 0;
    out.printf("OTA,%lu,wait_timeout\n", (unsigned long)scanCount);
  }
  pollNetUpdate();
  pollAutoUpdate(now);
  if (now - memLoggedAt >= MEM_LOG_MS) logMem("tick");
  savePlaces(false);
  pollRoomCheck();
  pollProbes(now);
  pollMeeting(now);
  // 等待按 BOOT 與開放更新期間，面板的倒數每秒更新
  static uint32_t lastOtaTick = 0;
  const bool armed = webpanel::otaArmedUntil && (int32_t)(webpanel::otaArmedUntil - millis()) > -1500;
  if ((otaWaitUntil || armed) && millis() - lastOtaTick >= 1000) {
    lastOtaTick = millis();
    publishSnapshot();
  }
  // 打記號、確認警示、發聲結果不必等下一次掃描才存檔，打完記號馬上拔線也不會漏
  if (!CROWD_MODE && logMounted && histSig() != histSavedSig && (!history.empty() || !allMarks.empty())) {
    histDirty = true;
    publishHistory();
  }
  if (scanRunning) {
    if (!scan->isScanning()) {
      scanRunning = false;
      if (scanIsFind) {
        finishFindScan();
      } else {
        finishScanWindow();
        processAutoSound(millis());
      }
    }
  } else if (webpanel::otaActive || joining || roomPending || roomStage) {
    // 上傳韌體、手機剛連入或房間檢查時，掃描延後，射頻留給 Wi-Fi
  } else if (lastScanAt == 0 || now - lastScanAt >= (meet.running() ? MEET_SCAN_MS : scanIntervalMs)) {
    lastScanAt = now;
    startScanWindow();
  } else if (findOn) {
    if (now - findStartAt >= FIND_TIMEOUT_MS) {
      startFind("");
      flushLog();
      publishSnapshot();
    } else if (!findWifi && now - lastScanAt < scanIntervalMs - 1500) {
      startFindScan();  // 下一次正式掃描前 1.5 秒停下，不影響偵測的節奏
    }
  }
  pollWifiFind(millis());
  pollFindTicks(millis());
  pollScreen(false);
  updateLed(millis());
  delay(20);
}
