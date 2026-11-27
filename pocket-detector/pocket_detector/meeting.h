// 會議室檢查：收走手機之後，監看房間裡有沒有還開著藍牙或 Wi-Fi 的裝置。
//
// 開始後 BASE_MS 建立基準：記下這時收得到、訊號強於門檻的藍牙裝置（多半是隔壁、走廊或房間裡
// 固定的設備），以及近距離 Wi-Fi 探測請求的頻率。之後持續監看：
//
// - 藍牙：基準之外出現新的強訊號裝置就列出來。iPhone、Mac 的藍牙位址約 15 分鐘換一次，同一類的
//   基準裝置在 SWAP_MS 內消失時，把新出現的當成同一台換了位址，不提醒
// - Wi-Fi：手機收走之後近距離不該有探測請求。每分鐘的近距離請求比基準多就提醒。新的手機每次掃描都
//   換隨機位址，只能算次數，算不出幾支手機
//
// 每個裝置記下類型與型號、廣播的名稱、使用中的跡象、Apple 的活動碼、最近 10 次的訊號與出現的時間（devinfo.h）。
// 名稱常帶主人的名字，跟其他資料一樣只放在記憶體、顯示在面板，不寫進記錄。
// Wi-Fi 探測請求依指紋分組（probes.h 的 describe），估計有幾種裝置在找 Wi-Fi，有 WPS 資訊時顯示型號。
//
// 只開行動網路、Wi-Fi 與藍牙都關掉的手機偵測不到。位址只用雜湊比對，藍牙的原始位址只放在記憶體給
// 「尋找」用，不顯示也不記錄。不依賴 Arduino，韌體與 tests/unit/ 編譯同一份。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "devinfo.h"

namespace meeting {

// 建立基準的時間。閒置的手機可能好幾分鐘才找一次 Wi-Fi，2026-11-27 A 板實測 1 分鐘的基準記不全
static const uint32_t BASE_MS = 120000;
static const uint32_t PRESENT_MS = 90000;  // 多久沒收到就算離開
// 同一類的基準裝置在這段時間內消失，新的當成換了位址。2026-11-27 A 板實測，舊位址消失到新位址出現
// 可以超過 2 分鐘，改成 10 分鐘，另外要求類型相同（都知道時）、訊號相差 12 dB 以內
static const uint32_t SWAP_MS = 600000;
static const int SWAP_RSSI = 12;
static const uint32_t RATE_MS = 120000;    // Wi-Fi 探測請求的頻率用最近多久計算
static const size_t MAX_DEVS = 192;
// 建立基準時多收 10 dB：訊號在門檻上下徘徊的裝置（例如 -72 到 -68 dBm）基準時就記下來，
// 監看時才不會一越過門檻就被當成多出來的。2026-11-27 A 板實測，沒有人移動也冒出 3 個「新」裝置
static const int BASE_MARGIN = 10;
static const size_t MAX_GROUPS = 48;
static const uint32_t USE_RECENT_MS = 60000;  // 多久之內的使用跡象算「正在使用」

using devinfo::Kind;

// 字串存成固定長度的陣列：整個 vector 是一塊大於 4 KB 的記憶體，ESP32 會放在 PSRAM，
// 不會有一堆小的 std::string 把內部 RAM 切碎。截斷時退回 UTF-8 字元的開頭，中文不會被切一半
static void copyText(char* dst, size_t n, const std::string& src) {
  size_t len = src.size() < n - 1 ? src.size() : n - 1;
  while (len > 0 && len < src.size() && ((uint8_t)src[len] & 0xC0) == 0x80) len--;
  memcpy(dst, src.data(), len);
  dst[len] = 0;
}

struct Dev {
  uint32_t key = 0;
  uint8_t addr[6] = {};  // 原始位址，只給「尋找」用
  uint8_t kind = devinfo::OTHER;
  int8_t rssi = 0;       // 最近一次的訊號
  int8_t best = -127;    // 最強的訊號
  bool base = false;     // 基準時就在，或判斷成基準裝置換了位址
  bool swapped = false;
  uint32_t first = 0, last = 0;
  char label[80] = "";   // 類型與型號
  char name[44] = "";    // 廣播的名稱
  uint8_t rank = 0;
  uint8_t flags = 0;     // devinfo::HOTSPOT、FINDMY、ESTIMATE（使用中與 AirDrop 另外記時間）
  int8_t act = -1;       // Apple 的活動碼
  uint32_t useAt = 0, airAt = 0;  // 最後一次 Handoff、AirDrop 的時間，0 代表沒有
  int8_t rh[10] = {};
  uint8_t rhN = 0;
};

// 依指紋分組的 Wi-Fi 探測請求
struct Group {
  uint32_t fp = 0;
  char model[64] = "";
  char name[36] = "";  // WPS 的裝置名稱
  bool apple = false;
  bool base = false;
  uint32_t n = 0, first = 0, last = 0;
  int8_t best = -127;
};

enum Phase : uint8_t { IDLE, BASELINE, WATCH };

class Watch {
 public:
  Phase phase = IDLE;
  int rssiMin = -70;
  uint32_t startedAt = 0, watchFrom = 0;
  std::vector<Dev> devs;
  std::vector<Group> groups;
  std::vector<uint32_t> probeTimes;  // 近距離 Wi-Fi 探測請求的時間（監看期間），最多保留 RATE_MS
  uint32_t baseProbes = 0;            // 建立基準期間的近距離探測請求數
  uint32_t probesTotal = 0;           // 監看期間的近距離探測請求總數
  uint32_t maxNew = 0;                // 監看期間同時多出幾個裝置的最大值，結束時寫進記錄

