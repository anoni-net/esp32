// 房間檢查：旅館、民宿的攝影機與無線裝置檢查。
//
// 主程式先用藍牙主動掃描 15 秒收集附近的藍牙裝置（addBle），關掉熱點之後呼叫 startWifi()。
// Wi-Fi 的部分在自己的工作執行：先掃描基地台，記下有基地台的頻道，再把射頻切到監聽模式
// （promiscuous），輪流在這些頻道停留，經過 PASSES 輪。每個連網裝置記下每一輪送出（上傳）與
// 收到（下載）的資料量，除以停在它那個頻道的時間就是速率。影像串流的上傳量穩定而且大，
// 每一輪都在送、送出遠多於收到，是最直接的線索。
//
// 判斷只用三種線索，報告裡每個裝置都列出依據，讓使用者自己決定要不要找出來：
//   1. MAC 位址前三碼（OUI）或藍牙的公司代碼、服務 UUID 屬於攝影機品牌或 IoT 模組廠
//      （roomcheck_ids.h，由 tools/make_roomcheck_ids.py 從 IEEE 與藍牙 SIG 的登記資料產生）
//   2. 持續上傳
//   3. 熱點或藍牙的名稱像攝影機
//
// 隱私：監聽會收到房內與隔壁所有裝置的 MAC 位址。報告與面板只顯示廠商、名稱與判斷理由，
// MAC 位址只在主程式開啟「記錄房間檢查」時才寫進 flash 記錄。報告不觸發蜂鳴器，也不算進警示。
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include "esp_wifi.h"
#include "i18n.h"
#include "roomcheck_ids.h"
#include <algorithm>
#include <vector>

namespace roomcheck {

static const uint32_t BLE_MS = 15000;           // 藍牙主動掃描的長度
static const uint8_t PASSES = 3;                // 每個頻道輪流停留幾輪
static const uint32_t LISTEN_BUDGET_MS = 105000;  // 監聽的總時間，依頻道數分給每一次停留
static const uint32_t DWELL_MIN_MS = 2500, DWELL_MAX_MS = 15000;
static const uint32_t EST_SEC = 130;            // 面板顯示的預估時間（藍牙、基地台掃描、監聽、恢復）
// 持續上傳的門檻：每秒 12 KB（約 96 kbps），比低畫質的影像串流還低一些
static const uint32_t UP_STEADY_BPS = 12000;
static const int MAX_STA = 96, MAX_AP = 48, MAX_BLE = 32;

enum State : uint8_t { IDLE, BLE, WIFI, DONE, CANCELLED };

struct Sta {
  uint8_t mac[6], bssid[6];
  uint8_t ch;  // 它所連的基地台的頻道，在其他頻道收到的（相鄰頻道漏過來的）不計
  int8_t rssi;
  uint32_t up[PASSES], down[PASSES], frames;
};

struct Ap {
  uint8_t bssid[6];
  char ssid[33];
  uint8_t ch;
  int8_t rssi;
};

struct BleDev {
  uint8_t addr[6];
  char name[30];
  int8_t rssi;
  int8_t vendor;     // VENDORS 的編號，-1 代表沒有比對到
  char match[12];    // 名稱裡比對到的字
};

struct Item {
  uint8_t kind;   // 0 連網裝置、1 基地台、2 藍牙
  uint8_t level;  // 2 建議找出來、1 可以留意、0 一般裝置
  int8_t rssi;
  uint8_t ch;
  uint8_t mac[6];
  uint32_t upBps;
  String name, sub, why;  // why 用換行分隔每一條理由
};

static volatile State state = IDLE;
static volatile bool cancelReq = false;
static volatile uint8_t progress = 0;  // 0 到 100
static uint32_t startedAt = 0, finishedAt = 0;

static Sta stas[MAX_STA];
static volatile int staCount = 0;
static volatile uint32_t staDropped = 0;
static Ap aps[MAX_AP];
static int apCount = 0;
static BleDev bles[MAX_BLE];
static int bleCount = 0, bleTotal = 0;
static uint32_t visitMs[PASSES][15];
static uint8_t chans[14];
static int chanCount = 0;
static volatile uint8_t curPass = 0, curCh = 0;
static volatile bool listening = false;
static uint8_t selfMac[6];
static std::vector<Item> items;

static bool running() { return state == BLE || state == WIFI; }

// ---- 比對 ----------------------------------------------------------------

static int lookup(const uint32_t* table, size_t n, uint32_t key) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const uint32_t k = table[mid] >> 8;
    if (k == key) return table[mid] & 0xFF;
    if (k < key) lo = mid + 1;
    else hi = mid;
  }
  return -1;
}

