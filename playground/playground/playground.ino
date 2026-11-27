// ESP32-S3 能力展示韌體
//
// 板子開一個 Wi-Fi 熱點，手機或筆電連上後自動跳出控制頁（captive portal），
// 在頁面上直接操作：晶片資訊、RGB LED、觸控腳位、傳輸速度、Wi-Fi 與 BLE 掃描、
// 連上外部 Wi-Fi、OTA 更新（上傳檔案、從網址拉、定期檢查新版）。
//
// 需要：
//   - Arduino-ESP32 3.x
//   - NimBLE-Arduino 2.x

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <NimBLEDevice.h>
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "page.h"
#include "bench.h"
#include "ble_uart.h"

// 每次要測試 OTA 時把這個數字加一，再重新建置
#define FW_BUILD 1

static const char* AP_PASS = "playground";  // 熱點密碼，至少 8 個字元
static const int RGB_LED_GPIO = 48;          // 部分板子是 38
static const uint8_t TOUCH_PINS[] = {1, 2, 5, 6, 7, 10, 11, 12, 13, 14};

static WebServer server(80);
static DNSServer dns;
static Preferences prefs;
static String apSsid;

static String staSsid, staPass;     // 外部 Wi-Fi
static String pendingSsid;          // 藍牙配網：先收到名稱，再收到密碼才連線
static uint32_t staJoinAt = 0;      // 藍牙配網開始連線的時間，連上或逾時後回報
static uint32_t blePin = 0;         // 藍牙配對碼
static String autoUrl;              // 定期檢查用的版本資訊網址
static uint32_t autoMinutes = 0;    // 0 代表不定期檢查
static uint32_t lastAutoCheck = 0;

// ---- 事件紀錄，顯示在頁面上 ----

static const int LOG_LINES = 24;
static String logBuf[LOG_LINES];
static int logHead = 0, logCount = 0;