  void start(uint32_t now, int threshold) {
    phase = BASELINE;
    rssiMin = threshold;
    startedAt = now;
    watchFrom = 0;
    devs.clear();
    groups.clear();
    probeTimes.clear();
    baseProbes = probesTotal = maxNew = 0;
  }

  void stop() { phase = IDLE; }

  bool running() const { return phase != IDLE; }

  // 收得到的下限：已經在清單上的裝置與建立基準時用寬鬆的門檻，監看時新出現的要強於門檻
  int floorRssi() const { return rssiMin - BASE_MARGIN; }

  void ble(uint32_t key, const uint8_t* addr, const devinfo::Info& in, int rssi, uint32_t now) {
    if (phase == IDLE || rssi < floorRssi()) return;
    for (auto& d : devs) {
      if (d.key == key) {
        note(d, in, rssi, now);
        return;
      }
    }
    if (phase == WATCH && rssi < rssiMin) return;
    if (devs.size() >= MAX_DEVS) prune(now);
    if (devs.size() >= MAX_DEVS) return;
    Dev d;
    d.key = key;
    memcpy(d.addr, addr, 6);
    d.kind = in.kind;
    d.first = now;
    note(d, in, rssi, now);
    const uint8_t kind = in.kind;
    d.base = phase == BASELINE;
    if (!d.base) {
      // 同一類的基準裝置剛消失（超過 PRESENT_MS 沒收到、但還在 SWAP_MS 內）：多半是換了位址
      for (auto& o : devs) {
        const bool sameLabel = o.rank < 2 || in.rank < 2 || in.label == o.label;
        const int diff = o.rssi > rssi ? o.rssi - rssi : rssi - o.rssi;
        if (o.base && !o.swapped && o.kind == kind && sameLabel && diff <= SWAP_RSSI && now - o.last > PRESENT_MS / 3 &&
            now - o.last <= SWAP_MS) {
          o.swapped = true;  // 舊位址用掉了，不再拿來配對
          d.base = d.swapped = true;
          break;
        }
      }
    }
    devs.push_back(d);
  }

  // 依指紋分組：同一種裝置（同一支手機換了隨機位址也一樣）算一組
  void probeDev(uint32_t fp, const std::string& model, const std::string& name, bool apple, int rssi, uint32_t now) {
    if (phase == IDLE || rssi < floorRssi()) return;
    Group* g = nullptr;
    for (auto& x : groups) {
      if (x.fp == fp) {
        g = &x;
        break;
      }
    }
    if (!g) {
      if (groups.size() >= MAX_GROUPS || (phase == WATCH && rssi < rssiMin)) return;
      groups.push_back(Group());
      g = &groups.back();
      g->fp = fp;
      g->first = now;
      g->base = phase == BASELINE;
    }
    g->n++;
    g->last = now;
    if (rssi > g->best) g->best = (int8_t)rssi;
    if (!model.empty()) copyText(g->model, sizeof(g->model), model);
    if (!name.empty()) copyText(g->name, sizeof(g->name), name);
    g->apple = g->apple || apple;
  }

