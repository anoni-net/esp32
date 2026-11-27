// 從網址下載韌體更新。
//
// 板子用 Station 介面連上可以上網的 Wi-Fi（家裡或辦公室的基地台），經由 HTTPS 讀取
// assets.anoni.net 上的版本資訊 latest.json，建置時間跟自己不同就下載韌體，邊寫進另一個
// 程式槽邊算 SHA-256，跟 latest.json 寫的相符才切換。憑證用 ESP-IDF 內建的根憑證組合驗證。
// 網址固定在程式裡，面板與序列埠都不能改，知道熱點密碼的人也只能裝上社群發布的版本。
//
// latest.json 的格式（tools/publish_firmware.sh 產生）：
//   {"build":"Nov 26 2026 10:12:03","file":"tracker-20261126-101203.bin","size":1320687,
//    "sha256":"…64 個十六進位字元…","commit":"5c0007b"}
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include "updcheck.h"
// 訊息會顯示在面板上。專案有 i18n.h（例如隨身偵測器）時跟著翻譯，沒有時維持正體中文
#if __has_include("i18n.h")
#include "i18n.h"
#endif
#ifndef TR
#define TR(s) (s)
#endif
#include <mbedtls/platform.h>
#include <lwip/dns.h>

// ESP-IDF 預設的配置函式（esp_mem.h，只用內部 RAM），更新結束後換回來。esp_mem.h 沒有 extern "C"，
// 直接用連結時的符號名稱，避免 C++ 名稱修飾
void* idfTlsCalloc(size_t n, size_t size) __asm__("esp_mbedtls_mem_calloc");
void idfTlsFree(void* ptr) __asm__("esp_mbedtls_mem_free");

namespace netupdate {

// 兩個發布通道，網址都寫死在程式裡，由各工具在 include 之前定義 NETUPDATE_URL_STABLE 與
// NETUPDATE_URL_TEST（結尾帶 /）。正式版是實機測試過、從測試版原封不動複製過去的同一個檔案。
// 測試版是每次修改都發布的版本，給開發用的板子
#if !defined(NETUPDATE_URL_STABLE) || !defined(NETUPDATE_URL_TEST)
#error "include netupdate.h 之前要定義 NETUPDATE_URL_STABLE 與 NETUPDATE_URL_TEST"
#endif
static const char* URL_STABLE = NETUPDATE_URL_STABLE;
static const char* URL_TEST = NETUPDATE_URL_TEST;
static const char* BASE_URL = URL_STABLE;
static void setTestChannel(bool test) { BASE_URL = test ? URL_TEST : URL_STABLE; }
static const char* HOST = "assets.anoni.net";
static const char* MANIFEST = "latest.json";
static const uint32_t JOIN_TIMEOUT_MS = 20000;
static const uint32_t STALL_TIMEOUT_MS = 20000;

enum State : uint8_t {
  IDLE,
  CONNECTING,   // 連線到 Wi-Fi
  CHECKING,     // 讀取 latest.json
  DOWNLOADING,  // 下載並寫入另一個程式槽
  DONE,         // 寫入完成，等主程式重新開機
  LATEST,       // 已經是最新版
  FAILED,
  NONET,        // 自動更新：被動掃描沒看到更新用的 Wi-Fi，沒有連線
};

static volatile State state = IDLE;
static volatile uint32_t got = 0, total = 0;
static char msg[384] = "";
static char newBuild[24] = "";
static char newRev[24] = "";  // latest.json 的 commit，面板顯示用
// 結果是 LATEST 時，msg 是否要顯示給使用者（遠端的版本比較舊、上次自動安裝失敗）。面板用它決定顯示 msg
// 還是「已經是最新版」，不比對 msg 的文字，翻譯過的韌體也一樣判斷
static bool latestNote = false;
static String ssid, pass;
static const char* currentBuild = "";
static bool probeFirst = false;  // 自動更新：先被動掃描，附近有這個 Wi-Fi 才連線
static String skipBuild;         // 自動安裝後沒有正常開機的版本，不再自動安裝

static bool busy() { return state == CONNECTING || state == CHECKING || state == DOWNLOADING; }

static void setMsg(const char* m) { strlcpy(msg, m, sizeof(msg)); }

static uint8_t buf[4096];
static wifi_mode_t prevMode = WIFI_MODE_NULL;

// TLS 的緩衝區預設只能放內部 RAM（CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC），板子開著藍牙、熱點與面板跑久了，
// 內部 RAM 被切碎，最大的連續空間可能不到交握需要的 40 KB。更新期間改成優先放 PSRAM（8 MB），
// 放不下才用內部 RAM，跟 ESP-IDF 的 CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC 相同。free 兩邊都能處理
// 這次更新的加密連線在 PSRAM 與內部 RAM 各配置了多少（累計 bytes），面板與記錄用來確認改用 PSRAM 有作用
static volatile uint32_t tlsPsBytes = 0, tlsIntBytes = 0;
static void* tlsCalloc(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    tlsPsBytes += n * size;
    return p;
  }
  p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p) tlsIntBytes += n * size;
  return p;
}
static void tlsFree(void* p) { heap_caps_free(p); }