// 第一個 byte 的位元 1 是「本地管理」：手機、電腦連 Wi-Fi 時多半用隨機產生的這種位址
static bool localAdmin(const uint8_t* mac) { return mac[0] & 0x02; }

static int ouiVendor(const uint8_t* mac) {
  if (localAdmin(mac)) return -1;
  return lookup(OUI_TABLE, sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]),
                (uint32_t)mac[0] << 16 | (uint32_t)mac[1] << 8 | mac[2]);
}

// 名稱像攝影機的熱點或藍牙裝置。傳回比對到的字，沒有就是 nullptr
static const char* cameraWord(const char* name) {
  String n = name;
  n.toLowerCase();
  static const char* WORDS[] = {"ipcam", "ipc", "camera", "wificam", "cam", "dvr", "v380", "yoosee", "ezviz", "imou",
                                "lookcam", "spy"};
  for (const char* w : WORDS) {
    int i = n.indexOf(w);
    // camp、campus 之類的不算
    while (i >= 0 && strcmp(w, "cam") == 0 && n.length() > (unsigned)i + 3 && n[i + 3] == 'p') i = n.indexOf(w, i + 1);
    if (i >= 0) return w;
  }
  // V380 系列的熱點叫 MV 加上一串數字
  if (n.startsWith("mv") && n.length() >= 8) {
    bool digits = true;
    for (size_t k = 2; k < n.length(); k++) digits &= isDigit(n[k]);
    if (digits) return TR("MV＋數字");
  }
  return nullptr;
}

// Espressif 晶片當熱點時預設的名稱：ESP_ 加上 MAC 末六碼
static bool espDefault(const char* ssid) {
  return strncmp(ssid, "ESP_", 4) == 0 && strlen(ssid) == 10;
}

// ---- 藍牙 ----------------------------------------------------------------

static void beginBle() {
  bleCount = bleTotal = 0;
  staCount = 0;
  staDropped = 0;
  apCount = chanCount = 0;
  items.clear();
  memset(visitMs, 0, sizeof(visitMs));
  cancelReq = false;
  progress = 0;
  startedAt = millis();
  state = BLE;
}

static void addBle(const NimBLEAdvertisedDevice* d) {
  bleTotal++;
  int vendor = -1;
  if (d->haveManufacturerData()) {
    const std::string m = d->getManufacturerData();
    if (m.size() >= 2) {
      const uint16_t company = (uint8_t)m[0] | (uint16_t)(uint8_t)m[1] << 8;
      vendor = lookup(BLE_COMPANY, sizeof(BLE_COMPANY) / sizeof(BLE_COMPANY[0]), company);
    }
  }
  for (int i = 0; vendor < 0 && i < d->getServiceUUIDCount(); i++) {
    const NimBLEUUID u = d->getServiceUUID(i);
    if (u.bitSize() == 16) {
      vendor = lookup(BLE_UUID16, sizeof(BLE_UUID16) / sizeof(BLE_UUID16[0]), *(const uint16_t*)u.getValue());
    }
  }
  for (int i = 0; vendor < 0 && i < d->getServiceDataCount(); i++) {
    const NimBLEUUID u = d->getServiceDataUUID(i);
    if (u.bitSize() == 16) {
      vendor = lookup(BLE_UUID16, sizeof(BLE_UUID16) / sizeof(BLE_UUID16[0]), *(const uint16_t*)u.getValue());
    }
  }
  const std::string name = d->haveName() ? d->getName() : "";
  const char* word = name.empty() ? nullptr : cameraWord(name.c_str());
  if (vendor < 0 && !word) return;
  if (bleCount >= MAX_BLE) return;
  BleDev& b = bles[bleCount++];
  memcpy(b.addr, d->getAddress().getVal(), 6);
  strlcpy(b.name, name.c_str(), sizeof(b.name));
  b.rssi = d->getRSSI();
  b.vendor = vendor;
  strlcpy(b.match, word ? word : "", sizeof(b.match));
}

// ---- Wi-Fi 監聽 ----------------------------------------------------------

static int findAp(const uint8_t* bssid) {
  for (int i = 0; i < apCount; i++) {
    if (memcmp(aps[i].bssid, bssid, 6) == 0) return i;
  }
  return -1;
}

