// OLED 的各個畫面（128×64）。每個畫面只依傳進來的資料繪製，不讀主程式的狀態，
// 主程式依情境挑一個畫面、填好資料再畫。序列埠的 oled demo 用同一組函式畫假資料，
// 沒接 OLED 的板子也能把畫面緩衝區印出來檢查。
//
// 字型只有英文，畫面上的字用英文縮寫與數字。
#pragma once
#include <Arduino.h>
#include <U8g2lib.h>
#include "qrcode.h"  // ESP-IDF 內建的 esp_qrcode

namespace screen {

// ---- 平常：狀態與追蹤器 --------------------------------------------------

struct Row {
  char type[8];
  char tail[6];   // 位址末四碼，例如 a1:b2
  int16_t rssi;
  uint8_t pct;    // 離警示的進度，取各條件裡最低的
  bool alert;     // 警示中、還沒確認
};

struct Normal {
  uint8_t state;  // 0 沒有發現、1 觀察中、2 警示、3 已確認
  uint16_t count;
  uint32_t scan;
  bool scanning;
  uint8_t nRows;
  Row rows[3];
  char toast[24];  // 右下角的短暫提示，例如剛打的記號，空字串代表不顯示
};

// ---- 尋找 ----------------------------------------------------------------

struct Find {
  char type[8];
  char tail[6];
  int16_t rssi;
  bool seen;       // 收過這個位址的廣播
  bool stale;      // 太久沒收到，可能換了位址或離開範圍
  uint16_t agoSec;
  uint16_t leftSec;
};

// ---- 房間檢查 ------------------------------------------------------------

struct Room {
  bool done;
  uint8_t stage;   // 1 藍牙掃描、2 關熱點、3 Wi-Fi 監聽
  uint8_t pct;
  uint16_t leftSec;
  int16_t lv2, lv1, aps, stas, ble;
};

// ---- 熱點的 QR code ------------------------------------------------------

struct Wifi {
  char name[33];
  bool ble;
  char pin[8];
  uint16_t leftSec;
};

// ---- 發聲 ----------------------------------------------------------------

struct Sound {
  char tail[6];
  bool busy;
  int8_t result;  // trackersound::Result
};

// ---- 韌體更新 ------------------------------------------------------------

struct Fw {
  uint8_t kind;  // 0 等待按 BOOT、1 上傳中、2 下載中
  uint16_t leftSec;
  uint32_t got, total;
};

// ---- 主程式挑畫面用 ------------------------------------------------------

enum Scr : uint8_t { SCR_NORMAL, SCR_STATS, SCR_CROWD, SCR_FIND, SCR_ROOM, SCR_WIFI, SCR_SOUND, SCR_FW };

struct View {
  Scr scr;
  union {
    Normal n;
    Find f;
    Room r;
    Wifi w;
    Sound s;
    Fw fw;
    uint32_t scan;  // 統計模式與人流計數：掃描次數變了才重畫
  };
};

// ---- 共用 ----------------------------------------------------------------

static void bar(U8G2& d, int x, int y, int w, int h, uint8_t pct, bool hollow = false) {
  d.drawFrame(x, y, w, h);
  if (!hollow) d.drawBox(x, y, (w * min<int>(pct, 100) + 50) / 100, h);
}

static void center(U8G2& d, int y, const char* s) {
  d.drawStr((128 - d.getStrWidth(s)) / 2, y, s);
}

static void title(U8G2& d, const char* s, bool invert) {
  d.setFont(u8g2_font_helvB10_tr);
  if (invert) {
    d.drawBox(0, 0, 128, 14);
    d.setDrawColor(0);
  }
  d.drawStr(2, 1, s);
  d.setDrawColor(1);
}

static void drawNormal(U8G2& d, const Normal& v) {
  static const char* STATE[] = {"CLEAR", "WATCH", "ALERT", "ACKED"};
  char buf[32];
  const bool alert = v.state == 2;
  d.setFont(u8g2_font_helvB14_tr);
  if (alert) {
    d.drawBox(0, 0, 128, 18);
    d.setDrawColor(0);
  }
  d.drawStr(2, 1, STATE[v.state]);
  d.setFont(u8g2_font_5x8_tf);
  snprintf(buf, sizeof(buf), "%u trk", v.count);
  d.drawStr(127 - d.getStrWidth(buf), 1, buf);
  snprintf(buf, sizeof(buf), "#%lu", (unsigned long)v.scan);
  d.drawStr(127 - d.getStrWidth(buf) - (v.scanning ? 7 : 0), 9, buf);
  if (v.scanning) d.drawBox(123, 10, 4, 4);  // 掃描中
  d.setDrawColor(1);
  if (!alert) d.drawHLine(0, 18, 128);

  if (v.nRows == 0) {
    d.setFont(u8g2_font_6x10_tf);
    center(d, 34, "no trackers nearby");
  }
  for (int i = 0; i < v.nRows; i++) {
    const Row& r = v.rows[i];
    const int y = 21 + i * 14;
    snprintf(buf, sizeof(buf), "%s%-5s %s %4d", r.alert ? ">" : "", r.type, r.tail, r.rssi);
    d.drawStr(0, y, buf);
    snprintf(buf, sizeof(buf), "%u%%", r.pct);
    d.drawStr(127 - d.getStrWidth(buf), y, buf);
    bar(d, 0, y + 9, 128, 3, r.pct);
  }
  if (v.toast[0]) {
    // 右下角反白的小標籤，例如剛打的記號，2 秒後消失
    snprintf(buf, sizeof(buf), "%s", v.toast);
    const int w = d.getStrWidth(buf) + 6;
    d.setDrawColor(0);
    d.drawBox(127 - w - 1, 52, w + 2, 12);
    d.setDrawColor(1);
    d.drawBox(127 - w, 53, w, 11);
    d.setDrawColor(0);
    d.drawStr(127 - w + 3, 55, buf);
    d.setDrawColor(1);
  }
}

static void drawFind(U8G2& d, const Find& v) {
  char buf[32];
  d.setFont(u8g2_font_5x8_tf);
  snprintf(buf, sizeof(buf), "FIND %s %s", v.type, v.tail);
  d.drawStr(0, 0, buf);
  snprintf(buf, sizeof(buf), "%u:%02u", v.leftSec / 60, v.leftSec % 60);
  d.drawStr(127 - d.getStrWidth(buf), 0, buf);
  d.drawHLine(0, 9, 128);
  if (!v.seen) {
    d.setFont(u8g2_font_helvB10_tr);
    center(d, 18, "waiting for");
    center(d, 32, "signal...");
  } else {
    d.setFont(u8g2_font_logisoso28_tn);
    snprintf(buf, sizeof(buf), "%d", v.rssi);
    const int w = d.getStrWidth(buf);
    const int x = (128 - w - 22) / 2;
    d.drawStr(x, 12, buf);
    d.setFont(u8g2_font_6x10_tf);
    d.drawStr(x + w + 3, 30, "dBm");
  }
  // 訊號 -100 到 -40 dBm 對應長條的 0 到 100%。太久沒收到時只畫外框
  const int pct = v.seen ? constrain((v.rssi + 100) * 100 / 60, 0, 100) : 0;
  bar(d, 0, 45, 128, 7, pct, v.stale || !v.seen);
  d.setFont(u8g2_font_5x8_tf);
  if (v.stale) snprintf(buf, sizeof(buf), "LOST? last %us ago", v.agoSec);
  else if (v.seen) snprintf(buf, sizeof(buf), "closer = faster beeps");
  else snprintf(buf, sizeof(buf), "walk around slowly");
  d.drawStr(0, 55, buf);
}

static void drawRoom(U8G2& d, const Room& v) {
  char buf[32];
  if (!v.done) {
    title(d, "ROOM CHECK", false);
    d.drawHLine(0, 15, 128);
    d.setFont(u8g2_font_6x10_tf);
    static const char* STAGE[] = {"starting", "Bluetooth scan", "hotspot off", "listening Wi-Fi"};
    d.drawStr(0, 19, STAGE[min<int>(v.stage, 3)]);
    bar(d, 0, 32, 128, 8, v.pct);
    snprintf(buf, sizeof(buf), "about %us left", v.leftSec);
    d.drawStr(0, 43, buf);
    d.setFont(u8g2_font_5x8_tf);
    d.drawStr(0, 56, "BOOT: cancel");
    return;
  }
  title(d, "ROOM CHECKED", true);
  d.setFont(u8g2_font_6x10_tf);
  snprintf(buf, sizeof(buf), "check: %d", v.lv2);
  d.drawStr(0, 17, buf);
  snprintf(buf, sizeof(buf), "note: %d", v.lv1);
  d.drawStr(68, 17, buf);
  d.setFont(u8g2_font_5x8_tf);
  snprintf(buf, sizeof(buf), "AP %d  dev %d  BT %d", v.aps, v.stas, v.ble);
  d.drawStr(0, 31, buf);
  d.drawStr(0, 43, "join the hotspot and");
  d.drawStr(0, 52, "open the Room tab");
}

// QR code：ESP-IDF 編碼完呼叫 display_func，handle 只在回呼裡有效，所以在回呼裡複製出來
static const int QR_MAX = 29;  // 第 3 版，放大 2 倍是 58 點，放得進 64 點高的螢幕
static uint8_t qrBits[(QR_MAX * QR_MAX + 7) / 8];
static int qrSize = 0;

static void qrCopy(esp_qrcode_handle_t q) {
  const int n = esp_qrcode_get_size(q);
  if (n > QR_MAX) return;
  memset(qrBits, 0, sizeof(qrBits));
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      if (esp_qrcode_get_module(q, x, y)) qrBits[(y * n + x) / 8] |= 1 << ((y * n + x) % 8);
    }
  }
  qrSize = n;
}