static void logf(const char* fmt, ...) {
  char line[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  char stamped[220];
  snprintf(stamped, sizeof(stamped), "[%lus] %s", (unsigned long)(millis() / 1000), line);
  Serial.println(stamped);
  logBuf[logHead] = stamped;
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

// ---- JSON 小工具 ----

static String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

static String hexOf(const std::string& s) {
  static const char* H = "0123456789abcdef";
  String o;
  for (unsigned char c : s) { o += H[c >> 4]; o += H[c & 15]; }
  return o;
}

static void sendJson(const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", body);
}

// ---- API ----

static void handleInfo() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  String j = "{";
  j += "\"model\":\"" + String(ESP.getChipModel()) + "\"";
  j += ",\"rev\":" + String(ESP.getChipRevision());
  j += ",\"cores\":" + String(ESP.getChipCores());
  j += ",\"mhz\":" + String(ESP.getCpuFreqMHz());
  j += ",\"temp\":" + String(temperatureRead(), 1);
  j += ",\"flash\":" + String(ESP.getFlashChipSize());
  j += ",\"psram\":" + String(ESP.getPsramSize());
  j += ",\"psramFree\":" + String(ESP.getFreePsram());
  j += ",\"heap\":" + String(ESP.getFreeHeap());
  j += ",\"sketch\":" + String(ESP.getSketchSize());
  j += ",\"sketchFree\":" + String(ESP.getFreeSketchSpace());
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"ap\":\"" + jsonEscape(apSsid) + "\"";
  j += ",\"apClients\":" + String(WiFi.softAPgetStationNum());
  const bool staUp = WiFi.status() == WL_CONNECTED;
  j += ",\"sta\":\"" + (staUp ? jsonEscape(WiFi.SSID()) : String("")) + "\"";
  j += ",\"staIp\":\"" + (staUp ? WiFi.localIP().toString() : String("")) + "\"";
  j += ",\"staRssi\":" + String(staUp ? WiFi.RSSI() : 0);
  j += ",\"mac\":\"" + WiFi.macAddress() + "\"";
  j += ",\"ble\":" + String(bleuart::connected ? "true" : "false");
  j += ",\"bleSecure\":" + String(bleuart::secure ? "true" : "false");
  char pin[8];
  snprintf(pin, sizeof(pin), "%06lu", (unsigned long)blePin);
  j += ",\"blePin\":\"" + String(pin) + "\"";
  j += ",\"build\":" + String(FW_BUILD);
  j += ",\"part\":\"" + String(run ? run->label : "?") + "\"";
  j += ",\"autoUrl\":\"" + jsonEscape(autoUrl) + "\"";
  j += ",\"autoMin\":" + String(autoMinutes);
  j += ",\"log\":[";
  for (int i = 0; i < logCount; i++) {
    const int idx = (logHead - logCount + i + LOG_LINES) % LOG_LINES;
    if (i) j += ",";
    j += "\"" + jsonEscape(logBuf[idx]) + "\"";
  }
  j += "]}";
  sendJson(j);
}

static void handleLed() {
  const uint8_t r = server.arg("r").toInt(), g = server.arg("g").toInt(), b = server.arg("b").toInt();
  rgbLedWrite(RGB_LED_GPIO, r, g, b);
  sendJson("{\"ok\":true}");
}

static void handleTouch() {
  String j = "[";
  for (size_t i = 0; i < sizeof(TOUCH_PINS); i++) {
    if (i) j += ",";
    j += "{\"pin\":" + String(TOUCH_PINS[i]) + ",\"v\":" + String(touchRead(TOUCH_PINS[i])) + "}";
  }
  j += "]";
  sendJson(j);
}

static void handleSpeed() {
  uint32_t kb = server.hasArg("kb") ? server.arg("kb").toInt() : 2048;
  if (kb < 1) kb = 1;
  if (kb > 32768) kb = 32768;
  const size_t total = (size_t)kb * 1024;
  static uint8_t buf[8192];
  static bool filled = false;
  if (!filled) { for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 31 + 7); filled = true; }

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(total);
  server.send(200, "application/octet-stream", "");
  WiFiClient c = server.client();
  size_t sent = 0;
  int stalls = 0;
  const uint32_t t0 = millis();
  while (sent < total && c.connected()) {
    const size_t n = min(sizeof(buf), total - sent);
    const size_t w = c.write(buf, n);
    if (w == 0) {
      if (++stalls > 3000) break;
      delay(1);
      continue;
    }
    stalls = 0;
    sent += w;
  }
  const uint32_t ms = millis() - t0;
  logf("速度測試：送出 %u KB，%lu ms，約 %.2f Mbit/s", (unsigned)(sent / 1024), (unsigned long)ms,
       ms ? sent * 8.0 / ms / 1000.0 : 0.0);
}

static const char* encName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "開放";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2 企業";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "其他";
  }
}

static void handleWifiScan() {
  const int16_t n = WiFi.scanNetworks(false, true);
  String j = "[";
  for (int i = 0; i < n; i++) {
    if (i) j += ",";
    j += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"ch\":" + String(WiFi.channel(i)) + ",\"enc\":\"" + encName(WiFi.encryptionType(i)) + "\"}";
  }
  j += "]";
  WiFi.scanDelete();
  logf("Wi-Fi 掃描：%d 個基地台", n);
  sendJson(j);
}

static void handleBleScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(100);
  NimBLEScanResults results = scan->getResults(3000, false);
  String j = "[";
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    if (i) j += ",";
    j += "{\"addr\":\"" + String(d->getAddress().toString().c_str()) + "\",\"rssi\":" + String(d->getRSSI()) +
         ",\"name\":\"" + jsonEscape(String(d->getName().c_str())) + "\",\"mfg\":\"" +
         (d->haveManufacturerData() ? hexOf(d->getManufacturerData()) : String("")) + "\"}";
  }
  j += "]";
  logf("BLE 掃描：%d 個裝置", results.getCount());
  scan->clearResults();
  sendJson(j);
}

static void connectSta() {
  if (staSsid.isEmpty()) return;
  logf("連線到外部 Wi-Fi：%s", staSsid.c_str());
  WiFi.begin(staSsid.c_str(), staPass.c_str());
}