// 在 Wi-Fi 的工作裡執行：不配置記憶體、不印東西，只更新固定大小的表
static void onPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!listening || type != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  const uint16_t len = p->rx_ctrl.sig_len;
  if (len < 24) return;
  const uint8_t* f = p->payload;
  const bool toDs = f[1] & 0x01, fromDs = f[1] & 0x02;
  const uint8_t *sta, *bssid;
  bool up;
  if (toDs && !fromDs) {  // 裝置送給基地台：上傳
    bssid = f + 4;
    sta = f + 10;
    up = true;
  } else if (!toDs && fromDs) {  // 基地台送給裝置：下載
    sta = f + 4;
    bssid = f + 10;
    up = false;
  } else {
    return;
  }
  if (sta[0] & 0x01) return;  // 廣播、群播
  if (memcmp(sta, selfMac, 6) == 0) return;
  const uint8_t ch = curCh, pass = curPass;
  int i = 0;
  const int n = staCount;
  for (; i < n; i++) {
    if (memcmp(stas[i].mac, sta, 6) == 0) break;
  }
  if (i == n) {
    if (findAp(sta) >= 0) return;  // 基地台自己
    if (n >= MAX_STA) {
      staDropped = staDropped + 1;
      return;
    }
    Sta& s = stas[n];
    memset(&s, 0, sizeof(s));
    memcpy(s.mac, sta, 6);
    memcpy(s.bssid, bssid, 6);
    const int a = findAp(bssid);
    s.ch = a >= 0 ? aps[a].ch : ch;
    s.rssi = -127;
    staCount = n + 1;
  }
  Sta& s = stas[i];
  if (ch != s.ch) return;
  s.frames++;
  if (up) {
    s.up[pass] += len;
    if (p->rx_ctrl.rssi > s.rssi) s.rssi = p->rx_ctrl.rssi;
  } else {
    s.down[pass] += len;
  }
}

static const int8_t RSSI_UNKNOWN = -127;
static const int WEAK_RSSI = -80;  // 比這個弱的多半在隔壁房間或更遠

// 訊號很弱或不明時最多列為「可以留意」，理由寫清楚
static void addItem(Item&& it) {
  if (it.rssi == RSSI_UNKNOWN) {
    it.why += TR("監聽期間沒有收到它送出的資料，訊號強度不明，多半在隔壁或更遠\n");
    if (it.level == 2) it.level = 1;
  } else if (it.rssi < WEAK_RSSI && it.level == 2) {
    it.why += String(TR("訊號很弱（")) + it.rssi + TR(" dBm），多半在隔壁或更遠\n");
    it.level = 1;
  }
  items.push_back(std::move(it));
}

static String kbps(uint32_t bps) {
  return bps >= 10240 ? String(bps / 1024) + " KB/s" : String(bps / 1024.0f, 1) + " KB/s";
}

static String vendorWhy(int v, const char* what) {
  if (v < 0) return "";
  const Vendor& V = VENDORS[v];
  if (V.cat == 1) return String(what) + TR("屬於攝影機品牌 ") + V.name + "\n";
  return String(what) + TR("屬於 ") + V.name + TR("，這家的晶片或模組常用在便宜的 Wi-Fi 攝影機，也用在插座、燈泡等智慧家電\n");
}