  // 最近 RATE_MS 內出現、基準時沒有的指紋有幾種
  uint32_t newGroups(uint32_t now) const {
    uint32_t n = 0;
    for (const auto& g : groups) n += !g.base && now - g.last <= RATE_MS;
    return n;
  }

  void probe(int rssi, uint32_t now) {
    if (phase == IDLE || rssi < rssiMin) return;
    if (phase == BASELINE) {
      baseProbes++;
      return;
    }
    probesTotal++;
    probeTimes.push_back(now);
  }

  // 每秒呼叫：基準時間到了就開始監看，清掉太舊的資料
  void tick(uint32_t now) {
    if (phase == BASELINE && now - startedAt >= BASE_MS) {
      phase = WATCH;
      watchFrom = now;
    }
    while (!probeTimes.empty() && now - probeTimes.front() > RATE_MS) probeTimes.erase(probeTimes.begin());
    if (phase == WATCH) {
      const uint32_t n = newCount(now);
      if (n > maxNew) maxNew = n;
    }
  }

  bool present(const Dev& d, uint32_t now) const { return now - d.last <= PRESENT_MS; }

  uint32_t newCount(uint32_t now) const {
    uint32_t n = 0;
    for (const auto& d : devs) n += !d.base && present(d, now);
    return n;
  }

  uint32_t baseCount(uint32_t now) const {
    uint32_t n = 0;
    for (const auto& d : devs) n += d.base && present(d, now);
    return n;
  }

  // 每分鐘的近距離探測請求：監看期間用最近 RATE_MS，基準用整段基準時間
  float probeRate(uint32_t now) const {
    if (phase != WATCH) return 0;
    const uint32_t span = now - watchFrom < RATE_MS ? now - watchFrom : RATE_MS;
    return span ? probeTimes.size() * 60000.0f / span : 0;
  }
  float baseRate() const { return baseProbes * 60000.0f / BASE_MS; }

  // Wi-Fi 的提醒：最近每分鐘的近距離探測請求比基準多 1 個以上，而且至少有 2 個
  // Wi-Fi 的動靜比藍牙雜：隔壁、門外的手機也會不定時找 Wi-Fi。最近每分鐘的近距離請求超過基準的 2 倍再加 2 個
  // （至少 3 個），或基準時沒有的指紋收到 2 次以上，才標成「留意」，不跟多出藍牙裝置同一個等級
  uint32_t newGroupsSeen(uint32_t now, uint32_t minN) const {
    uint32_t n = 0;
    for (const auto& g : groups) n += !g.base && g.n >= minN && now - g.last <= RATE_MS;
    return n;
  }

  bool wifiAlert(uint32_t now) const {
    return phase == WATCH &&
           ((probeTimes.size() >= 3 && probeRate(now) >= baseRate() * 2 + 2) || newGroupsSeen(now, 2) > 0);
  }

  // 2 多出藍牙裝置（提醒）、1 只有 Wi-Fi 的動靜（留意）、0 沒有
  uint8_t level(uint32_t now) const { return newCount(now) > 0 ? 2 : wifiAlert(now) ? 1 : 0; }

  bool alert(uint32_t now) const { return level(now) > 0; }

