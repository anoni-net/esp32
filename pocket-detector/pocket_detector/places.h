// 地點指紋：認得「這裡來過」，不換算成座標。
//
// 打開移動判斷時，板子每輪掃描周圍的 Wi-Fi 基地台。停留夠久的地點存一份指紋：出現最穩定的
// 24 個基地台。換地點時跟存過的指紋比對，共同的基地台夠多就認成舊地點，沿用同一個編號，
// 使用者可以在面板替它命名（家、公司）。
//
// - BSSID 加上每塊板子自己的隨機鹽做雜湊才存。不能直接貼到公開的基地台資料庫查詢，不同板子的指紋
//   也比對不起來。鹽跟指紋存在同一個檔案，拿到板子的人仍可以拿公開的基地台清單逐一算雜湊比對，
//   推出地點，這只是提高門檻，不是加密
// - 隨機位址（locally administered）的基地台不收，多半是手機熱點，會跟著人移動
// - 只有停留 PLACE_SAVE_SCANS 輪以上才存，通勤途中一閃而過的地方不佔位置
//
// 不依賴 Arduino，韌體與 tests/unit/ 編譯同一份。2026-11-27 用 A 板 11/24 的實測記錄離線模擬：
// 晚上回到出發的地方時跟早上的指紋重疊 83%，兩個白天停留的地方重疊 12%。地下路段每輪只掃到 1 到 4 個
// 基地台，比例很容易虛高，所以另外要求至少 3 個共同的基地台。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

namespace places {

static const uint8_t FP_SIZE = 24;         // 每個地點存幾個基地台
static const uint8_t MAX_PLACES = 32;      // 最多存幾個地點
static const uint8_t NAME_BYTES = 30;        // 名稱最多幾個 bytes（UTF-8，中文約 10 個字）
static const uint8_t MATCH_MIN_SHARED = 3;  // 認成舊地點至少要有幾個共同的基地台
static const uint8_t MATCH_MIN_PCT = 40;    // 共同基地台佔兩邊較小那一邊的比例
static const uint16_t SAVE_SCANS = 10;      // 停留幾輪才存成指紋
static const uint16_t MERGE_EVERY = 20;     // 停留中每幾輪把這段時間的統計併進指紋

struct Entry {
  uint32_t key;
  uint16_t hits;
};

struct Place {
  uint16_t id = 0;
  std::string name;
  uint16_t visits = 0;
  uint32_t lastUse = 0;  // 越大代表越近期來過，滿了先移除最久沒來、沒有名稱的
  std::vector<Entry> fp;
};

// 隨機位址的第一個 byte 第 2 個位元是 1
static bool randomBssid(const uint8_t* b) { return b[0] & 0x02; }

// FNV-1a，先混入鹽
static uint32_t keyOf(const uint8_t* bssid, uint32_t salt) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 4; i++) {
    h ^= (salt >> (8 * i)) & 0xff;
    h *= 16777619u;
  }
  for (int i = 0; i < 6; i++) {
    h ^= bssid[i];
    h *= 16777619u;
  }
  return h;
}

class Book {
 public:
  std::vector<Place> list;
  uint32_t salt = 0;
  uint16_t nextId = 1;
  uint32_t useSeq = 0;
  bool dirty = false;

  // 目前所在的地點。cur 是 list 的索引，-1 代表還沒存成指紋（停留不夠久）
  uint16_t curId = 0;
  int cur = -1;
  uint16_t curScans = 0;

  int find(uint16_t id) const {
    for (size_t i = 0; i < list.size(); i++) {
      if (list[i].id == id) return (int)i;
    }
    return -1;
  }

  // 跟每個存過的指紋比對，傳回最像的索引與分數（百分比），都不夠像時傳回 -1
  int match(const std::vector<uint32_t>& keys, int* scoreOut = nullptr, int* sharedOut = nullptr) const {
    int best = -1, bestScore = 0, bestShared = 0;
    for (size_t i = 0; i < list.size(); i++) {
      const auto& fp = list[i].fp;
      if (fp.empty()) continue;
      int shared = 0;
      for (const auto& e : fp) {
        if (std::find(keys.begin(), keys.end(), e.key) != keys.end()) shared++;
      }
      const int denom = (int)std::min(fp.size(), keys.size());
      const int score = denom ? shared * 100 / denom : 0;
      if (shared >= MATCH_MIN_SHARED && score > bestScore) {
        best = (int)i;
        bestScore = score;
        bestShared = shared;
      }
    }
    if (scoreOut) *scoreOut = bestScore;
    if (sharedOut) *sharedOut = bestShared;
    return bestScore >= MATCH_MIN_PCT ? best : -1;
  }