static void buildReport() {
  items.clear();
  // 連網裝置
  for (int i = 0; i < staCount; i++) {
    const Sta& s = stas[i];
    Item it;
    it.kind = 0;
    it.ch = s.ch;
    it.rssi = s.rssi > -127 ? s.rssi : RSSI_UNKNOWN;
    memcpy(it.mac, s.mac, 6);
    const int v = ouiVendor(s.mac);
    const bool rnd = localAdmin(s.mac);
    uint32_t upSum = 0, downSum = 0, msSum = 0;
    uint8_t steady = 0;
    for (int p = 0; p < PASSES; p++) {
      const uint32_t ms = visitMs[p][s.ch];
      upSum += s.up[p];
      downSum += s.down[p];
      msSum += ms;
      if (ms && (uint64_t)s.up[p] * 1000 / ms >= UP_STEADY_BPS) steady++;
    }
    it.upBps = msSum ? (uint64_t)upSum * 1000 / msSum : 0;
    const bool uploading = steady >= 2 && upSum > downSum * 2;
    it.name = v >= 0 ? String(VENDORS[v].name) + TR(" 的裝置") : rnd ? TR("隨機位址的裝置") : TR("連網裝置");
    const int a = findAp(s.bssid);
    if (a >= 0 && aps[a].ssid[0]) it.sub = String(TR("連在「")) + aps[a].ssid + TR("」");
    it.why = vendorWhy(v, TR("MAC 位址"));
    if (uploading) {
      it.why += String(TR("持續上傳：")) + PASSES + TR(" 輪監聽有 ") + steady + TR(" 輪在送資料，平均 ") + kbps(it.upBps) +
                (downSum ? TR("，送出是收到的 ") + String(downSum ? upSum / downSum : 0) + TR(" 倍") : TR("，幾乎沒有收到資料")) + "\n";
    }
    if (rnd) it.why += TR("位址是隨機產生的，多半是手機、平板或電腦\n");
    if (v >= 0 && VENDORS[v].cat == 1) it.level = 2;
    else if (uploading && !rnd) it.level = 2;
    else if (uploading || v >= 0) it.level = 1;
    else it.level = 0;
    addItem(std::move(it));
  }
  // 基地台
  for (int i = 0; i < apCount; i++) {
    const Ap& a = aps[i];
    Item it;
    it.kind = 1;
    it.ch = a.ch;
    it.rssi = a.rssi;
    it.upBps = 0;
    memcpy(it.mac, a.bssid, 6);
    const int v = ouiVendor(a.bssid);
    const char* word = a.ssid[0] ? cameraWord(a.ssid) : nullptr;
    it.name = a.ssid[0] ? String(TR("「")) + a.ssid + TR("」") : String(TR("沒有名稱的基地台"));
    if (v >= 0) it.sub = VENDORS[v].name;
    it.why = vendorWhy(v, TR("基地台的 MAC 位址"));
    if (word) it.why += String(TR("名稱含有「")) + word + TR("」，有些攝影機會開自己的熱點讓手機直接連\n");
    if (espDefault(a.ssid)) it.why += TR("Espressif 晶片預設的熱點名稱，常見於還沒設定好或自製的裝置\n");
    if (!a.ssid[0]) it.why += TR("不廣播名稱\n");
    if ((v >= 0 && VENDORS[v].cat == 1) || word) it.level = 2;
    else if (v >= 0 || espDefault(a.ssid) || (!a.ssid[0] && a.rssi >= -65)) it.level = 1;
    else it.level = 0;
    addItem(std::move(it));
  }
  // 藍牙：只收了比對到的
  for (int i = 0; i < bleCount; i++) {
    const BleDev& b = bles[i];
    Item it;
    it.kind = 2;
    it.ch = 0;
    it.rssi = b.rssi;
    it.upBps = 0;
    memcpy(it.mac, b.addr, 6);
    it.name = b.name[0] ? String(b.name) : String(TR("藍牙裝置"));
    if (b.vendor >= 0) it.sub = VENDORS[b.vendor].name;
    it.why = vendorWhy(b.vendor, TR("藍牙廣播的公司代碼或服務"));
    if (b.match[0]) it.why += String(TR("名稱含有「")) + b.match + TR("」\n");
    it.level = (b.vendor >= 0 && VENDORS[b.vendor].cat == 1) || b.match[0] ? 2 : 1;
    addItem(std::move(it));
  }
  std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) {
    if (x.level != y.level) return x.level > y.level;
    return x.rssi > y.rssi;
  });
}

static void listenTask(void*) {
  const wifi_mode_t prevMode = WiFi.getMode();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  esp_wifi_get_mac(WIFI_IF_STA, selfMac);

  // 先掃描基地台，記下每個基地台的頻道
  const int16_t n = WiFi.scanNetworks(false, true, false, 120);
  bool used[14] = {};
  for (int i = 0; i < n && apCount < MAX_AP; i++) {
    const uint8_t* b = WiFi.BSSID(i);
    const int ch = WiFi.channel(i);
    if (!b || ch < 1 || ch > 13) continue;
    Ap& a = aps[apCount++];
    memcpy(a.bssid, b, 6);
    strlcpy(a.ssid, WiFi.SSID(i).c_str(), sizeof(a.ssid));
    a.ch = ch;
    a.rssi = WiFi.RSSI(i);
    used[ch] = true;
  }
  WiFi.scanDelete();
  for (int ch = 1; ch <= 13; ch++) {
    if (used[ch]) chans[chanCount++] = ch;
  }
  if (chanCount == 0) {
    const uint8_t def[] = {1, 6, 11};
    for (uint8_t c : def) chans[chanCount++] = c;
  }
  progress = 10;

  const uint32_t dwell = std::min(DWELL_MAX_MS, std::max(DWELL_MIN_MS, LISTEN_BUDGET_MS / (PASSES * chanCount)));
  wifi_promiscuous_filter_t flt = {};
  flt.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(&onPacket);
  esp_wifi_set_promiscuous(true);
  const int steps = PASSES * chanCount;
  int step = 0;
  for (uint8_t p = 0; p < PASSES && !cancelReq; p++) {
    for (int c = 0; c < chanCount && !cancelReq; c++) {
      listening = false;
      esp_wifi_set_channel(chans[c], WIFI_SECOND_CHAN_NONE);
      curPass = p;
      curCh = chans[c];
      listening = true;
      const uint32_t t0 = millis();
      while (millis() - t0 < dwell && !cancelReq) vTaskDelay(pdMS_TO_TICKS(50));
      listening = false;
      visitMs[p][chans[c]] += millis() - t0;
      progress = 10 + 90 * ++step / steps;
    }
  }
  listening = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  WiFi.mode(prevMode);
  if (!cancelReq) buildReport();
  finishedAt = millis();
  state = cancelReq ? CANCELLED : DONE;
  vTaskDelete(nullptr);
}