static State fin(State s, const char* m) {
  setMsg(m);
  return s;
}

// TLS 的緩衝區只能用內部 RAM，交握時要約 40 KB 的連續空間。連線失敗時附上剩下多少，
// 分得出是網路的問題還是板子的記憶體不夠
static const uint32_t TLS_NEED_BYTES = 40000;
static IPAddress hostIp, dnsIp;  // 查到的位址與用的 DNS 伺服器，連線失敗時寫進訊息
static void connectError(char* m, size_t n, const char* what, int code, NetworkClientSecure& client) {
  char tls[64] = "";
  client.lastError(tls, sizeof(tls));
  const uint32_t freeKb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
  const uint32_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  snprintf(m, n, TR("%s（%s%s%s，%s → %s，DNS %s，記憶體剩 %lu KB、最大 %lu KB）%s"), what,
           HTTPClient::errorToString(code).c_str(), tls[0] ? TR("：") : "", tls, HOST, hostIp.toString().c_str(),
           dnsIp.toString().c_str(), (unsigned long)freeKb, (unsigned long)(big / 1024),
           big < TLS_NEED_BYTES && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < TLS_NEED_BYTES
               ? TR("，記憶體不夠，重新開機後再試") : "");
  // iPhone 個人熱點的網段。手機同時連著板子的熱點時，個人熱點不會替板子轉送連線
  if (dnsIp[0] == 172 && dnsIp[1] == 20 && dnsIp[2] == 10) {
    strlcat(m, TR("。手機連著開發板的熱點時，iPhone 的個人熱點沒辦法替開發板上網：打開自動更新，手機離開開發板的熱點後，開發板會自己連上個人熱點更新"), n);
  }
}

