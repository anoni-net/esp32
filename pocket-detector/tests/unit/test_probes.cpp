// 探測請求統計的單元測試，編譯韌體用的同一份 probes.h。封包與名稱都是虛構的。
#include "../../pocket_detector/probes.h"

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0, total = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    total++;                                                         \
    if (!(cond)) {                                                   \
      fails++;                                                       \
      std::printf("失敗 %s:%d：%s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)

using namespace probes;

// 組一個探測請求：標頭 24 bytes，送出者在位址 2，本文先放 SSID 標籤，再放支援速率標籤
static std::vector<uint8_t> frame(const uint8_t* mac, const std::string& ssid) {
  std::vector<uint8_t> f(24, 0);
  f[0] = 0x40;
  for (int i = 0; i < 6; i++) f[4 + i] = 0xff, f[10 + i] = mac[i], f[16 + i] = 0xff;
  f.push_back(0);
  f.push_back((uint8_t)ssid.size());
  f.insert(f.end(), ssid.begin(), ssid.end());
  f.push_back(1);
  f.push_back(2);
  f.push_back(0x82);
  f.push_back(0x84);
  return f;
}

static void feed(Survey& s, const uint8_t* mac, const std::string& ssid, int8_t rssi = -60) {
  const auto f = frame(mac, ssid);
  const uint8_t *m, *n = nullptr;
  uint8_t l = 0;
  if (parse(f.data(), f.size(), &m, &n, &l)) s.add(m, n, l, rssi);
}

int main() {
  const uint8_t fixedA[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  const uint8_t randB[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
  const uint8_t randC[6] = {0x06, 0x01, 0x02, 0x03, 0x04, 0x05};

  // 解析
  const auto f = frame(fixedA, "Home-5G");
  const uint8_t *m, *n;
  uint8_t l;
  CHECK(parse(f.data(), f.size(), &m, &n, &l) && l == 7 && std::string((const char*)n, l) == "Home-5G" && m[5] == 0x55);
  auto beacon = f;
  beacon[0] = 0x80;  // 不是探測請求
  CHECK(!parse(beacon.data(), beacon.size(), &m, &n, &l));
  auto cut = f;
  cut.resize(27);  // 名稱被截斷
  CHECK(!parse(cut.data(), cut.size(), &m, &n, &l));
  const auto wild = frame(randB, "");
  CHECK(parse(wild.data(), wild.size(), &m, &n, &l) && l == 0);
  CHECK(randomMac(randB) && !randomMac(fixedA));

  // 統計
  Survey s;
  s.reset(12345);
  feed(s, fixedA, "Home-5G", -50);
  feed(s, fixedA, "Home-5G", -40);
  feed(s, randB, "");
  feed(s, randB, "iTaiwan");
  feed(s, randC, "iTaiwan");
  feed(s, randC, "iTaiwan");
  CHECK(s.frames == 6 && s.directed == 5);
  CHECK(s.sources.size() == 3 && s.fixedSources == 1);
  CHECK(s.ssids.size() == 2);
  const Ssid* home = s.ssids[0].name == "Home-5G" ? &s.ssids[0] : &s.ssids[1];
  const Ssid* tw = home == &s.ssids[0] ? &s.ssids[1] : &s.ssids[0];
  CHECK(home->nSrc == 1 && home->frames == 2 && home->fixed && home->rssi == -40);
  CHECK(tw->nSrc == 2 && tw->frames == 3 && !tw->fixed);

  // JSON：名稱依來源數排序，引號與反斜線跳脫，不合法的 UTF-8 換成 ?
  feed(s, randB, std::string("a\"b\\c"));
  feed(s, randB, std::string("\xff\xfe" "x") + "\x01");
  const std::string j = s.json(true, 60, 240, 6, 3);
  CHECK(j.find("\"ver\":3,\"on\":true,\"sec\":60,\"left\":240,\"ch\":6,\"frames\":8,\"src\":3,\"fixed\":1,\"directed\":7") != std::string::npos);
  CHECK(j.find("\"n\":\"iTaiwan\"") < j.find("\"n\":\"Home-5G\""));
  CHECK(j.find("\"n\":\"a\\\"b\\\\c\"") != std::string::npos);
  CHECK(j.find("\"n\":\"??x \"") != std::string::npos);
  CHECK(printable((const uint8_t*)"家", 3) == "家");
  CHECK(printable((const uint8_t*)"\xe5\xae", 2) == "??");  // 被截斷的中文

  // 換一次監聽，鹽不同，資料清空
  s.reset(999);
  CHECK(s.frames == 0 && s.sources.empty() && s.ssids.empty());

  // 上限
  Survey big;
  big.reset(1);
  for (uint32_t i = 0; i < MAX_SOURCES + 10; i++) {
    uint8_t mm[6] = {0x02, (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i, 0, 0};
    feed(big, mm, "");
  }
  CHECK(big.sources.size() == MAX_SOURCES && big.sourcesFull);

  // 指紋：同一種裝置換了位址與名稱還是同一個指紋，功能不同就不同
  const auto f1 = frame(fixedA, "Home-5G");
  const auto f2 = frame(randB, "Office");
  auto f3 = frame(randB, "Office");
  f3[f3.size() - 1] = 0x8B;  // 支援的速度不同
  CHECK(describe(f1.data(), f1.size()).fp == describe(f2.data(), f2.size()).fp);
  CHECK(describe(f2.data(), f2.size()).fp != describe(f3.data(), f3.size()).fp);
  // WPS：廠牌、型號、型號編號與裝置名稱
  auto wps = frame(randB, "");
  const uint8_t ie[] = {221, 0, 0x00, 0x50, 0xF2, 0x04,
                        0x10, 0x21, 0x00, 0x07, 's', 'a', 'm', 's', 'u', 'n', 'g',
                        0x10, 0x23, 0x00, 0x08, 'S', 'M', '-', 'S', '9', '1', '1', 'B',
                        0x10, 0x11, 0x00, 0x05, 'A', 'l', 'i', 'c', 'e'};
  std::vector<uint8_t> w(ie, ie + sizeof(ie));
  w[1] = (uint8_t)(sizeof(ie) - 2);
  wps.insert(wps.end(), w.begin(), w.end());
  const ProbeInfo pw = describe(wps.data(), wps.size());
  CHECK(pw.model == "samsung SM-S911B" && pw.name == "Alice");
  // Apple 的廠商自訂欄位
  auto ap = frame(randC, "");
  const uint8_t aie[] = {221, 4, 0x00, 0x17, 0xF2, 0x0A};
  ap.insert(ap.end(), aie, aie + sizeof(aie));
  CHECK(describe(ap.data(), ap.size()).apple);
  // 欄位長度超出封包不越界
  auto cut2 = wps;
  cut2.resize(cut2.size() - 3);
  CHECK(describe(cut2.data(), cut2.size()).name.empty());

  std::printf("探測請求測試：%d / %d 通過\n", total - fails, total);
  return fails ? 1 : 0;
}