// 編碼失敗（字太長，第 3 版放不下）時 qrSize 為 0，畫面改成只顯示文字
static void makeQr(const char* text) {
  qrSize = 0;
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = qrCopy;
  cfg.max_qrcode_version = 3;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&cfg, text);
}

static void drawWifi(U8G2& d, const Wifi& v) {
  char buf[40];
  int x0 = 0;
  if (qrSize) {
    // 亮底暗點，跟紙上印的 QR code 一樣，反過來的很多手機掃不到
    const int s = 2, side = qrSize * s, off = (64 - side) / 2;
    d.drawBox(0, 0, 64, 64);
    d.setDrawColor(0);
    for (int y = 0; y < qrSize; y++) {
      for (int x = 0; x < qrSize; x++) {
        const int i = y * qrSize + x;
        if (qrBits[i / 8] & (1 << (i % 8))) d.drawBox(off + x * s, off + y * s, s, s);
      }
    }
    d.setDrawColor(1);
    x0 = 68;
  }
  d.setFont(u8g2_font_5x8_tf);
  int y = 0;
  d.drawStr(x0, y, qrSize ? "scan to join" : "Wi-Fi hotspot on");
  y += 11;
  // 名稱依寬度換行，最多兩行
  const int perLine = (128 - x0) / 5;
  const size_t len = strlen(v.name);
  for (size_t i = 0; i < len && y < 31; i += perLine) {
    snprintf(buf, sizeof(buf), "%.*s", perLine, v.name + i);
    d.drawStr(x0, y, buf);
    y += 9;
  }
  y = 32;
  d.drawStr(x0, y, "192.168.4.1");
  if (v.ble) {
    snprintf(buf, sizeof(buf), "PIN %s", v.pin);
    d.drawStr(x0, 42, buf);
  }
  snprintf(buf, sizeof(buf), "%us", v.leftSec);
  d.drawStr(127 - d.getStrWidth(buf), 55, buf);
  if (!qrSize) d.drawStr(0, 55, "name too long for QR");
}