static void handleWifi() {
  staSsid = server.arg("ssid");
  staPass = server.arg("pass");
  prefs.putString("ssid", staSsid);
  prefs.putString("pass", staPass);
  if (staSsid.isEmpty()) {
    WiFi.disconnect(false, true);
    logf("已忘記外部 Wi-Fi");
    server.send(200, "text/plain; charset=utf-8", "已忘記外部 Wi-Fi");
    return;
  }
  connectSta();
  server.send(200, "text/plain; charset=utf-8", "連線中，幾秒後看上方的「外部 Wi-Fi」欄位");
}

// ---- OTA ----

static void handleUploadDone() {
  if (Update.hasError()) {
    server.send(200, "text/plain; charset=utf-8", String("更新失敗：") + Update.errorString());
    return;
  }
  server.send(200, "text/plain; charset=utf-8", "更新完成，重新開機中，約 10 秒後重新整理頁面");
  logf("上傳更新完成，重新開機");
  delay(800);
  ESP.restart();
}

static void handleUploadChunk() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    logf("開始接收韌體：%s", u.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) logf("Update.begin 失敗：%s", Update.errorString());
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) logf("寫入失敗：%s", Update.errorString());
  } else if (u.status == UPLOAD_FILE_END) {
    if (Update.end(true)) logf("收到 %u bytes，驗證通過", (unsigned)u.totalSize);
    else logf("Update.end 失敗：%s", Update.errorString());
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    logf("上傳中斷");
  }
}

// 從網址下載韌體並更新，成功會直接重新開機，不會回傳
static String pullFirmware(const String& url) {
  if (WiFi.status() != WL_CONNECTED) return "板子還沒連上外部 Wi-Fi";
  logf("從網址更新：%s", url.c_str());
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.setLedPin(-1);
  t_httpUpdate_return r;
  if (url.startsWith("https://")) {
    WiFiClientSecure c;
    c.setInsecure();  // 開發階段先不驗憑證
    r = httpUpdate.update(c, url);
  } else {
    WiFiClient c;
    r = httpUpdate.update(c, url);
  }
  if (r == HTTP_UPDATE_NO_UPDATES) return "伺服器回應沒有更新";
  String err = "更新失敗：" + httpUpdate.getLastErrorString();
  logf("%s", err.c_str());
  return err;
}

static void handlePull() {
  const String url = server.arg("url");
  if (url.isEmpty()) { server.send(200, "text/plain; charset=utf-8", "請填網址"); return; }
  server.send(200, "text/plain; charset=utf-8", pullFirmware(url));
}

// 讀 manifest.txt（build=N 與 url=...），版本比目前新就更新
static String checkManifest() {
  if (autoUrl.isEmpty()) return "還沒設定版本資訊網址";
  if (WiFi.status() != WL_CONNECTED) return "板子還沒連上外部 Wi-Fi";
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  if (autoUrl.startsWith("https://")) {
    secure.setInsecure();
    http.begin(secure, autoUrl);
  } else {
    http.begin(plain, autoUrl);
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    String msg = "讀取版本資訊失敗，HTTP " + String(code);
    logf("%s", msg.c_str());
    return msg;
  }
  const String body = http.getString();
  http.end();
  long build = -1;
  String fwUrl;
  int start = 0;
  while (start < (int)body.length()) {
    int end = body.indexOf('\n', start);
    if (end < 0) end = body.length();
    String line = body.substring(start, end);
    line.trim();
    if (line.startsWith("build=")) build = line.substring(6).toInt();
    else if (line.startsWith("url=")) fwUrl = line.substring(4);
    start = end + 1;
  }
  if (build < 0 || fwUrl.isEmpty()) return "版本資訊格式不對，需要 build= 與 url= 兩行";
  logf("版本資訊：伺服器 build=%ld，目前 build=%d", build, FW_BUILD);
  if (build <= FW_BUILD) return "已經是最新版（build " + String(FW_BUILD) + "）";
  return pullFirmware(fwUrl);
}