// 熱點關掉之後才呼叫。結果看 state
static bool startWifi() {
  state = WIFI;
  if (xTaskCreatePinnedToCore(listenTask, "roomcheck", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    state = CANCELLED;
    return false;
  }
  return true;
}

// ---- 尋找 Wi-Fi 裝置 ------------------------------------------------------
//
// 射頻停在裝置所在的頻道（熱點開著時由主程式先把熱點換到同一個頻道），只聽這個位址送出的
// 管理與資料封包，記下最強的訊號。主程式定期取走，當成尋找模式的訊號強度

static uint8_t findMac[6];
static volatile bool findActive = false;
static volatile int8_t findMax = RSSI_UNKNOWN;
static volatile uint16_t findHits = 0;

static void onFindPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!findActive || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 24 || memcmp(p->payload + 10, findMac, 6) != 0) return;  // 送出者是位址 2
  if (p->rx_ctrl.rssi > findMax) findMax = p->rx_ctrl.rssi;
  findHits = findHits + 1;
}

static void wifiFindStart(const uint8_t* mac) {
  memcpy(findMac, mac, 6);
  findMax = RSSI_UNKNOWN;
  findHits = 0;
  wifi_promiscuous_filter_t flt = {};
  flt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(&onFindPacket);
  findActive = true;
  esp_wifi_set_promiscuous(true);
}

static void wifiFindStop() {
  if (!findActive) return;
  findActive = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
}

// 上次取走之後收到的最強訊號，沒收到傳回 false
static bool wifiFindTake(int& rssi) {
  if (!findHits) return false;
  rssi = findMax;
  findMax = RSSI_UNKNOWN;
  findHits = 0;
  return true;
}

// ---- 報告 ----------------------------------------------------------------

static String esc(const String& s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

static const char* KIND[] = {"sta", "ap", "ble"};

// 面板讀的 JSON，不含 MAC 位址。一般裝置只列前 40 個
static String json(uint32_t ver) {
  String j;
  j.reserve(4096);
  const char* st = state == DONE ? "done" : state == CANCELLED ? "cancelled" : running() ? "running" : "idle";
  int rnd = 0;
  for (int i = 0; i < staCount; i++) rnd += localAdmin(stas[i].mac);
  j = "{\"ver\":" + String(ver) + ",\"state\":\"" + st + "\",\"at\":" + String(finishedAt / 1000) +
      ",\"dur\":" + String(finishedAt > startedAt ? (finishedAt - startedAt) / 1000 : 0) + ",\"chs\":[";
  for (int i = 0; i < chanCount; i++) j += (i ? "," : "") + String(chans[i]);
  j += "],\"aps\":" + String(apCount) + ",\"stas\":" + String(staCount) + ",\"rand\":" + String(rnd) +
       ",\"ble\":" + String(bleTotal) + ",\"drop\":" + String(staDropped) + ",\"items\":[";
  int shown = 0, plain = 0;
  for (size_t i = 0; i < items.size(); i++) {
    const Item& it = items[i];
    if (it.level == 0 && ++plain > 40) continue;
    if (shown++) j += ",";
    // i：在 items 裡的編號，面板按「尋找」時送回來，位址留在板子上不送出
    j += "{\"i\":" + String(i) + ",\"k\":\"" + String(KIND[it.kind]) + "\",\"lv\":" + String(it.level) + ",\"name\":\"" + esc(it.name) +
         "\",\"sub\":\"" + esc(it.sub) + "\",\"rssi\":" + String(it.rssi) + ",\"ch\":" + String(it.ch) +
         ",\"up\":" + String(it.upBps) + ",\"why\":\"" + esc(it.why) + "\"}";
  }
  j += "]}";
  return j;
}

}  // namespace roomcheck