static void drawSound(U8G2& d, const Sound& v) {
  // 依 trackersound::Result 的順序
  static const char* RESULT[] = {"SOUND SENT",   "NOT FOUND",   "CAN'T CONNECT", "NO SOUND SVC", "WRITE FAILED",
                                 "NO RESPONSE",  "REFUSED",     "NOT CONNECTABLE", "SKIPPED"};
  static const char* HINT[] = {"listen for it", "moved or out of range", "move closer, try again",
                               "no sound service", "try again", "try again", "owner may be nearby",
                               "not accepting connection", ""};
  char buf[32];
  title(d, "SOUND", false);
  d.setFont(u8g2_font_5x8_tf);
  d.drawStr(127 - d.getStrWidth(v.tail), 3, v.tail);
  d.drawHLine(0, 15, 128);
  d.setFont(u8g2_font_helvB10_tr);
  if (v.busy) {
    center(d, 24, "connecting...");
    d.setFont(u8g2_font_5x8_tf);
    center(d, 44, "up to 20 seconds");
    return;
  }
  const int r = constrain(v.result, 0, 8);
  center(d, 24, RESULT[r]);
  d.setFont(u8g2_font_5x8_tf);
  snprintf(buf, sizeof(buf), "%s", HINT[r]);
  center(d, 44, buf);
}

static void drawFw(U8G2& d, const Fw& v) {
  char buf[32];
  title(d, "FIRMWARE", false);
  d.drawHLine(0, 15, 128);
  d.setFont(u8g2_font_6x10_tf);
  if (v.kind == 0) {
    center(d, 20, "press BOOT once");
    center(d, 32, "to allow update");
    snprintf(buf, sizeof(buf), "%us", v.leftSec);
    center(d, 48, buf);
    return;
  }
  d.drawStr(0, 20, v.kind == 1 ? "uploading..." : "downloading...");
  const uint8_t pct = v.total ? (uint64_t)v.got * 100 / v.total : 0;
  bar(d, 0, 34, 128, 8, pct, !v.total);
  if (v.total) {
    snprintf(buf, sizeof(buf), "%lu / %lu KB", (unsigned long)(v.got / 1024), (unsigned long)(v.total / 1024));
    d.drawStr(0, 46, buf);
  }
}

}  // namespace screen
