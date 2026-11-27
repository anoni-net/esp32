// 會議室檢查的單元測試，編譯韌體用的同一份 meeting.h。資料都是虛構的。
#include "../../pocket_detector/meeting.h"

#include <cstdio>
#include <string>

static int fails = 0, total = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    total++;                                                         \
    if (!(cond)) {                                                   \
      fails++;                                                       \
      std::printf("失敗 %s:%d：%s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)

using namespace meeting;
using devinfo::Info;

static const uint8_t A[6] = {1, 2, 3, 4, 5, 6};
static Info kind(devinfo::Kind k, const char* label = "", uint8_t flags = 0, const char* name = "") {
  Info in;
  in.kind = k;
  in.label = label;
  in.rank = *label ? 1 : 0;
  in.flags = flags;
  in.name = name;
  return in;
}

int main() {

  Watch w;
  w.start(0, -70);
  // 建立基準：隔壁的 Apple 裝置（強）、走廊的裝置（弱，不算）
  for (uint32_t t = 0; t < BASE_MS; t += 5000) {
    w.ble(100, A, kind(devinfo::APPLE), -65, t);
    w.ble(200, A, kind(devinfo::OTHER), -85, t);
    w.tick(t);
  }
  w.tick(BASE_MS);
  CHECK(w.phase == WATCH);
  CHECK(w.baseCount(BASE_MS) == 1 && w.newCount(BASE_MS) == 0 && !w.alert(BASE_MS));

  // 會議中：基準的 Apple 裝置換了位址（舊的 40 秒前消失），不提醒
  uint32_t t = BASE_MS + 100000;
  w.ble(101, A, kind(devinfo::APPLE), -64, t);
  w.tick(t);
  CHECK(w.newCount(t) == 0 && !w.alert(t));

  // 有人沒交手機：多了一個強訊號的 Android 裝置
  w.ble(300, A, kind(devinfo::GOOGLE, "Android、Google 裝置", 0, "Pixel 的主人"), -55, t + 1000);
  w.tick(t + 1000);
  CHECK(w.newCount(t + 1000) == 1 && w.alert(t + 1000));
  std::string j = w.json(t + 1000, 7);
  CHECK(j.find("\"l\":\"Android、Google 裝置\",\"nm\":\"Pixel 的主人\"") != std::string::npos);
  CHECK(j.find("\"r\":-55") != std::string::npos && j.find("\"now\":true") != std::string::npos);
  CHECK(j.find("\"new\":1,\"lvl\":2,\"alert\":true") != std::string::npos);
  CHECK(w.maxNew == 1);

  // 它離開 90 秒以上就不算了
  w.tick(t + 1000 + PRESENT_MS + 1000);
  CHECK(w.newCount(t + 1000 + PRESENT_MS + 1000) == 0);

  // 新的 Apple 裝置，但同一類的基準裝置都還在：要提醒（不能都當成換位址）
  uint32_t t2 = t + 300000;
  w.ble(101, A, kind(devinfo::APPLE), -64, t2);
  w.ble(400, A, kind(devinfo::APPLE, "Apple 裝置", devinfo::IN_USE), -60, t2);
  w.tick(t2);
  CHECK(w.newCount(t2) == 1);
  // 正在使用（Handoff）：使用的跡象記下時間
  std::string j2 = w.json(t2 + 5000, 8);
  CHECK(j2.find("\"use\":5,") != std::string::npos);
  // 離開的裝置也留在清單（時間軸用），標成不在
  CHECK(j2.find("\"now\":false") != std::string::npos);

  // Wi-Fi：基準時沒有近距離的探測請求，會議中 2 分鐘內出現 3 個強的，提醒；弱的不算
  Watch v;
  v.start(0, -70);
  v.probe(-80, 1000);
  v.tick(BASE_MS);
  CHECK(v.baseProbes == 0);
  v.probe(-50, BASE_MS + 10000);
  v.tick(BASE_MS + 10000);
  CHECK(!v.wifiAlert(BASE_MS + 10000));  // 只有 1 個不算
  v.probe(-52, BASE_MS + 20000);
  v.probe(-90, BASE_MS + 25000);
  v.probe(-55, BASE_MS + 30000);
  v.tick(BASE_MS + 30000);
  CHECK(v.probesTotal == 3 && v.wifiAlert(BASE_MS + 30000) && v.alert(BASE_MS + 30000));
  // 3 分鐘後舊的請求過期
  v.tick(BASE_MS + 30000 + RATE_MS + 1000);
  CHECK(!v.wifiAlert(BASE_MS + 30000 + RATE_MS + 1000));

  // 基準時本來就有固定的探測請求（例如允許帶進來的筆電）：要比基準多才提醒
  Watch u;
  u.start(0, -70);
  for (uint32_t k = 0; k < 6; k++) u.probe(-60, k * 10000);  // 每分鐘 6 個
  u.tick(BASE_MS);
  for (uint32_t k = 0; k < 6; k++) u.probe(-60, BASE_MS + k * 10000);
  u.tick(BASE_MS + 60000);
  CHECK(!u.wifiAlert(BASE_MS + 60000));

  // Wi-Fi 指紋：基準時沒有的指紋出現就提醒，同一個指紋算一組，記下型號與名稱
  Watch g;
  g.start(0, -70);
  g.probeDev(0xAAAA, "", "", false, -60, 1000);  // 基準時就有（允許帶進來的筆電）
  g.tick(BASE_MS);
  g.probeDev(0xAAAA, "", "", false, -60, BASE_MS + 1000);
  g.tick(BASE_MS + 1000);
  CHECK(g.newGroups(BASE_MS + 1000) == 0 && !g.wifiAlert(BASE_MS + 1000));
  g.probeDev(0xBBBB, "samsung SM-S911B", "王小明的 S23", false, -55, BASE_MS + 2000);
  g.probeDev(0xBBBB, "", "", false, -50, BASE_MS + 3000);
  g.probeDev(0xCCCC, "", "", true, -80, BASE_MS + 3000);  // 太弱，不算
  g.tick(BASE_MS + 3000);
  CHECK(g.newGroups(BASE_MS + 3000) == 1 && g.wifiAlert(BASE_MS + 3000));
  CHECK(g.level(BASE_MS + 3000) == 1);  // 只有 Wi-Fi 的動靜：留意，不是提醒
  // 新的指紋只收到 1 次不算
  Watch once;
  once.start(0, -70);
  once.tick(BASE_MS);
  once.probeDev(0xF00F, "", "", false, -60, BASE_MS + 1000);
  once.tick(BASE_MS + 1000);
  CHECK(!once.wifiAlert(BASE_MS + 1000));
  // 2026-11-27 A 板實測：基準每分鐘 4 個，監看時一陣 9 個，不該觸發（要超過 4 × 2 + 2）
  Watch burst;
  burst.start(0, -70);
  for (uint32_t k = 0; k < 8; k++) burst.probe(-60, k * 15000);  // 基準 2 分鐘 8 個，每分鐘 4 個
  burst.tick(BASE_MS);
  for (uint32_t k = 0; k < 9; k++) burst.probe(-60, BASE_MS + k * 6000);
  burst.tick(BASE_MS + 60000);
  CHECK(!burst.wifiAlert(BASE_MS + 60000));
  const std::string gj = g.json(BASE_MS + 3000, 1);
  CHECK(gj.find("\"groups\":1") != std::string::npos);
  CHECK(gj.find("\"n\":2") != std::string::npos && gj.find("\"m\":\"samsung SM-S911B\",\"nm\":\"王小明的 S23\"") != std::string::npos);
  CHECK(gj.find("aaaa") == std::string::npos);  // 基準的不列出

  // 訊號在門檻上下徘徊的裝置：基準時 -73 dBm（比門檻弱，但在寬鬆的範圍內），監看時 -68 dBm，不算多出來的
  Watch h;
  h.start(0, -70);
  h.ble(500, A, kind(devinfo::APPLE), -73, 10000);
  h.ble(501, A, kind(devinfo::OTHER), -85, 10000);  // 太弱，基準也不記
  h.tick(BASE_MS);
  h.ble(500, A, kind(devinfo::APPLE), -68, BASE_MS + 5000);
  h.ble(502, A, kind(devinfo::GOOGLE), -75, BASE_MS + 5000);  // 監看時新出現但比門檻弱，不列
  h.tick(BASE_MS + 5000);
  CHECK(h.newCount(BASE_MS + 5000) == 0 && !h.alert(BASE_MS + 5000));
  h.ble(503, A, kind(devinfo::GOOGLE), -60, BASE_MS + 6000);  // 真的多了一個
  h.tick(BASE_MS + 6000);
  CHECK(h.newCount(BASE_MS + 6000) == 1);
  // Wi-Fi 的指紋也一樣：基準時 -75 dBm 的指紋，監看時變強不算新的
  Watch q;
  q.start(0, -70);
  q.probeDev(0xD00D, "", "", false, -75, 1000);
  q.tick(BASE_MS);
  q.probeDev(0xD00D, "", "", false, -65, BASE_MS + 1000);
  q.probeDev(0xE00E, "", "", false, -75, BASE_MS + 1000);  // 監看時新出現但比門檻弱，不算
  q.tick(BASE_MS + 1000);
  CHECK(q.newGroups(BASE_MS + 1000) == 0);

  // 換位址：舊位址消失 5 分鐘後新位址才出現，類型相同、訊號接近，仍當成同一台
  Watch r;
  r.start(0, -70);
  r.ble(600, A, kind(devinfo::APPLE, "Apple Watch", 0), -62, 10000);
  r.ble(600, A, kind(devinfo::APPLE, "Apple Watch", 0), -62, BASE_MS);
  r.tick(BASE_MS);
  r.ble(601, A, kind(devinfo::APPLE, "Apple Watch", 0), -60, BASE_MS + 300000);
  r.tick(BASE_MS + 300000);
  CHECK(r.newCount(BASE_MS + 300000) == 0);
  // 同一類但類型不同（手錶消失、出現 AirPods）或訊號差很多，不能當成換位址
  Watch r2;
  r2.start(0, -70);
  Info watch = kind(devinfo::APPLE, "Apple Watch", 0);
  watch.rank = 3;
  Info pods = kind(devinfo::APPLE, "AirPods Pro", 0);
  pods.rank = 4;
  r2.ble(700, A, watch, -62, BASE_MS - 1000);
  r2.tick(BASE_MS);
  r2.ble(701, A, pods, -61, BASE_MS + 100000);
  r2.ble(702, A, kind(devinfo::APPLE), -45, BASE_MS + 100000);
  r2.tick(BASE_MS + 100000);
  CHECK(r2.newCount(BASE_MS + 100000) == 2);

  // 名稱存成固定長度，截斷時不切壞中文
  char buf[8];
  copyText(buf, sizeof(buf), "中文名稱");  // 每個字 3 bytes，7 bytes 只放得下兩個字
  CHECK(std::string(buf) == "中文");
  copyText(buf, sizeof(buf), "abc");
  CHECK(std::string(buf) == "abc");

  // 停止
  w.stop();
  CHECK(!w.running());

  std::printf("會議室檢查測試：%d / %d 通過\n", total - fails, total);
  return fails ? 1 : 0;
}