static State work() {
  char m[384];
  WiFi.persistent(false);
  WiFi.mode((prevMode & WIFI_MODE_AP) ? WIFI_AP_STA : WIFI_STA);
  if (probeFirst) {
    // 被動掃描只聽基地台的 beacon，不送出帶著 Wi-Fi 名稱的探測請求。不在家時，板子不會
    // 一直對外喊家裡的 Wi-Fi 名稱，也不會為了連不上的網路停掉藍牙掃描 20 秒。
    // 隱藏名稱的 Wi-Fi 在被動掃描裡沒有名稱，自動更新找不到它
    const int16_t n = WiFi.scanNetworks(false, false, true, 150);
    if (n < 0) {
      snprintf(m, sizeof(m), TR("掃描 Wi-Fi 失敗（%d）"), n);
      return fin(FAILED, m);
    }
    bool seen = false;
    for (int i = 0; i < n && !seen; i++) seen = WiFi.SSID(i) == ssid;
    WiFi.scanDelete();
    if (!seen) {
      snprintf(m, sizeof(m), TR("附近沒有 Wi-Fi「%s」"), ssid.c_str());
      return fin(NONET, m);
    }
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < JOIN_TIMEOUT_MS) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(m, sizeof(m), TR("連不上 Wi-Fi「%s」，請確認名稱、密碼，而且是 2.4 GHz 的網路"), ssid.c_str());
    return fin(FAILED, m);
  }

  state = CHECKING;
  // lwIP 的 DNS 伺服器是所有介面共用的，熱點設定 DNS（自動跳出面板用）時可能蓋掉基地台
  // 給的那一組，查詢就被板子自己的 DNS 攔走、回答熱點的位址。查到熱點的網段時，改用
  // 基地台的閘道與 1.1.1.1 重新查一次
  // lwIP 的 DNS 伺服器清單是所有介面共用的。熱點設定 DNS（自動跳出面板用）可能蓋掉基地台給的那一組，
  // 路由器有 IPv6 時清單裡也可能混進 IPv6 的伺服器。WiFi.dnsIP() 是 Station 介面上的設定，不一定是實際
  // 查詢用的，所以另外讀 lwIP 的。查不到時依序改用「基地台的閘道 + 1.1.1.1」、「8.8.8.8 + 1.1.1.1」再查
  const IPAddress apIp = WiFi.softAPIP();
  auto inApNet = [&](const IPAddress& a) {
    return (prevMode & WIFI_MODE_AP) && a[0] == apIp[0] && a[1] == apIp[1] && a[2] == apIp[2];
  };
  IPAddress ip;
  dnsIp = WiFi.dnsIP(0);
  char used[48] = "";
  auto lwipDns = [&]() {
    const ip_addr_t* a = dns_getserver(0);
    strlcpy(used, a ? ipaddr_ntoa(a) : "?", sizeof(used));
  };
  // hostByName 失敗時傳回 lwIP 的錯誤碼（8 位元，-54 是 EAI_FAIL 查詢失敗、-53 是 EAI_MEMORY 記憶體不夠），
  // 不是 0，只有 1 才算查到
  auto lookup = [&]() {
    int e = 0;
    for (int i = 0; i < 2; i++) {
      e = WiFi.hostByName(HOST, ip);
      if (e == 1 && !inApNet(ip)) return 1;
      delay(500);
    }
    return e == 1 ? -1 : e;  // 查到熱點自己的網段也算失敗
  };
  lwipDns();
  int dnsErr = lookup();
  if (dnsErr != 1) {
    WiFi.setDNS(WiFi.gatewayIP(), IPAddress(1, 1, 1, 1));
    dnsErr = lookup();
  }
  if (dnsErr != 1) {
    WiFi.setDNS(IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
    dnsErr = lookup();
  }
  hostIp = ip;
  if (dnsErr != 1) {
    snprintf(m, sizeof(m),
             TR("連上「%s」了，但查不到 %s 的位址（Wi-Fi 給的 DNS %s，實際查詢 %s，換過閘道與公用 DNS 也失敗，錯誤 %d）。這個 Wi-Fi 可能要先登入，或沒有連上網路"),
             ssid.c_str(), HOST, dnsIp.toString().c_str(), used, dnsErr);
    return fin(FAILED, m);
  }
  static NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(15000);
  http.setUserAgent("anoni-tracker");
  // 加上隨機的查詢字串，避開 Cloudflare 與途中的快取，一定讀到最新的 latest.json
  if (!http.begin(client, String(BASE_URL) + MANIFEST + "?r=" + String(esp_random(), HEX))) return fin(FAILED, TR("無法建立連線"));
  int code = http.GET();
  if (code != 200) {
    if (code < 0) connectError(m, sizeof(m), TR("讀不到版本資訊"), code, client);
    else snprintf(m, sizeof(m), TR("讀不到版本資訊（%d）"), code);
    http.end();
    return fin(FAILED, m);
  }
  const String j = http.getString();
  http.end();
  updcheck::Manifest man;
  const updcheck::Verdict v = updcheck::decide(std::string(j.c_str()), currentBuild, skipBuild.c_str(), sizeof(newBuild), man);
  if (v == updcheck::BAD) return fin(FAILED, TR("版本資訊的格式不對，沒有更新"));
  strlcpy(newBuild, man.build.c_str(), sizeof(newBuild));
  strlcpy(newRev, man.commit.c_str(), sizeof(newRev));
  latestNote = v == updcheck::SKIPPED || v == updcheck::OLDER;
  if (v == updcheck::SAME) return fin(LATEST, TR("已經是最新版"));
  if (v == updcheck::SKIPPED) return fin(LATEST, TR("這個版本上次自動安裝後沒有正常開機，不再自動安裝"));
  if (v == updcheck::BADTIME) return fin(FAILED, TR("讀不懂建置時間，沒有更新"));
  if (v == updcheck::OLDER) {
    snprintf(m, sizeof(m), TR("assets.anoni.net 上的版本（%s）比目前的舊，沒有更新"), man.build.c_str());
    return fin(LATEST, m);
  }
  const uint32_t size = man.size;
  const String file = man.file.c_str(), sha = man.sha.c_str();

  state = DOWNLOADING;
  total = size;
  got = 0;
  if (!http.begin(client, String(BASE_URL) + file)) return fin(FAILED, TR("無法建立連線"));
  code = http.GET();
  if (code != 200) {
    if (code < 0) connectError(m, sizeof(m), TR("下載失敗"), code, client);
    else snprintf(m, sizeof(m), TR("下載失敗（%d）"), code);
    http.end();
    return fin(FAILED, m);
  }
  const int len = http.getSize();
  // 只接受有 Content-Length 的回應，分段傳送（chunked）時串流裡會夾著分段的標頭
  if (len < 0 || (uint32_t)len != size) {
    http.end();
    return fin(FAILED, len < 0 ? TR("伺服器沒有回報檔案大小，沒有更新") : TR("檔案大小跟版本資訊不符，沒有更新"));
  }
  if (!Update.begin(size)) {
    snprintf(m, sizeof(m), TR("無法開始寫入：%s"), Update.errorString());
    http.end();
    return fin(FAILED, m);
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  NetworkClient* s = http.getStreamPtr();
  uint32_t lastData = millis();
  bool ok = true;
  while (got < size) {
    const size_t avail = s->available();
    if (avail) {
      const size_t want = min(min(avail, sizeof(buf)), (size_t)(size - got));
      const int n = s->readBytes(buf, want);
      if (n <= 0) continue;
      if (Update.write(buf, n) != (size_t)n) {
        ok = false;
        snprintf(m, sizeof(m), TR("寫入失敗：%s"), Update.errorString());
        break;
      }
      mbedtls_sha256_update(&ctx, buf, n);
      got += n;
      lastData = millis();
    } else if (!s->connected() || millis() - lastData > STALL_TIMEOUT_MS) {
      ok = false;
      snprintf(m, sizeof(m), TR("下載中斷，收到 %lu / %lu bytes"), (unsigned long)got, (unsigned long)size);
      break;
    } else {
      delay(5);
    }
  }
  http.end();
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (!ok) {
    Update.abort();
    return fin(FAILED, m);
  }
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
  if (!sha.equalsIgnoreCase(hex)) {
    Update.abort();
    return fin(FAILED, TR("SHA-256 跟版本資訊不符，沒有更新"));
  }
  if (!Update.end(true)) {
    snprintf(m, sizeof(m), TR("驗證失敗：%s"), Update.errorString());
    return fin(FAILED, m);
  }
  return fin(DONE, TR("下載完成，重新開機換成新版"));
}

// 在函式外收尾，work() 裡的 HTTPClient 與字串這時都已經釋放
static void run(void*) {
  prevMode = WiFi.getMode();
  tlsPsBytes = tlsIntBytes = 0;
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  const State s = work();
  mbedtls_platform_set_calloc_free(idfTlsCalloc, idfTlsFree);
  WiFi.disconnect(false, true);  // 斷開 Station，不留下儲存的網路設定
  WiFi.mode(prevMode);
  state = s;
  vTaskDelete(nullptr);
}

// 呼叫前先暫停藍牙掃描。結果看 state，DONE 時由主程式重新開機。
// probe：自動更新，先被動掃描確認附近有這個 Wi-Fi。skip：不安裝的建置時間
static bool start(const String& wifiName, const String& wifiPass, const char* build, bool probe, const String& skip) {
  if (busy() || wifiName.length() == 0) return false;
  probeFirst = probe;
  skipBuild = skip;
  ssid = wifiName;
  pass = wifiPass;
  currentBuild = build;
  got = 0;
  total = 0;
  newBuild[0] = 0;
  newRev[0] = 0;
  setMsg(TR("連線到 Wi-Fi"));
  state = CONNECTING;
  // TLS 交握需要比較大的堆疊
  if (xTaskCreatePinnedToCore(run, "netupdate", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    state = FAILED;
    setMsg(TR("無法建立下載的工作"));
    return false;
  }
  return true;
}

}  // namespace netupdate