  // 每輪掃描呼叫一次，keys 是這輪的基地台（已排除隨機位址、算好雜湊）
  void observe(const std::vector<uint32_t>& keys) {
    curScans++;
    for (uint32_t k : keys) {
      auto it = std::find_if(visit.begin(), visit.end(), [k](const Entry& e) { return e.key == k; });
      if (it != visit.end()) {
        if (it->hits < 60000) it->hits++;
      } else if (visit.size() < 400) {
        visit.push_back({k, 1});
      }
    }
    visitScans++;
    if (visitScans >= MERGE_EVERY) commit();
  }

  // 把這段停留的統計併進目前地點的指紋。停留不夠久、還沒有指紋的地點不存
  void commit() {
    if (visit.empty()) return;
    if (cur < 0 && curScans < SAVE_SCANS) return;
    if (cur < 0) cur = add(curId);
    if (cur < 0) {
      visit.clear();
      visitScans = 0;
      return;
    }
    Place& p = list[cur];
    std::vector<Entry> merged;
    for (const auto& e : p.fp) merged.push_back({e.key, (uint16_t)(e.hits / 2)});
    for (const auto& v : visit) {
      auto it = std::find_if(merged.begin(), merged.end(), [&v](const Entry& e) { return e.key == v.key; });
      if (it != merged.end()) it->hits = (uint16_t)std::min<uint32_t>(60000, (uint32_t)it->hits + v.hits);
      else merged.push_back(v);
    }
    std::stable_sort(merged.begin(), merged.end(), [](const Entry& a, const Entry& b) { return a.hits > b.hits; });
    if (merged.size() > FP_SIZE) merged.resize(FP_SIZE);
    p.fp = merged;
    p.lastUse = ++useSeq;
    visit.clear();
    visitScans = 0;
    dirty = true;
  }

  // 換了地點（或剛開機的第一輪）。先收尾上一個地點，再比對指紋。
  // 傳回新的地點編號，revisit 表示認成存過的地點
  uint16_t enter(const std::vector<uint32_t>& keys, bool* revisit = nullptr, int* score = nullptr) {
    if (curId) commit();
    visit.clear();
    visitScans = 0;
    curScans = 0;
    const int i = match(keys, score);
    if (revisit) *revisit = i >= 0;
    if (i >= 0) {
      cur = i;
      curId = list[i].id;
      list[i].visits++;
      list[i].lastUse = ++useSeq;
      dirty = true;
    } else {
      cur = -1;
      curId = takeId();
    }
    return curId;
  }

  // 替地點命名。目前所在的地點還沒存成指紋時，用到目前為止的統計馬上存
  bool rename(uint16_t id, const std::string& name) {
    if (name.size() > NAME_BYTES) return false;
    if (id == curId && cur < 0) {
      cur = add(curId);
      if (cur < 0) return false;
      commitForce();
    }
    const int i = find(id);
    if (i < 0) return false;
    list[i].name = name;
    dirty = true;
    return true;
  }

  // 刪除目前所在的地點時，這裡改用新的編號重新累計，不會用原本的編號馬上存回來
  bool forget(uint16_t id) {
    const int i = find(id);
    if (i < 0) return false;
    list.erase(list.begin() + i);
    if (cur == i) {
      cur = -1;
      visit.clear();
      visitScans = 0;
      curScans = 0;
      curId = takeId();
    } else if (cur > i) {
      cur--;
    }
    dirty = true;
    return true;
  }

  const char* nameOf(uint16_t id) const {
    const int i = find(id);
    return i >= 0 ? list[i].name.c_str() : "";
  }

  // ---- 存檔：一行一個地點，名稱用十六進位，不怕裡面有空白或換行 ----

  std::string serialize() const {
    std::string s;
    char b[64];
    snprintf(b, sizeof(b), "PLACES1 %08lx %u %lu\n", (unsigned long)salt, (unsigned)nextId, (unsigned long)useSeq);
    s += b;
    for (const auto& p : list) {
      snprintf(b, sizeof(b), "%u %u %lu ", (unsigned)p.id, (unsigned)p.visits, (unsigned long)p.lastUse);
      s += b;
      if (p.name.empty()) s += "-";
      for (unsigned char c : p.name) {
        snprintf(b, sizeof(b), "%02x", c);
        s += b;
      }
      for (const auto& e : p.fp) {
        snprintf(b, sizeof(b), " %08lx:%u", (unsigned long)e.key, (unsigned)e.hits);
        s += b;
      }
      s += "\n";
    }
    return s;
  }