static void handleAuto() {
  autoUrl = server.arg("url");
  autoUrl.trim();
  autoMinutes = server.arg("min").toInt();
  prefs.putString("autoUrl", autoUrl);
  prefs.putUInt("autoMin", autoMinutes);
  lastAutoCheck = millis();
  logf("定期檢查：每 %lu 分鐘，%s", (unsigned long)autoMinutes, autoUrl.c_str());
  server.send(200, "text/plain; charset=utf-8",
              autoMinutes ? "已儲存，每 " + String(autoMinutes) + " 分鐘檢查一次" : String("已停用定期檢查"));
}

static void handleCheck() { server.send(200, "text/plain; charset=utf-8", checkManifest()); }

// ---- captive portal ----

static void handleNotFound() {
  // 系統檢查網路用的網址（captive.apple.com、connectivitycheck.gstatic.com 等）都會走到這裡，
  // 一律轉到控制頁，手機就會跳出登入視窗
  if (server.hostHeader() != WiFi.softAPIP().toString()) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "not found");
}

// ---- 手機藍牙指令 ----

static const char BLE_HELP[] =
    "指令：\n"
    "info  晶片資訊\n"
    "temp  晶片溫度\n"
    "led R G B  設定 LED 顏色（0 到 255），led off 關燈\n"
    "touch  觸控腳位數值\n"
    "wifi  掃描 Wi-Fi，列出最強的 8 個\n"
    "ble  掃描 BLE 3 秒，列出最強的 8 個\n"
    "bench  運算效能測試（約 10 秒）\n"
    "ip  熱點與外部 Wi-Fi 的 IP\n"
    "藍牙配網（讓板子連上家裡的 Wi-Fi）：\n"
    "ssid 網路名稱  先送出要連的網路名稱\n"
    "wpass 密碼  再送出密碼，板子開始連線\n"
    "forget  忘記外部 Wi-Fi\n";

