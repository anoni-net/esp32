// 手機網頁面板：長按 BOOT 開啟 Wi-Fi 熱點，手機連上後自動跳出頁面。
// 頁面上看得到附近每個追蹤器離警示門檻還差多少、用訊號強度尋找追蹤器、
// 打 MARK 記號、對時、確認警示，以及切換統計模式、flash 記錄、安靜模式。
//
// 網頁伺服器跑在自己的工作，藍牙掃描的 5 秒內頁面也能回應。
// 網頁這一側不直接碰記錄與追蹤器的資料：按鈕產生事件放進佇列，由 loop() 處理；
// 狀態由 loop() 在每次掃描後複製一份快照，網頁只讀快照。
#pragma once
#include "i18n.h"
#include <Arduino.h>
#include <DNSServer.h>
#include <FFat.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"

namespace webpanel {

enum EventType : uint8_t {
  EV_MARK, EV_TIME, EV_ACK, EV_STATS, EV_CLEAR, EV_CAPTIVE, EV_LINKOFF,
  EV_LINK,  // 設定連線，value：bit 0 Wi-Fi、bit 1 藍牙
 
  EV_FIND,   // label：要尋找的位址，空字串代表結束尋找
  EV_LOG,    // flash 記錄的寫入，value 1 開啟、0 暫停
  EV_QUIET,  // 安靜模式（蜂鳴器不響），value 1 開啟、0 關閉
  EV_CFG,    // label：設定名稱（preset、interval、duration、idle、wifiscan、autosound），value：新的值
  EV_IDENTITY,  // label：新的名稱，extra：新的密碼，空字串代表不改
  EV_LED,       // value 1 開啟、0 全關
  EV_FACTORY,   // 恢復原廠設定
  EV_OTA_ARM,   // 開放更新：等使用者短按 BOOT
  EV_OTA_DONE,  // 韌體已寫入另一個程式槽，value：大小
  EV_NET_WIFI,    // 從網址更新用的 Wi-Fi，label：名稱、extra：密碼
  EV_NET_UPDATE,  // 從 assets.anoni.net 下載更新
  EV_BLEPIN_NEW,   // 重新產生藍牙配對碼
  EV_BLE_UNPAIR,   // 清除已配對的手機
  EV_SOUND,        // label：要發出聲音的追蹤器位址
  EV_ROOM,         // 房間檢查，value 1 開始、0 取消
  EV_HIST_CLEAR,   // 刪除之前開機的警示歷史
  EV_ROOM_FIND,    // 尋找房間檢查報告裡的裝置，value：報告裡的編號
  EV_PLACE_NAME,   // 替地點命名，label：名稱（空字串代表清除），value：地點編號，0 代表目前的地點
  EV_PLACE_FORGET, // 刪除存過的地點，value：地點編號
  EV_PROBES,       // 監聽周圍手機的探測請求，value 1 開始、0 停止
  EV_MEET,         // 會議室檢查，value 0 停止，其他是門檻的絕對值（60、70、80）
  EV_MEET_FIND,    // 尋找會議室檢查列出的裝置，value：編號
};

struct Event {
  EventType type;
  uint32_t atMs;   // 按下的那一刻，loop() 稍後處理時用這個時間記錄
  uint32_t value;  // EV_TIME：手機的 Unix 秒數；開關類事件：1 開啟、0 關閉
  char label[64];
  char extra[64];
};

struct MarkInfo {
  uint32_t n, uptime, epoch;  // epoch 為 0 代表當時還沒對時
  char label[64];
};

struct TrackerInfo {
  char addr[18];
  char type[8];
  int16_t rssi;
  uint32_t spanSec;   // 第一次到最後一次出現相隔幾秒
  uint32_t agoSec;    // 最後一次出現是幾秒前
  uint16_t windows, nearWindows;
  uint8_t presence;   // 從第一次出現起看得到它的比例（%）
  bool alerted, acked;
  uint8_t places;
  int8_t rh[10];  // 最近 10 次出現時的訊號強度
  uint8_t rhN;
};

static const int RECENT_MARKS = 5;
static const int MAX_SHOWN = 12;

struct Snapshot {
  uint32_t scan = 0, uptime = 0, trackers = 0, marks = 0, epoch = 0, logKB = 0, wifiNow = 0;
  bool alert = false, timeKnown = false, logMounted = false, logOn = false, logFull = false, quiet = false;
  bool statsOn = false, statsAllowed = true, bleOn = false, bleConnected = false;
  uint32_t statNow = 0, statPersist = 0, statTotal = 0;
  char mode[16] = "";  // detect、stats、crowd，面板自己換成文字
  MarkInfo recent[RECENT_MARKS] = {};
  int recentCount = 0;
  TrackerInfo trk[MAX_SHOWN] = {};
  int trkCount = 0;
  uint32_t needSpanSec = 0;
  uint16_t needNear = 0;
  uint8_t needPresence = 0;
  int16_t needRssi = 0;
  uint8_t needPlaces = 0;  // 0 代表沒有打開 Wi-Fi 移動判斷
  uint16_t placeId = 0;
  uint32_t lastScanAt = 0;
  uint32_t memFree = 0, memBig = 0, memMin = 0, psFree = 0;
  uint32_t tlsPs = 0, tlsInt = 0;  // 上次網址更新的加密連線配置在 PSRAM、內部 RAM 的 bytes  // 內部 RAM 剩餘、最大連續、開機以來最低，PSRAM 剩餘  // 最近一次掃描開始時的 millis()，狀態 JSON 換算成幾毫秒前
  uint16_t bleNow = 0;      // 上一次掃描收到幾個藍牙裝置
  char placeName[32] = "";  // 目前地點的名稱，沒有命名時是空字串
  uint8_t placeCount = 0;   // 存了幾個地點，變了面板才重新讀清單
  bool moving = false;
  uint16_t appleDevices = 0;  // 沒有列出的 Apple 自家裝置
  bool findOn = false;
  char findAddr[18] = "";
  int16_t findRssi = 0;
  uint32_t findAgoSec = 0, findLeftSec = 0;
  bool findSeen = false;
  bool findWifi = false;     // 尋找的是 Wi-Fi 裝置
  char findLabel[64] = "";  // 尋找房間檢查的裝置時顯示的名稱（不顯示位址）
  uint8_t preset = 1;
  uint32_t intervalSec = 30, durSec = 5, idleMin = 10;
  bool wifiScan = false;
  uint8_t autoMode = 0;  // 自動發聲：0 關閉、1 跟著我移動時、2 也協助周圍的人
  char apName[33] = "";
  bool defaultPass = true, ledOn = true;
  char build[24] = "", part[8] = "", rev[24] = "", netRev[24] = "";
  uint8_t otaState = 0;  // 0 未啟用、1 等待按 BOOT、2 可以上傳
  uint8_t bootWhat = 0;  // 等 BOOT 確認的是：0 開放更新、1 打開自動更新、2 打開接收測試版
  uint32_t otaLeftSec = 0;
  char netSsid[33] = "", netMsg[384] = "", netBuild[24] = "";
  uint8_t netState = 0;  // netupdate::State
  bool netNote = false;  // netupdate::latestNote
  uint32_t netGot = 0, netTotal = 0;
  bool autoUpd = false;      // 自動更新
  bool testChan = false;     // 從測試通道更新
  bool netLastAuto = false;  // 上一次檢查是自動更新發起的
  uint32_t netAgoSec = 0;    // 上一次檢查結束到現在
  uint32_t autoNextSec = 0;  // 下一次自動檢查
  char autoFrom[16] = "";    // 這次開機是自動更新上來的，更新前的版本
  uint32_t blePin = 0;
  int bleBonds = 0;
  bool bleSecure = false;
  bool soundBusy = false;
  char soundAddr[18] = "";
  char soundMsg[128] = "";
  uint32_t soundAgoSec = 0;
  uint8_t roomState = 0;  // 0 沒在檢查、1 藍牙掃描（或等著開始）、2 關熱點、3 Wi-Fi 監聽
  uint32_t roomVer = 0, roomLeftSec = 0, roomEst = 0;
  bool probeOn = false;    // 正在監聽探測請求
  uint8_t meetPhase = 0;   // 會議室檢查：0 沒在檢查、1 建立基準、2 監看
  uint32_t meetVer = 0;
  bool meetAlert = false;    // 會議室檢查：多出藍牙裝置
  bool meetCaution = false;  // 會議室檢查：只有 Wi-Fi 的動靜
  uint32_t everySec = 0;   // 目前實際的掃描間隔（會議室檢查時比設定短）
  uint32_t probeVer = 0;   // 探測請求的統計更新了幾次，面板看到變了才讀取
  bool roomLog = false;
};

static WebServer server(80);
static DNSServer dns;
static QueueHandle_t events = nullptr;
static Snapshot snap;
static SemaphoreHandle_t snapLock = xSemaphoreCreateMutex();
static String ssid;
// true 時攔截 DNS，手機連上就自動跳出面板。iPhone 關掉跳出的視窗時會斷線，
// 要長時間連著就關掉：手機會顯示「沒有網際網路連線」但維持連線，面板改用網址開啟
static volatile bool captive = true;
static const char* logPath = nullptr;  // flash 記錄的路徑，FAT 分區沒掛載時為 nullptr
static const char* histPrevPath = nullptr;  // 之前開機的警示歷史（JSON 陣列）
// 最近一次有手機連入的時間。剛連入的幾秒要做 DHCP、DNS 與檢查網路，
// loop() 會把藍牙掃描往後延，讓射頻留給 Wi-Fi
static volatile uint32_t lastJoinMs = 0;
// 韌體更新：loop() 在使用者按下 BOOT 後設定開放上傳的期限，上傳期間 otaActive 為 true，
// loop() 暫停藍牙掃描，射頻留給 Wi-Fi
static volatile uint32_t otaArmedUntil = 0;
static volatile bool otaActive = false;
static bool otaReject = false, otaOk = false;
static bool otaUploading = false;  // otaActive 是這次上傳設的，結束時才由上傳清掉
static String otaErr;
static uint32_t otaSize = 0;
static String histJson = "{}";
static uint32_t histVer = 0;
static String roomJson = "{\"ver\":0,\"state\":\"idle\",\"items\":[]}";
static String meetingJson = "{\"ver\":0,\"phase\":0,\"devs\":[]}";
static String probesJson = "{\"ver\":0,\"on\":false,\"frames\":0,\"ssids\":[]}";
static String placesJson = "{\"cur\":0,\"max\":32,\"list\":[]}";

// 熱點名稱的上限是 32 bytes，藍牙的掃描回應只放得下 29 bytes 的名稱，兩者共用一個名稱，
// 所以限制在 29 bytes。WPA2 的密碼是 8 到 63 個可列印的 ASCII 字元
static const size_t NAME_MAX_BYTES = 29;

static bool validName(const String& n) {
  if (n.length() == 0 || n.length() > NAME_MAX_BYTES) return false;
  for (size_t i = 0; i < n.length(); i++) {
    if ((uint8_t)n[i] < 0x20 || n[i] == 0x7F) return false;
  }
  return true;
}

static bool validPass(const String& p) {
  if (p.length() < 8 || p.length() > 63) return false;
  for (size_t i = 0; i < p.length(); i++) {
    if (p[i] < 0x20 || p[i] > 0x7E) return false;
  }
  return true;
}

// 預設名稱：前綴加上第一次開機時隨機產生的四碼。不用 MAC 末四碼，名稱才不會洩漏晶片的位址
static String defaultName(const char* prefix, const String& suffix) {
  return String(prefix) + "-" + suffix;
}

// 熱點每次開啟都換一組隨機的 BSSID，周圍的人就沒辦法用固定的位址在不同地點認出這塊板子。
// 位址設成「本地管理、單播」（第一個 byte 的最低兩個位元是 10），避免跟真實廠商的位址重複
static void randomizeApMac() {
  uint8_t mac[6];
  esp_fill_random(mac, sizeof(mac));
  mac[0] = (mac[0] & 0xFC) | 0x02;
  esp_err_t e = esp_wifi_set_mac(WIFI_IF_AP, mac);
  if (e != ESP_OK) {
    // 介面啟動中不能改位址，先停下來再改
    esp_wifi_stop();
    e = esp_wifi_set_mac(WIFI_IF_AP, mac);
    esp_wifi_start();
  }
  Serial.printf("WIFIEV,ap_mac,%02X:%02X:%02X:%02X:%02X:%02X,%s\n", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5], esp_err_to_name(e));
}

