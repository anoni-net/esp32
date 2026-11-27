// 周圍手機洩漏的 Wi-Fi 名稱：統計這個空間的 Wi-Fi 探測請求（probe request）。
//
// 手機找 Wi-Fi 時會送出探測請求。只問「附近有誰」的請求不帶名稱，有些手機會直接問存過的網路
// （家、公司、住過的旅館），名稱就跟著送出來，附近的人都收得到。這裡統計一個空間裡有多少這樣的資訊：
// 收到幾個請求、來自幾個位址、其中幾個是固定位址（沒有隨機化，可以在不同地點認出同一支手機），
// 以及帶出了哪些名稱、各有幾個來源。
//
// - 位址不存原文，只存開始監聽時隨機產生的鹽算出來的雜湊，用來計數，停止或關機就消失
// - 名稱只放在記憶體，面板顯示（研究用，目前顯示完整名稱），不寫進 flash 記錄，也不印到序列埠
// - 新的手機多半每次掃描都換隨機位址，來源數會比實際的手機多
//
// 不依賴 Arduino，韌體與 tests/unit/ 編譯同一份。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

namespace probes {

static const size_t MAX_SOURCES = 2048;  // 不同的來源位址最多記幾個（只存 4 bytes 的雜湊）
static const size_t MAX_SSIDS = 128;     // 不同的名稱最多記幾個
static const uint8_t SRC_PER_SSID = 16;  // 每個名稱最多記幾個來源

// 802.11 管理封包裡的探測請求：frame control 第一個 byte 是 0x40（type 0、subtype 4），
// 送出者是位址 2（offset 10），本文從 24 開始，標籤 0 是 SSID。長度 0 是只問「附近有誰」
static bool parse(const uint8_t* f, size_t len, const uint8_t** mac, const uint8_t** ssid, uint8_t* ssidLen) {
  if (len < 26 || f[0] != 0x40) return false;
  *mac = f + 10;
  size_t i = 24;
  while (i + 2 <= len) {
    const uint8_t tag = f[i], l = f[i + 1];
    if (i + 2 + l > len) return false;
    if (tag == 0) {
      if (l > 32) return false;
      *ssid = f + i + 2;
      *ssidLen = l;
      return true;
    }
    i += 2 + l;
  }
  return false;
}

// 位址的第一個 byte 第 2 個位元是 1：本地管理（隨機）位址
static bool randomMac(const uint8_t* m) { return m[0] & 0x02; }

static uint32_t hashMac(const uint8_t* m, uint32_t salt) {
  uint32_t h = 2166136261u ^ salt;
  for (int i = 0; i < 6; i++) {
    h ^= m[i];
    h *= 16777619u;
  }
  return h;
}

// 名稱可能不是合法的 UTF-8（任意 bytes），不合法的 byte 換成「?」，控制字元換成空白，JSON 才不會壞掉
static std::string printable(const uint8_t* s, size_t n) {
  std::string out;
  size_t i = 0;
  while (i < n) {
    const uint8_t c = s[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    bool ok = len && i + len <= n;
    for (size_t k = 1; ok && k < len; k++) ok = (s[i + k] & 0xC0) == 0x80;
    if (!ok) {
      out += '?';
      i++;
      continue;
    }
    if (len == 1 && (c < 0x20 || c == 0x7f)) out += ' ';
    else out.append((const char*)s + i, len);
    i += len;
  }
  return out;
}

static std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

// 探測請求的指紋與型號（會議室檢查用）。
// 指紋：支援的速度、HT、VHT、HE、延伸功能等欄位的組合，不同廠牌、型號的手機通常不同，同一支手機換了
// 隨機位址也相同。不含名稱（標籤 0）與頻道（標籤 3），廠商自訂欄位只取廠商與類型。
// 型號與名稱：部分 Android 手機與 Windows 筆電在探測請求裡帶 WPS 資訊，取廠牌、型號名稱、型號編號
// 與裝置名稱（0x1011，常帶主人的名字，只放在記憶體、顯示在面板，不寫進記錄）。
// len 不含結尾 4 bytes 的 FCS，不然每個封包的指紋都會不同
struct ProbeInfo {
  uint32_t fp = 0;
  std::string model;  // WPS 的廠牌與型號，沒有時是空字串
  std::string name;   // WPS 的裝置名稱
  bool apple = false;  // 有 Apple 的廠商自訂欄位
};

static ProbeInfo describe(const uint8_t* f, size_t len) {
  ProbeInfo in;
  uint32_t h = 2166136261u;
  auto mix = [&h](uint8_t b) {
    h ^= b;
    h *= 16777619u;
  };
  std::string maker, name, number;
  size_t i = 24;
  while (i + 2 <= len) {
    const uint8_t tag = f[i], l = f[i + 1];
    if (i + 2 + l > len) break;
    const uint8_t* p = f + i + 2;
    if (tag == 221 && l >= 4) {
      for (int k = 0; k < 4; k++) mix(p[k]);
      mix(tag);
      if (p[0] == 0x00 && p[1] == 0x17 && p[2] == 0xF2) in.apple = true;
      if (p[0] == 0x00 && p[1] == 0x50 && p[2] == 0xF2 && p[3] == 0x04) {
        size_t j = 4;
        while (j + 4 <= l) {
          const uint16_t at = (p[j] << 8) | p[j + 1], al = (p[j + 2] << 8) | p[j + 3];
          if (j + 4 + al > l) break;
          const std::string v = al ? printable(p + j + 4, al > 32 ? 32 : al) : "";
          if (at == 0x1011) in.name = v;
          else if (at == 0x1021) maker = v;
          else if (at == 0x1023) name = v;
          else if (at == 0x1024) number = v;
          j += 4 + al;
        }
      }
    } else if (tag != 0 && tag != 3) {
      mix(tag);
      mix(l);
      for (uint8_t k = 0; k < l; k++) mix(p[k]);
    }
    i += 2 + l;
  }
  in.fp = h;
  for (const std::string* part : {&maker, &name, &number}) {
    if (part->empty() || in.model.find(*part) != std::string::npos) continue;
    if (!in.model.empty()) in.model += " ";
    in.model += *part;
  }
  if (in.model.size() > 60) in.model.resize(60);
  return in;
}

struct Ssid {
  std::string name;
  uint32_t frames = 0;
  uint32_t src[SRC_PER_SSID];
  uint8_t nSrc = 0;
  bool fixed = false;  // 有固定位址的來源送出過
  int8_t rssi = -127;  // 最強的訊號
};

class Survey {
 public:
  uint32_t salt = 0;
  uint32_t frames = 0, directed = 0;
  uint32_t fixedSources = 0;
  bool sourcesFull = false, ssidsFull = false;
  std::vector<uint32_t> sources;  // 排序過的雜湊
  std::vector<Ssid> ssids;

  void reset(uint32_t newSalt) {
    salt = newSalt;
    frames = directed = fixedSources = 0;
    sourcesFull = ssidsFull = false;
    sources.clear();
    ssids.clear();
  }

  void add(const uint8_t* mac, const uint8_t* ssid, uint8_t ssidLen, int8_t rssi) {
    frames++;
    const uint32_t h = hashMac(mac, salt);
    auto it = std::lower_bound(sources.begin(), sources.end(), h);
    if (it == sources.end() || *it != h) {
      if (sources.size() < MAX_SOURCES) {
        sources.insert(it, h);
        if (!randomMac(mac)) fixedSources++;
      } else {
        sourcesFull = true;
      }
    }
    if (!ssidLen) return;
    directed++;
    const std::string name = printable(ssid, ssidLen);
    Ssid* s = nullptr;
    for (auto& x : ssids) {
      if (x.name == name) {
        s = &x;
        break;
      }
    }
    if (!s) {
      if (ssids.size() >= MAX_SSIDS) {
        ssidsFull = true;
        return;
      }
      ssids.push_back(Ssid());
      s = &ssids.back();
      s->name = name;
    }
    s->frames++;
    if (rssi > s->rssi) s->rssi = rssi;
    if (!randomMac(mac)) s->fixed = true;
    bool seen = false;
    for (uint8_t k = 0; k < s->nSrc && !seen; k++) seen = s->src[k] == h;
    if (!seen && s->nSrc < SRC_PER_SSID) s->src[s->nSrc++] = h;
  }

  // 面板用的 JSON。名稱依來源數、請求數排序
  std::string json(bool on, uint32_t sec, uint32_t leftSec, uint8_t ch, uint32_t ver) const {
    std::vector<const Ssid*> list;
    for (const auto& s : ssids) list.push_back(&s);
    std::stable_sort(list.begin(), list.end(), [](const Ssid* a, const Ssid* b) {
      return a->nSrc != b->nSrc ? a->nSrc > b->nSrc : a->frames > b->frames;
    });
    char b[200];
    snprintf(b, sizeof(b),
             "{\"ver\":%lu,\"on\":%s,\"sec\":%lu,\"left\":%lu,\"ch\":%u,\"frames\":%lu,\"src\":%u,\"fixed\":%lu,"
             "\"directed\":%lu,\"full\":%s,\"ssids\":[",
             (unsigned long)ver, on ? "true" : "false", (unsigned long)sec, (unsigned long)leftSec, (unsigned)ch,
             (unsigned long)frames, (unsigned)sources.size(), (unsigned long)fixedSources, (unsigned long)directed,
             sourcesFull || ssidsFull ? "true" : "false");
    std::string j = b;
    for (size_t i = 0; i < list.size(); i++) {
      const Ssid& s = *list[i];
      snprintf(b, sizeof(b), "%s{\"src\":%u,\"f\":%lu,\"fx\":%s,\"r\":%d,\"n\":\"", i ? "," : "", (unsigned)s.nSrc,
               (unsigned long)s.frames, s.fixed ? "true" : "false", (int)s.rssi);
      j += b;
      j += jsonEscape(s.name) + "\"}";
    }
    j += "]}";
    return j;
  }
};

}  // namespace probes