  bool parse(const std::string& text) {
    size_t pos = 0;
    auto line = [&](std::string& out) {
      if (pos >= text.size()) return false;
      size_t e = text.find('\n', pos);
      if (e == std::string::npos) e = text.size();
      out = text.substr(pos, e - pos);
      pos = e + 1;
      return true;
    };
    std::string l;
    if (!line(l)) return false;
    unsigned long sl = 0, us = 0;
    unsigned ni = 0;
    if (sscanf(l.c_str(), "PLACES1 %lx %u %lu", &sl, &ni, &us) != 3) return false;
    std::vector<Place> out;
    while (line(l) && out.size() < MAX_PLACES) {
      if (l.empty()) continue;
      Place p;
      unsigned id = 0, visits = 0;
      unsigned long lu = 0;
      int used = 0;
      if (sscanf(l.c_str(), "%u %u %lu %n", &id, &visits, &lu, &used) != 3 || !id) continue;
      const char* c = l.c_str() + used;
      const char* sp = strchr(c, ' ');
      const std::string hex = sp ? std::string(c, sp - c) : std::string(c);
      if (hex != "-") {
        for (size_t k = 0; k + 1 < hex.size() && p.name.size() < NAME_BYTES; k += 2) {
          p.name += (char)strtoul(hex.substr(k, 2).c_str(), nullptr, 16);
        }
      }
      while (sp && p.fp.size() < FP_SIZE) {
        unsigned long key = 0;
        unsigned hits = 0;
        if (sscanf(sp, " %lx:%u", &key, &hits) != 2) break;
        p.fp.push_back({(uint32_t)key, (uint16_t)hits});
        sp = strchr(sp + 1, ' ');
      }
      p.id = (uint16_t)id;
      p.visits = (uint16_t)visits;
      p.lastUse = (uint32_t)lu;
      out.push_back(p);
    }
    list = out;
    salt = (uint32_t)sl;
    nextId = (uint16_t)(ni ? ni : 1);
    useSeq = (uint32_t)us;
    for (const auto& p : list) {
      if (p.id >= nextId) nextId = p.id + 1;
    }
    cur = -1;
    curId = 0;
    dirty = false;
    return true;
  }

 private:
  std::vector<Entry> visit;  // 這段停留期間每個基地台出現幾輪
  uint16_t visitScans = 0;

  uint16_t takeId() {
    uint16_t id = nextId++;
    if (nextId == 0) nextId = 1;
    while (find(id) >= 0 || id == 0) id = nextId++;
    dirty = true;
    return id;
  }

  // 新增一個地點，滿了就移除最久沒來、沒有名稱的那個。全部都有名稱時不存
  int add(uint16_t id) {
    if (list.size() >= MAX_PLACES) {
      int victim = -1;
      for (size_t i = 0; i < list.size(); i++) {
        if (!list[i].name.empty() || (int)i == cur) continue;
        if (victim < 0 || list[i].lastUse < list[victim].lastUse) victim = (int)i;
      }
      if (victim < 0) return -1;
      list.erase(list.begin() + victim);
    }
    Place p;
    p.id = id;
    p.visits = 1;
    p.lastUse = ++useSeq;
    list.push_back(p);
    dirty = true;
    return (int)list.size() - 1;
  }

  void commitForce() {
    const uint16_t keep = curScans;
    curScans = std::max<uint16_t>(curScans, SAVE_SCANS);
    commit();
    curScans = keep;
  }
};

// 警示歷史的 JSON 裡每一個 "places":{…}（地點編號對應的名稱）換成 "places":{}，其他內容不動。
// 恢復原廠設定時用：名稱（家、公司）是個人設定，警示歷史是舉證用的資料要保留。
// 名稱裡的括號與跳脫過的引號不影響判斷。字串裡的引號一定會被跳脫成 \"，不會被誤認成鍵
static std::string stripNames(const std::string& a) {
  static const char KEY[] = "\"places\":{";
  const size_t klen = sizeof(KEY) - 1;
  std::string out;
  out.reserve(a.size());
  size_t i = 0;
  while (true) {
    const size_t k = a.find(KEY, i);
    if (k == std::string::npos) {
      out.append(a, i, std::string::npos);
      break;
    }
    out.append(a, i, k - i);
    out += "\"places\":{}";
    size_t j = k + klen;
    int depth = 1;
    bool inStr = false, esc = false;
    for (; j < a.size() && depth; j++) {
      const char c = a[j];
      if (inStr) {
        if (esc) esc = false;
        else if (c == '\\') esc = true;
        else if (c == '"') inStr = false;
        continue;
      }
      if (c == '"') inStr = true;
      else if (c == '{') depth++;
      else if (c == '}') depth--;
    }
    i = j;
  }
  return out;
}

}  // namespace places