static void publish(const Snapshot& s) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  snap = s;
  xSemaphoreGive(snapLock);
}

static void copyCurrent(Snapshot& s) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  s = snap;
  xSemaphoreGive(snapLock);
}

static bool nextEvent(Event& e) { return events && xQueueReceive(events, &e, 0) == pdTRUE; }

static String jsonEscape(const char* s) {
  String o;
  for (; *s; s++) {
    const char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

// 手機面板的網頁在 panel.html，建置前由 tools/make_panel.py 壓縮成 panel_gz.h
// 各語系的面板由 make_panel.py 從 panel.html 與 i18n/panel.toml 產生
#if defined(FW_LANG_ZH_CN)
#include "panel_gz_zh_cn.h"
#elif defined(FW_LANG_EN)
#include "panel_gz_en.h"
#else
#include "panel_gz.h"
#endif

static void sendText(const String& t) { server.send(200, "text/plain; charset=utf-8", t); }

static void pushEvent(EventType type, const String& label = "", uint32_t value = 0, const String& extra = "") {
  Event e = {};
  strlcpy(e.extra, extra.c_str(), sizeof(e.extra));
  e.type = type;
  e.atMs = millis();
  e.value = value;
  strlcpy(e.label, label.c_str(), sizeof(e.label));
  xQueueSend(events, &e, 0);
}

static const char* jb(bool b) { return b ? "true" : "false"; }

// 狀態的 JSON，網頁每 2.5 秒讀一次。序列埠送 json 也會印出同一份，方便不用手機檢查
static String statusJson() {
  static Snapshot s;  // 放在靜態區，避免佔用工作的堆疊
  copyCurrent(s);
  String j;
  j.reserve(2600);
  j = "{";
  j += "\"scan\":" + String(s.scan) + ",\"uptime\":" + String(s.uptime) + ",\"trackers\":" + String(s.trackers);
  j += ",\"alert\":" + String(jb(s.alert)) + ",\"marks\":" + String(s.marks);
  j += ",\"timeKnown\":" + String(jb(s.timeKnown)) + ",\"epoch\":" + String(s.epoch);
  j += ",\"logMounted\":" + String(jb(s.logMounted)) + ",\"logOn\":" + String(jb(s.logOn)) + ",\"log\":" + String(jb(s.logOn));
  j += ",\"logFull\":" + String(jb(s.logFull)) + ",\"logKB\":" + String(s.logKB) + ",\"wifiNow\":" + String(s.wifiNow);
  j += ",\"quiet\":" + String(jb(s.quiet)) + ",\"captive\":" + String(jb(captive));
  j += ",\"stats\":" + String(jb(s.statsOn)) + ",\"statsOn\":" + String(jb(s.statsOn)) + ",\"statsAllowed\":" + String(jb(s.statsAllowed));
  j += ",\"statNow\":" + String(s.statNow) + ",\"statPersist\":" + String(s.statPersist) + ",\"statTotal\":" + String(s.statTotal);
  j += ",\"bleOn\":" + String(jb(s.bleOn)) + ",\"bleConnected\":" + String(jb(s.bleConnected));
  j += ",\"needSpan\":" + String(s.needSpanSec) + ",\"needNear\":" + String(s.needNear) +
       ",\"needPresence\":" + String(s.needPresence) + ",\"needRssi\":" + String(s.needRssi);
  j += ",\"needPlaces\":" + String(s.needPlaces) + ",\"placeId\":" + String(s.placeId) + ",\"scanAgo\":" + String(s.lastScanAt ? millis() - s.lastScanAt : 0) +
       ",\"bleNow\":" + String(s.bleNow) + ",\"mem\":[" + String(s.memFree) + "," + String(s.memBig) + "," +
       String(s.memMin) + "," + String(s.psFree) + "," + String(s.tlsPs) + "," + String(s.tlsInt) + "]" + ",\"placeName\":\"" + jsonEscape(s.placeName) + "\",\"placeCount\":" + String(s.placeCount) + ",\"moving\":" +
       String(jb(s.moving)) + ",\"appleDevices\":" + String(s.appleDevices);
  j += ",\"findOn\":" + String(jb(s.findOn)) + ",\"findAddr\":\"" + String(s.findAddr) + "\",\"findRssi\":" + String(s.findRssi);
  j += ",\"findLabel\":\"" + jsonEscape(s.findLabel) + "\",\"findWifi\":" + String(jb(s.findWifi));
  j += ",\"findSeen\":" + String(jb(s.findSeen)) + ",\"findAgo\":" + String(s.findAgoSec) + ",\"findLeft\":" + String(s.findLeftSec);
  String findType;
  j += ",\"mode\":\"" + jsonEscape(s.mode) + "\",\"trk\":[";
  for (int i = 0; i < s.trkCount; i++) {
    const TrackerInfo& t = s.trk[i];
    if (strcmp(t.addr, s.findAddr) == 0) findType = t.type;
    if (i) j += ",";
    j += "{\"addr\":\"" + String(t.addr) + "\",\"type\":\"" + String(t.type) + "\",\"rssi\":" + String(t.rssi) +
         ",\"span\":" + String(t.spanSec) + ",\"ago\":" + String(t.agoSec) + ",\"win\":" + String(t.windows) +
         ",\"near\":" + String(t.nearWindows) + ",\"pres\":" + String(t.presence) +
         ",\"alerted\":" + String(jb(t.alerted)) + ",\"acked\":" + String(jb(t.acked)) +
         ",\"places\":" + String(t.places) + ",\"rh\":[";
    for (uint8_t k = 0; k < t.rhN; k++) j += (k ? "," : "") + String(t.rh[k]);
    j += "]}";
  }
  j += "],\"findType\":\"" + findType + "\",\"recent\":[";
  for (int i = 0; i < s.recentCount; i++) {
    const MarkInfo& m = s.recent[i];
    if (i) j += ",";
    j += "{\"n\":" + String(m.n) + ",\"uptime\":" + String(m.uptime) + ",\"epoch\":" + String(m.epoch) +
         ",\"label\":\"" + jsonEscape(m.label) + "\"}";
  }
  j += "]";
  j += ",\"preset\":" + String(s.preset) + ",\"interval\":" + String(s.intervalSec) + ",\"duration\":" +
       String(s.durSec) + ",\"idle\":" + String(s.idleMin) + ",\"wifiscan\":" + String(jb(s.wifiScan)) +
       ",\"autosound\":" + String(jb(s.autoMode > 0)) + ",\"automode\":" + String(s.autoMode);
  j += ",\"histVer\":" + String(histVer);
  j += ",\"apName\":\"" + jsonEscape(s.apName) + "\",\"defaultPass\":" + String(jb(s.defaultPass)) +
       ",\"led\":" + String(jb(s.ledOn));
  j += ",\"rev\":\"" + jsonEscape(s.rev) + "\",\"netRev\":\"" + jsonEscape(s.netRev) + "\"";
  j += ",\"lang\":\"" FW_LANG "\",\"build\":\"" + String(s.build) + "\",\"part\":\"" + String(s.part) + "\",\"otaState\":" + String(s.otaState) + ",\"bootWhat\":" + String(s.bootWhat) +
       ",\"otaLeft\":" + String(s.otaLeftSec);
  j += ",\"netSsid\":\"" + jsonEscape(s.netSsid) + "\",\"netState\":" + String(s.netState) + ",\"netNote\":" + (s.netNote ? "true" : "false") + ",\"netMsg\":\"" +
       jsonEscape(s.netMsg) + "\",\"netBuild\":\"" + jsonEscape(s.netBuild) + "\",\"netGot\":" + String(s.netGot) +
       ",\"netTotal\":" + String(s.netTotal);
  char pin[8];
  snprintf(pin, sizeof(pin), "%06lu", (unsigned long)s.blePin);
  j += ",\"blePin\":\"" + String(pin) + "\",\"bleBonds\":" + String(s.bleBonds) + ",\"bleSecure\":" + String(jb(s.bleSecure));
  j += ",\"soundBusy\":" + String(jb(s.soundBusy)) + ",\"soundAddr\":\"" + String(s.soundAddr) + "\",\"soundMsg\":\"" +
       jsonEscape(s.soundMsg) + "\",\"soundAgo\":" + String(s.soundAgoSec);
  j += ",\"meetPhase\":" + String(s.meetPhase) + ",\"meetVer\":" + String(s.meetVer) + ",\"meetAlert\":" + String(jb(s.meetAlert)) + ",\"meetCaution\":" + String(jb(s.meetCaution)) + ",\"every\":" + String(s.everySec) + ",\"probeOn\":" + String(jb(s.probeOn)) + ",\"probeVer\":" + String(s.probeVer) + ",\"roomState\":" + String(s.roomState) + ",\"roomVer\":" + String(s.roomVer) + ",\"roomLeft\":" +
       String(s.roomLeftSec) + ",\"roomEst\":" + String(s.roomEst) + ",\"roomlog\":" + String(jb(s.roomLog));
  j += ",\"testchan\":" + String(jb(s.testChan));
  j += ",\"autoupd\":" + String(jb(s.autoUpd)) + ",\"netLastAuto\":" + String(jb(s.netLastAuto)) + ",\"netAgo\":" +
       String(s.netAgoSec) + ",\"autoNext\":" + String(s.autoNextSec) + ",\"autoFrom\":\"" + jsonEscape(s.autoFrom) + "\"";
  j += "}";
  return j;
}

// 警示歷史、時間軸與摘要用的資料，由 loop() 在內容變更時組好 JSON 交過來。
// 面板從狀態的 histVer 得知有新版本才讀取，不跟著每次輪詢傳送

static void publishHistory(const String& j) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  histJson = j;
  histVer++;
  xSemaphoreGive(snapLock);
}

static void handleHistory() {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  const String j = histJson;
  xSemaphoreGive(snapLock);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

// 房間檢查的報告，檢查結束時由 loop() 組好交過來。面板從狀態的 roomVer 得知有新的報告才讀取
static void publishRoom(const String& j) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  roomJson = j;
  xSemaphoreGive(snapLock);
}

static void handleRoom() {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  const String j = roomJson;
  xSemaphoreGive(snapLock);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

// 會議室檢查的狀態，檢查期間 loop() 每 2 秒組好交過來，位址不在裡面
static void publishMeeting(const String& j) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  meetingJson = j;
  xSemaphoreGive(snapLock);
}

static void handleMeeting() {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  const String j = meetingJson;
  xSemaphoreGive(snapLock);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

// 探測請求的統計，監聽期間 loop() 每 2 秒組好交過來。名稱只在記憶體，位址不在裡面
static void publishProbes(const String& j) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  probesJson = j;
  xSemaphoreGive(snapLock);
}

static void handleProbes() {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  const String j = probesJson;
  xSemaphoreGive(snapLock);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

// 存過的地點，換地點、命名或刪除時由 loop() 組好交過來。不含基地台的資料
static void publishPlaces(const String& j) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  placesJson = j;
  xSemaphoreGive(snapLock);
}

static void handlePlaces() {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  const String j = placesJson;
  xSemaphoreGive(snapLock);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

static void handleStatus() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", statusJson());
}

// 記錄檔最後約 3 KB，從完整的一行開始
static void handleTail() {
  if (!logPath || !FFat.exists(logPath)) { sendText(""); return; }
  File f = FFat.open(logPath, FILE_READ);
  if (!f) { sendText(""); return; }
  const size_t size = f.size();
  const size_t want = 3000;
  if (size > want) f.seek(size - want);
  String t = f.readString();
  f.close();
  if (size > want) {
    const int nl = t.indexOf('\n');
    if (nl >= 0) t = t.substring(nl + 1);
  }
  server.sendHeader("Cache-Control", "no-store");
  sendText(t);
}

// 熱點的事件印在序列埠，方便排查手機連不上的原因
static void onWifiEvent(arduino_event_id_t id, arduino_event_info_t info) {
  switch (id) {
    case ARDUINO_EVENT_WIFI_AP_START: Serial.println("WIFIEV,ap_start"); break;
    case ARDUINO_EVENT_WIFI_AP_STOP: Serial.println("WIFIEV,ap_stop"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      lastJoinMs = millis();
      Serial.println("WIFIEV,client_join");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.printf("WIFIEV,client_leave,reason=%d\n", info.wifi_ap_stadisconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.printf("WIFIEV,client_ip,%s\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
      break;
    default: break;
  }
}

// 熱點的開關在這個工作執行，避免與 handleClient() 同時操作伺服器
static volatile bool want = false;
static volatile bool active = false;
static bool inited = false;
static bool keepStaCfg = false;
static String pass;

// 熱點的頻道，平常是 1。setApChannel() 改變它
static volatile uint8_t apChannel = 1;
static volatile bool pendingChannel = false, channelDone = false;

static void apply(bool on) {
  if (on) {
    const bool modeOk = WiFi.mode(keepStaCfg ? WIFI_AP_STA : WIFI_AP);
    randomizeApMac();
    const bool apOk = WiFi.softAP(ssid.c_str(), pass.c_str(), apChannel);
    // DHCP 明確把板子自己設成 DNS 伺服器，手機查詢檢查網路用的網址時才會問到板子；
    // 再加上 DHCP option 114（RFC 8910）直接告訴手機面板的網址
    const IPAddress ip(192, 168, 4, 1);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0), IPAddress(192, 168, 4, 2), ip);
    if (captive) WiFi.AP.enableDhcpCaptivePortal();
    delay(100);
    Serial.printf("WIFIEV,apply_on,mode=%d,softap=%d,ip=%s,ch=%d\n", modeOk, apOk,
                  WiFi.softAPIP().toString().c_str(), WiFi.channel());
    if (captive) dns.start(53, "*", WiFi.softAPIP());
    server.begin();
  } else {
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(keepStaCfg ? WIFI_STA : WIFI_OFF);
  }
  active = on;
}

// 熱點的頻道。尋找房間裡的 Wi-Fi 裝置時換到它的頻道，射頻才能同時開熱點又聽到它。
// 換頻道要重新啟動熱點，手機會斷線幾秒再自動連回。channelDone 在重新啟動完成後設成 true

static void setApChannel(uint8_t ch) {
  channelDone = false;
  if (ch == apChannel && active) {
    channelDone = true;
    return;
  }
  apChannel = ch;
  pendingChannel = true;
}

// 名稱或密碼變更：loop() 存好設定後交過來，由這個工作重新啟動熱點
static bool pendingIdentity = false;
static String pendingName, pendingPass;

static void requestIdentity(const String& name, const String& password) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  pendingName = name;
  pendingPass = password;
  pendingIdentity = true;
  xSemaphoreGive(snapLock);
}

static void task(void*) {
  for (;;) {
    if (pendingIdentity) {
      xSemaphoreTake(snapLock, portMAX_DELAY);
      ssid = pendingName;
      pass = pendingPass;
      pendingIdentity = false;
      xSemaphoreGive(snapLock);
      if (active) {
        vTaskDelay(pdMS_TO_TICKS(1500));  // 讓剛才的回應先送出
        apply(false);
        vTaskDelay(pdMS_TO_TICKS(300));
        apply(true);
      }
    }
    if (pendingChannel) {
      pendingChannel = false;
      if (active) {
        vTaskDelay(pdMS_TO_TICKS(1500));  // 讓剛才的回應先送出
        apply(false);
        vTaskDelay(pdMS_TO_TICKS(300));
        apply(true);
      }
      channelDone = true;
    }
    if (want != active) apply(want);
    if (active) {
      if (captive) dns.processNextRequest();
      server.handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// 只記下設定，熱點等到 setEnabled(true) 才開。keepSta 為 true 時保留 Station 介面，
// 給 Wi-Fi 基地台掃描使用
static void configure(const String& name, const String& password, bool keepSta, bool captiveOn) {
  captive = captiveOn;
  keepStaCfg = keepSta;
  pass = password;
  ssid = name;
  events = xQueueCreate(8, sizeof(Event));
  WiFi.onEvent(onWifiEvent);
}

static void registerRoutes() {
  // 網頁事先用 gzip 壓縮（約 70 KB 變 25 KB），瀏覽器自己解壓縮，板子直接從 flash 送出
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html; charset=utf-8", (const char*)PANEL_GZ, PANEL_GZ_LEN);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/tail", HTTP_GET, handleTail);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/history/prev", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    if (!histPrevPath || !FFat.exists(histPrevPath)) {
      server.send(200, "application/json; charset=utf-8", "[]");
      return;
    }
    File f = FFat.open(histPrevPath, FILE_READ);
    server.streamFile(f, "application/json; charset=utf-8");
    f.close();
  });
  server.on("/api/history/clear", HTTP_POST, []() {
    pushEvent(EV_HIST_CLEAR);
    sendText(TR("已刪除之前開機的紀錄，這次開機的保留"));
  });
  server.on("/api/room", HTTP_GET, handleRoom);
  server.on("/api/places", HTTP_GET, handlePlaces);
  server.on("/api/probes", HTTP_GET, handleProbes);
  server.on("/api/meeting", HTTP_GET, handleMeeting);
  server.on("/api/meeting/start", HTTP_POST, []() {
    const int r = server.arg("rssi").toInt();
    pushEvent(EV_MEET, "", r >= 50 && r <= 90 ? r : 70);
    sendText(TR("開始會議室檢查，先用 2 分鐘建立基準"));
  });
  server.on("/api/meeting/stop", HTTP_POST, []() {
    pushEvent(EV_MEET, "", 0);
    sendText(TR("已結束會議室檢查"));
  });
  server.on("/api/meeting/find", HTTP_POST, []() {
    pushEvent(EV_MEET_FIND, "", server.arg("i").toInt());
    sendText(TR("開始尋找，拿著偵測器慢慢走"));
  });
  server.on("/api/probes/start", HTTP_POST, []() {
    pushEvent(EV_PROBES, "", 1);
    sendText(TR("開始監聽 5 分鐘，結果每 2 秒更新"));
  });
  server.on("/api/probes/stop", HTTP_POST, []() {
    pushEvent(EV_PROBES, "", 0);
    sendText(TR("已停止監聽"));
  });
  server.on("/api/places/name", HTTP_POST, []() {
    String n = server.arg("name");
    n.trim();
    bool ok = n.length() <= 30;
    for (size_t i = 0; ok && i < n.length(); i++) ok = (uint8_t)n[i] >= 0x20 && (uint8_t)n[i] != 0x7f;
    if (!ok) {
      sendText(TR("名稱最多 30 bytes（中文約 10 個字），不能有控制字元"));
      return;
    }
    pushEvent(EV_PLACE_NAME, n, server.arg("id").toInt());
    sendText(TR("已送出"));  // 結果由面板重新讀取清單確認
  });
  server.on("/api/places/forget", HTTP_POST, []() {
    pushEvent(EV_PLACE_FORGET, "", server.arg("id").toInt());
    sendText(TR("已刪除這個地點，下次經過時會當成新的地點"));
  });
  server.on("/api/room/find", HTTP_POST, []() {
    pushEvent(EV_ROOM_FIND, "", server.arg("i").toInt());
    sendText(server.arg("k") == "ble" ? TR("開始尋找，拿著偵測器慢慢走")
                                      : TR("開始尋找。熱點會換到這個裝置的頻道，手機斷線幾秒後會自動連回"));
  });
  server.on("/api/room/start", HTTP_POST, []() {
    if (otaActive) {
      // 409：面板看狀態碼決定要不要進入等待畫面，不比對文字
      server.send(409, "text/plain; charset=utf-8", TR("正在更新韌體，結束後再檢查"));
      return;
    }
    pushEvent(EV_ROOM, "", 1);
    sendText(TR("開始檢查，約 2 分鐘。15 秒後熱點會關閉，燈號綠色連閃兩下時再連回來看報告"));
  });
  server.on("/api/mark", HTTP_POST, []() {
    String label = server.arg("label");
    label.trim();
    pushEvent(EV_MARK, label);
    sendText(label.length() ? TR("已記錄：") + label : String("已記錄"));
  });
  server.on("/api/time", HTTP_POST, []() {
    const long epoch = server.arg("epoch").toInt();
    if (epoch < 1700000000L) { sendText(TR("時間格式不對")); return; }
    pushEvent(EV_TIME, "", (uint32_t)epoch);
    sendText(TR("已用手機的時間對時"));
  });
  server.on("/api/find", HTTP_POST, []() {
    const String addr = server.arg("addr");
    pushEvent(EV_FIND, addr);
    sendText(addr.length() ? TR("開始尋找") : TR("結束尋找"));
  });
  server.on("/api/set", HTTP_POST, []() {
    const String key = server.arg("key");
    const bool on = server.arg("on") == "1";
    const uint32_t v = server.arg("v").toInt();
    if (key == "preset" || key == "interval" || key == "duration" || key == "idle") {
      pushEvent(EV_CFG, key, v);
      sendText(key == "preset" ? TR("警示門檻已變更，追蹤器重新累計") : TR("設定已變更"));
    } else if (key == "autosound") {
      const uint32_t mode = server.hasArg("v") ? v : (on ? 1 : 0);
      if (mode > 2) {
        sendText(TR("設定值不正確"));
        return;
      }
      pushEvent(EV_CFG, key, mode);
      sendText(mode == 0   ? TR("自動發聲已關閉，只能手動讓追蹤器響")
               : mode == 1 ? TR("追蹤器跟著你經過好幾個地點時，會自動讓它響")
                           : TR("也會協助周圍的人：附近與主人分開的 DULT 追蹤器出現夠久，會讓它響一次"));
    } else if (key == "autoupd") {
      if (on && !snap.netSsid[0]) {
        sendText(TR("先設定更新用的 Wi-Fi"));
        return;
      }
      pushEvent(EV_CFG, key, on);
      sendText(on ? TR("請在 60 秒內短按一下偵測器的 BOOT 鍵，確認打開自動更新") : TR("自動更新已關閉，從網路更新要先按「開放更新」再按 BOOT"));
    } else if (key == "testchan") {
      pushEvent(EV_CFG, key, on);
      sendText(on ? TR("請在 60 秒內短按一下偵測器的 BOOT 鍵，確認改從測試通道更新") : TR("改回正式通道"));
    } else if (key == "roomlog") {
      pushEvent(EV_CFG, key, on);
      sendText(on ? TR("房間檢查收到的位址會寫進 flash 記錄") : TR("房間檢查的位址不寫進記錄"));
    } else if (key == "wifiscan") {
      pushEvent(EV_CFG, key, on);
      sendText(on ? TR("移動判斷開啟，追蹤器要跟著你經過 3 個不同的地點才警示") : TR("移動判斷關閉"));
    } else if (key == "stats") {
      pushEvent(EV_STATS, "", on);
      sendText(on ? TR("統計模式開啟") : TR("統計模式關閉"));
    } else if (key == "log") {
      pushEvent(EV_LOG, "", on);
      sendText(on ? TR("flash 記錄開始寫入") : TR("flash 記錄暫停"));
    } else if (key == "quiet") {
      pushEvent(EV_QUIET, "", on);
      sendText(on ? TR("安靜模式開啟，蜂鳴器不響") : TR("安靜模式關閉"));
    } else if (key == "captive") {
      if (on && !captive) dns.start(53, "*", WiFi.softAPIP());
      if (!on && captive) dns.stop();
      captive = on;
      pushEvent(EV_CAPTIVE, "", on);
      sendText(on ? TR("已開啟自動跳出面板，下次連線生效") : TR("已關閉自動跳出面板，重新連線後會維持連線"));
    } else {
      sendText(TR("不認得的設定"));
    }
  });
  server.on("/log.txt", HTTP_GET, []() {
    if (!logPath || !FFat.exists(logPath)) { server.send(404, "text/plain; charset=utf-8", TR("沒有 flash 記錄")); return; }
    File f = FFat.open(logPath, FILE_READ);
    if (!f) { server.send(500, "text/plain; charset=utf-8", TR("無法開啟記錄檔")); return; }
    static Snapshot s;
    copyCurrent(s);
    const String name = ssid + "-" + String(s.timeKnown ? s.epoch : s.uptime) + ".txt";
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    server.streamFile(f, "text/plain; charset=utf-8");
    f.close();
  });
  server.on("/api/clear", HTTP_POST, []() {
    pushEvent(EV_CLEAR);
    sendText(TR("已刪除 flash 記錄"));
  });
  server.on("/api/identity", HTTP_POST, []() {
    String name = server.arg("name"), pw = server.arg("pass");
    name.trim();
    if (name.length() && !validName(name)) { sendText(TR("名稱太長或含有控制字元，最多 29 bytes（中文約 9 個字）")); return; }
    if (pw.length() && !validPass(pw)) { sendText(TR("密碼要 8 到 63 個英數字或符號，不能用中文")); return; }
    if (!name.length() && !pw.length()) { sendText(TR("沒有要變更的內容")); return; }
    pushEvent(EV_IDENTITY, name, 0, pw);
    sendText(TR("已儲存。熱點會在幾秒後以新的設定重新啟動，請用新的名稱與密碼重新連線"));
  });
  server.on("/api/led", HTTP_POST, []() {
    const bool on = server.arg("on") == "1";
    pushEvent(EV_LED, "", on);
    sendText(on ? TR("燈號開啟") : TR("燈號全關"));
  });
  server.on("/api/factory", HTTP_POST, []() {
    pushEvent(EV_FACTORY);
    sendText(TR("恢復原廠設定，偵測器即將重新開機。熱點名稱與密碼會回到預設值"));
  });
  server.on("/api/sound", HTTP_POST, []() {
    const String addr = server.arg("addr");
    if (addr.length() != 17) { sendText(TR("位址格式不對")); return; }
    pushEvent(EV_SOUND, addr);
    sendText(TR("正在尋找並連線到追蹤器，約 5 到 10 秒"));
  });
  server.on("/api/ble/newpin", HTTP_POST, []() {
    pushEvent(EV_BLEPIN_NEW);
    sendText(TR("已產生新的配對碼，之後配對的手機要用新的配對碼"));
  });
  server.on("/api/ble/unpair", HTTP_POST, []() {
    pushEvent(EV_BLE_UNPAIR);
    sendText(TR("已清除所有配對過的手機，下次連線要重新配對"));
  });
  // 從網址更新：先設定可以上網的 Wi-Fi，開放更新的期間內才接受下載
  server.on("/api/net/wifi", HTTP_POST, []() {
    const String n = server.arg("ssid"), pw = server.arg("pass");
    if (n.length() > 32 || pw.length() > 63) {
      sendText(TR("Wi-Fi 名稱最多 32 bytes，密碼最多 63 個字元"));
      return;
    }
    pushEvent(EV_NET_WIFI, n.c_str(), 0, pw.c_str());
    sendText(n.length() ? TR("已儲存更新用的 Wi-Fi") : TR("已清除更新用的 Wi-Fi"));
  });
  server.on("/api/net/update", HTTP_POST, []() {
    // 開啟自動更新後不必按 BOOT。上傳檔案（/update）仍然只在按過 BOOT 的期間內接受
    if (!snap.autoUpd && (otaArmedUntil == 0 || (int32_t)(otaArmedUntil - millis()) <= 0)) {
      sendText(TR("還沒開放更新，先按「開放更新」，再短按偵測器的 BOOT 鍵"));
      return;
    }
    if (!snap.netSsid[0]) {
      sendText(TR("先設定更新用的 Wi-Fi"));
      return;
    }
    if (otaActive) {
      sendText(TR("正在更新中"));
      return;
    }
    pushEvent(EV_NET_UPDATE);
    sendText(TR("開始檢查 assets.anoni.net 上的版本"));
  });
  server.on("/api/ota/arm", HTTP_POST, []() {
    pushEvent(EV_OTA_ARM);
    sendText(TR("請在 60 秒內短按一下偵測器的 BOOT 鍵"));
  });
  server.on("/update", HTTP_POST, []() {
    if (!otaOk) {
      sendText(TR("更新失敗：") + otaErr);
      return;
    }
    sendText(TR("更新完成，偵測器即將重新開機。重新開機後熱點會自動打開，約 20 秒後重新連上，確認頁尾的版本"));
    pushEvent(EV_OTA_DONE, "", otaSize);
  }, []() {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      otaOk = false;
      otaErr = "";
      otaReject = false;
      if (otaActive) {
        otaErr = TR("正在從網路更新，請等它結束");
        otaReject = true;
        return;
      }
      if (otaArmedUntil == 0 || (int32_t)(otaArmedUntil - millis()) <= 0) {
        otaErr = snap.autoUpd ? TR("還沒開放上傳，先按「開放上傳」，再短按偵測器的 BOOT 鍵")
                              : TR("還沒開放更新，先按「開放更新」，再短按偵測器的 BOOT 鍵");
        otaReject = true;
        return;
      }
      otaActive = true;
      otaUploading = true;
      Serial.printf("OTA：開始接收 %s\n", u.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        otaErr = Update.errorString();
        otaReject = true;
        otaActive = false;
      }
    } else if (u.status == UPLOAD_FILE_WRITE) {
      if (otaReject) return;
      if (Update.write(u.buf, u.currentSize) != u.currentSize) {
        otaErr = Update.errorString();
        otaReject = true;
        Update.abort();
        otaActive = false;
      }
    } else if (u.status == UPLOAD_FILE_END) {
      if (!otaReject) {
        // end(true) 會驗證映像檔的格式、晶片型號與雜湊，通過才切換下次開機的程式槽
        if (Update.end(true)) {
          otaOk = true;
          otaSize = u.totalSize;
        } else {
          otaErr = Update.errorString();
        }
      }
      if (otaUploading) otaActive = false;
      otaUploading = false;
    } else if (u.status == UPLOAD_FILE_ABORTED) {
      if (!otaReject) Update.abort();
      otaErr = TR("上傳中斷");
      otaReject = true;
      if (otaUploading) otaActive = false;
      otaUploading = false;
    }
  });
  server.on("/api/link", HTTP_POST, []() {
    const bool wifi = server.arg("wifi") == "1", ble = server.arg("ble") == "1";
    pushEvent(EV_LINK, "", (wifi ? 1 : 0) | (ble ? 2 : 0));
    sendText(!wifi && ble   ? TR("Wi-Fi 即將關閉，只留藍牙。面板會斷線，要再打開請長按 BOOT 2 秒")
             : !wifi        ? TR("Wi-Fi 與藍牙即將關閉")
             : ble          ? TR("藍牙已開啟，用藍牙終端機 App 連線，配對碼在下方")
                            : TR("藍牙已關閉"));
  });
  server.on("/api/linkoff", HTTP_POST, []() {
    pushEvent(EV_LINKOFF);
    sendText(TR("已退出，Wi-Fi 與藍牙即將關閉"));
  });
  server.on("/api/ack", HTTP_POST, []() {
    pushEvent(EV_ACK);
    sendText(TR("已確認警示"));
  });
  // 系統檢查網路的網址一律轉到面板，手機就會跳出頁面
  server.onNotFound([]() {
    Serial.printf("WIFIEV,http,%s%s,captive=%d\n", server.hostHeader().c_str(), server.uri().c_str(), (int)captive);
    if (captive && server.hostHeader() != WiFi.softAPIP().toString()) {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain", "not found");
  });
}

static void setEnabled(bool on) {
  if (on && !inited) {
    registerRoutes();
    xTaskCreatePinnedToCore(task, "webpanel", 8192, nullptr, 1, nullptr, 0);
    inited = true;
  }
  want = on;
}

// 序列埠用：直接切換自動跳出頁面
static void setCaptive(bool on) {
  if (active) {
    if (on && !captive) dns.start(53, "*", WiFi.softAPIP());
    if (!on && captive) dns.stop();
  }
  captive = on;
}

static uint8_t stationCount() { return active ? WiFi.softAPgetStationNum() : 0; }

}  // namespace webpanel