static String bleCommand(const String& line) {
  String cmd = line, arg;
  const int sp = line.indexOf(' ');
  if (sp > 0) { cmd = line.substring(0, sp); arg = line.substring(sp + 1); arg.trim(); }
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") return BLE_HELP;
  if (cmd == "ssid") {
    if (!arg.length() || arg.length() > 32) return "網路名稱是 1 到 32 bytes\n";
    pendingSsid = arg;
    return "收到網路名稱「" + arg + "」，接著輸入 wpass 加上密碼\n";
  }
  if (cmd == "wpass") {
    if (!pendingSsid.length()) return "先輸入 ssid 加上網路名稱\n";
    if (arg.length() && (arg.length() < 8 || arg.length() > 63)) return "Wi-Fi 密碼是 8 到 63 個字元，開放網路的話只輸入 wpass\n";
    staSsid = pendingSsid;
    staPass = arg;
    pendingSsid = "";
    prefs.putString("ssid", staSsid);
    prefs.putString("pass", staPass);
    WiFi.disconnect(false, false);
    connectSta();
    staJoinAt = millis();
    logf("藍牙配網：連線到 %s", staSsid.c_str());  // 不記密碼
    return "開始連線到「" + staSsid + "」，最多約 20 秒，連上後會通知\n";
  }
  if (cmd == "forget") {
    staSsid = staPass = pendingSsid = "";
    prefs.putString("ssid", "");
    prefs.putString("pass", "");
    WiFi.disconnect(false, true);
    logf("藍牙配網：忘記外部 Wi-Fi");
    return "已忘記外部 Wi-Fi\n";
  }
  if (cmd == "temp") return "晶片溫度 " + String(temperatureRead(), 1) + " °C\n";
  if (cmd == "info") {
    const esp_partition_t* run = esp_ota_get_running_partition();
    return String(ESP.getChipModel()) + " rev " + ESP.getChipRevision() + "，" + ESP.getChipCores() + " 核 " +
           ESP.getCpuFreqMHz() + " MHz\n" + "溫度 " + String(temperatureRead(), 1) + " °C\n" +
           "可用記憶體 " + ESP.getFreeHeap() / 1024 + " KB，PSRAM 可用 " + ESP.getFreePsram() / 1024 + " KB\n" +
           "開機 " + millis() / 1000 + " 秒，build " + FW_BUILD + "，分區 " + (run ? run->label : "?") + "\n";
  }
  if (cmd == "ip") {
    String r = "熱點 " + apSsid + " " + WiFi.softAPIP().toString() + "，" + WiFi.softAPgetStationNum() + " 台連線\n";
    r += WiFi.status() == WL_CONNECTED ? "外部 Wi-Fi " + WiFi.SSID() + " " + WiFi.localIP().toString() + "\n"
                                       : String("外部 Wi-Fi 未連線\n");
    return r;
  }
  if (cmd == "led") {
    if (arg == "off") { rgbLedWrite(RGB_LED_GPIO, 0, 0, 0); return "LED 關閉\n"; }
    int r = 0, g = 0, b = 0;
    if (sscanf(arg.c_str(), "%d %d %d", &r, &g, &b) != 3) return "用法：led 255 0 0\n";
    rgbLedWrite(RGB_LED_GPIO, constrain(r, 0, 255), constrain(g, 0, 255), constrain(b, 0, 255));
    return "LED " + String(r) + " " + g + " " + b + "\n";
  }
  if (cmd == "touch") {
    String r;
    for (size_t i = 0; i < sizeof(TOUCH_PINS); i++) r += "GPIO" + String(TOUCH_PINS[i]) + " " + touchRead(TOUCH_PINS[i]) + "\n";
    return r;
  }
  if (cmd == "wifi") {
    const int16_t n = WiFi.scanNetworks(false, true);
    String r = String(n) + " 個基地台\n";
    std::vector<int> idx;
    for (int i = 0; i < n; i++) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
    for (size_t k = 0; k < idx.size() && k < 8; k++) {
      const String name = WiFi.SSID(idx[k]);
      r += String(WiFi.RSSI(idx[k])) + " ch" + WiFi.channel(idx[k]) + " " + (name.length() ? name : String("（隱藏）")) + "\n";
    }
    WiFi.scanDelete();
    return r;
  }
  if (cmd == "ble") {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);
    NimBLEScanResults res = scan->getResults(3000, false);
    String r = String(res.getCount()) + " 個裝置\n";
    std::vector<const NimBLEAdvertisedDevice*> v;
    for (int i = 0; i < res.getCount(); i++) v.push_back(res.getDevice(i));
    std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->getRSSI() > b->getRSSI(); });
    for (size_t i = 0; i < v.size() && i < 8; i++)
      r += String(v[i]->getRSSI()) + " " + v[i]->getAddress().toString().c_str() + " " + v[i]->getName().c_str() + "\n";
    scan->clearResults();
    return r;
  }
  if (cmd == "bench") {
    bleuart::send("測試中，約 10 秒…\n");
    return bench::run() + "\n";
  }
  return "不認得的指令：" + line + "，輸入 help 看指令\n";
}