  // 面板用的 JSON。i 是 devs 的索引，「尋找」用。位址不在裡面
  std::string json(uint32_t now, uint32_t ver) const {
    char b[220];
    const uint32_t left = phase == BASELINE && now - startedAt < BASE_MS ? (BASE_MS - (now - startedAt)) / 1000 : 0;
    snprintf(b, sizeof(b),
             "{\"ver\":%lu,\"phase\":%u,\"rssi\":%d,\"sec\":%lu,\"baseLeft\":%lu,\"base\":%lu,\"new\":%lu,\"lvl\":%u,\"alert\":%s,"
             "\"rate\":%.1f,\"baseRate\":%.1f,\"probes\":%lu,\"maxNew\":%lu,\"devs\":[",
             (unsigned long)ver, (unsigned)phase, rssiMin, (unsigned long)(running() ? (now - startedAt) / 1000 : 0),
             (unsigned long)left, (unsigned long)baseCount(now), (unsigned long)newCount(now), (unsigned)level(now),
             alert(now) ? "true" : "false",
             probeRate(now), baseRate(), (unsigned long)probesTotal, (unsigned long)maxNew);
    std::string j = b;
    auto rel = [this](uint32_t t) { return (unsigned long)((t - startedAt) / 1000); };
    bool first = true;
    // 監看期間出現過、基準之外的裝置，離開的也列出（時間軸用），present 標出現在還在不在
    for (size_t i = 0; i < devs.size(); i++) {
      const Dev& d = devs[i];
      if (d.base) continue;
      const char* act = d.act >= 0 ? devinfo::activityText(d.act) : "";
      snprintf(b, sizeof(b),
               "%s{\"i\":%u,\"r\":%d,\"best\":%d,\"ago\":%lu,\"since\":%lu,\"first\":%lu,\"last\":%lu,\"now\":%s,"
               "\"fl\":%u,\"use\":%ld,\"air\":%ld,\"rh\":[",
               first ? "" : ",", (unsigned)i, (int)d.rssi, (int)d.best, (unsigned long)((now - d.last) / 1000),
               (unsigned long)((now - d.first) / 1000), rel(d.first), rel(d.last), present(d, now) ? "true" : "false",
               (unsigned)d.flags, d.useAt ? (long)((now - d.useAt) / 1000) : -1L, d.airAt ? (long)((now - d.airAt) / 1000) : -1L);
      j += b;
      for (uint8_t k = 0; k < d.rhN; k++) j += (k ? "," : "") + std::to_string(d.rh[k]);
      j += "],\"l\":\"" + esc(d.label) + "\",\"nm\":\"" + esc(d.name) + "\",\"act\":\"" + act + "\"}";
      first = false;
    }
    j += "],\"groups\":" + std::to_string(newGroups(now)) + ",\"wg\":[";
    first = true;
    for (const auto& g : groups) {
      if (g.base) continue;
      snprintf(b, sizeof(b), "%s{\"fp\":\"%08lx\",\"n\":%lu,\"first\":%lu,\"last\":%lu,\"r\":%d,\"now\":%s,\"apple\":%s,\"m\":\"",
               first ? "" : ",", (unsigned long)g.fp, (unsigned long)g.n, rel(g.first), rel(g.last), (int)g.best,
               now - g.last <= RATE_MS ? "true" : "false", g.apple ? "true" : "false");
      j += b;
      j += esc(g.model) + "\",\"nm\":\"" + esc(g.name) + "\"}";
      first = false;
    }
    j += "]}";
    return j;
  }

 private:
  static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
      if (c == '"' || c == '\\') o += '\\';
      o += c;
    }
    return o;
  }

  static void note(Dev& d, const devinfo::Info& in, int rssi, uint32_t now) {
    d.rssi = (int8_t)rssi;
    if (rssi > d.best) d.best = (int8_t)rssi;
    d.last = now;
    if (in.rank >= d.rank && !in.label.empty()) {
      copyText(d.label, sizeof(d.label), in.label);
      d.rank = in.rank;
    }
    d.flags |= in.flags & (devinfo::HOTSPOT | devinfo::FINDMY | devinfo::ESTIMATE);
    if (in.flags & devinfo::IN_USE) d.useAt = now ? now : 1;
    if (in.flags & devinfo::AIRDROP) d.airAt = now ? now : 1;
    if (in.activity >= 0) d.act = in.activity;
    if (!in.name.empty()) copyText(d.name, sizeof(d.name), in.name);
    if (d.rhN < 10) {
      d.rh[d.rhN++] = (int8_t)rssi;
    } else {
      memmove(d.rh, d.rh + 1, 9);
      d.rh[9] = (int8_t)rssi;
    }
  }

  // 滿了就丟掉已經離開的裝置，基準裝置留著配對換位址用
  void prune(uint32_t now) {
    for (size_t i = 0; i < devs.size();) {
      if (!present(devs[i], now) && (!devs[i].base || now - devs[i].last > SWAP_MS)) devs.erase(devs.begin() + i);
      else i++;
    }
  }
};

}  // namespace meeting