void setup() {
  Serial.begin(115200);
  delay(300);
  rgbLedWrite(RGB_LED_GPIO, 0, 0, 20);

  prefs.begin("playground", false);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  autoUrl = prefs.getString("autoUrl", "");
  autoMinutes = prefs.getUInt("autoMin", 0);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  // 熱點與連外用的 Station 每次開機都換一組隨機的 MAC（本地管理、單播），跟手機的
  // 「私人 Wi-Fi 位址」一樣，周圍的人與連上的路由器都沒辦法用固定的位址認出這塊板子
  esp_wifi_stop();
  for (wifi_interface_t ifx : {WIFI_IF_STA, WIFI_IF_AP}) {
    uint8_t m[6];
    esp_fill_random(m, sizeof(m));
    m[0] = (m[0] & 0xFC) | 0x02;
    esp_wifi_set_mac(ifx, m);
  }
  esp_wifi_start();
  // 名稱後綴第一次開機時隨機產生，不用 MAC 末四碼
  String sfx = prefs.getString("namesfx", "");
  if (sfx.length() != 4) {
    char b[5];
    snprintf(b, sizeof(b), "%04X", (unsigned)(esp_random() & 0xFFFF));
    sfx = b;
    prefs.putString("namesfx", sfx);
  }
  apSsid = "S3-Playground-" + sfx;
  WiFi.softAP(apSsid.c_str(), AP_PASS);
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());

  NimBLEDevice::init("");
  // 藍牙對外使用定期更換的隨機位址（RPA），配對時把解析金鑰交給手機
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT);
  blePin = prefs.getUInt("blepin", 0);
  if (blePin < 100000 || blePin > 999999) {
    blePin = 100000 + esp_random() % 900000;
    prefs.putUInt("blepin", blePin);
  }
  bleuart::begin(apSsid, blePin);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/led", HTTP_GET, handleLed);
  server.on("/api/touch", HTTP_GET, handleTouch);
  server.on("/speed", HTTP_GET, handleSpeed);
  server.on("/api/wifiscan", HTTP_GET, handleWifiScan);
  server.on("/api/blescan", HTTP_GET, handleBleScan);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/pull", HTTP_POST, handlePull);
  server.on("/api/auto", HTTP_POST, handleAuto);
  server.on("/api/check", HTTP_POST, handleCheck);
  server.on("/api/bench", HTTP_GET, []() {
    logf("效能測試開始");
    const String j = bench::run();
    logf("效能測試完成：%s", j.c_str());
    sendJson(j);
  });
  server.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.onNotFound(handleNotFound);
  server.begin();

  connectSta();

  const esp_partition_t* run = esp_ota_get_running_partition();
  logf("開機：build %d，分區 %s", FW_BUILD, run ? run->label : "?");
  logf("熱點 %s，密碼 %s，控制頁 http://%s/", apSsid.c_str(), AP_PASS, WiFi.softAPIP().toString().c_str());
  Serial.printf("藍牙配對碼：%06lu\n", (unsigned long)blePin);
  rgbLedWrite(RGB_LED_GPIO, 0, 20, 0);
}

void loop() {
  dns.processNextRequest();

  String bleLine;
  if (bleuart::receive(bleLine) && bleLine.length()) {
    logf("藍牙指令：%s", bleLine.c_str());
    bleuart::send(bleCommand(bleLine));
  }
  static bool bleWas = false;
  if (bleuart::secure != bleWas) {
    bleWas = bleuart::secure;
    logf(bleWas ? "手機已用藍牙配對連線" : "手機的藍牙連線中斷");
    if (bleWas) bleuart::send(String("已配對連線到 ") + apSsid + "，輸入 help 看指令\n");
  }
  // 藍牙配網的結果
  if (staJoinAt) {
    if (WiFi.status() == WL_CONNECTED) {
      bleuart::send("已連上「" + WiFi.SSID() + "」，IP " + WiFi.localIP().toString() + "\n");
      staJoinAt = 0;
    } else if (millis() - staJoinAt > 20000) {
      bleuart::send("20 秒內沒有連上，請確認名稱與密碼，或板子是否在訊號範圍內（只支援 2.4 GHz）\n");
      staJoinAt = 0;
    }
  }
  server.handleClient();

  // 連上外部 Wi-Fi 後先檢查一次，之後依設定的間隔
  static wl_status_t lastStatus = WL_IDLE_STATUS;
  const wl_status_t st = WiFi.status();
  if (st != lastStatus) {
    if (st == WL_CONNECTED) {
      logf("外部 Wi-Fi 已連線，IP %s", WiFi.localIP().toString().c_str());
      lastAutoCheck = millis() - autoMinutes * 60000UL + 10000;  // 10 秒後第一次檢查
    }
    lastStatus = st;
  }
  if (autoMinutes > 0 && st == WL_CONNECTED && !autoUrl.isEmpty() &&
      millis() - lastAutoCheck >= autoMinutes * 60000UL) {
    lastAutoCheck = millis();
    const String r = checkManifest();
    logf("定期檢查：%s", r.c_str());
  }
  delay(2);
}
