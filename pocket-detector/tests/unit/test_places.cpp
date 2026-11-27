// 地點指紋的單元測試，編譯韌體用的同一份 places.h。資料都是虛構的。
#include "../../pocket_detector/places.h"

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

using namespace places;

// 虛構的地點：base 開頭的 n 個基地台，每輪隨機少掉幾個、混進一兩個路過的
static std::vector<uint32_t> scanAt(uint32_t base, int n, int round) {
  std::vector<uint32_t> k;
  for (int i = 0; i < n; i++) {
    if ((i * 7 + round) % 5 == 0) continue;  // 這輪沒掃到
    k.push_back(base + i);
  }
  k.push_back(0xF0000000u + round);  // 路過的
  return k;
}

static uint16_t stay(Book& b, uint32_t base, int n, int rounds, bool* revisit = nullptr) {
  uint16_t id = b.enter(scanAt(base, n, 0), revisit);
  for (int r = 0; r < rounds; r++) b.observe(scanAt(base, n, r));
  return id;
}

int main() {
  // 雜湊與隨機位址
  const uint8_t a[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  const uint8_t r[6] = {0x12, 0x20, 0x30, 0x40, 0x50, 0x60};
  CHECK(keyOf(a, 1) != keyOf(a, 2));  // 換一塊板子，同一個基地台的雜湊不同
  CHECK(keyOf(a, 1) == keyOf(a, 1));
  CHECK(!randomBssid(a));
  CHECK(randomBssid(r));

  // 家 → 公司 → 家：回家時認得出來，沿用同一個編號
  Book b;
  bool rv = true;
  const uint16_t home = stay(b, 1000, 30, 40, &rv);
  CHECK(!rv);
  const uint16_t office = stay(b, 5000, 25, 40, &rv);
  CHECK(!rv && office != home);
  const uint16_t back = stay(b, 1000, 30, 5, &rv);
  CHECK(rv && back == home);
  CHECK(b.list.size() == 2);
  CHECK(b.list[b.find(home)].visits == 2);
  CHECK(b.list[b.find(home)].fp.size() == FP_SIZE);

  // 通勤途中：每個地方只停一兩輪，不存成指紋；只有一兩個基地台時不會被認成存過的地點
  for (int s = 0; s < 6; s++) stay(b, 20000 + s * 100, 2, 2, &rv);
  CHECK(b.list.size() == 2);
  std::vector<uint32_t> tiny = {1000, 1001};  // 恰好是家的兩個基地台
  int score = 0, shared = 0;
  CHECK(b.match(tiny, &score, &shared) < 0);

  // 只有一部分重疊的鄰近地點不算同一個
  std::vector<uint32_t> near;
  for (int i = 0; i < 6; i++) near.push_back(1000 + i);
  for (int i = 0; i < 30; i++) near.push_back(9000 + i);
  CHECK(b.match(near) < 0);

  // 命名還沒存成指紋的目前地點：馬上用到目前為止的統計存起來
  const uint16_t cafe = stay(b, 7000, 20, 3);
  CHECK(b.find(cafe) < 0);
  CHECK(b.rename(cafe, "咖啡店"));
  CHECK(b.find(cafe) >= 0 && b.list[b.find(cafe)].name == "咖啡店");
  CHECK(!b.list[b.find(cafe)].fp.empty());
  CHECK(!b.rename(cafe, std::string(NAME_BYTES + 1, 'x')));
  CHECK(b.rename(home, "家"));
  CHECK(std::string(b.nameOf(home)) == "家");
  CHECK(std::string(b.nameOf(999)).empty());

  // 存檔再讀回來：編號、名稱、指紋都一樣，新的編號不會跟舊的重複
  b.salt = 0xdeadbeef;
  const std::string saved = b.serialize();
  Book c;
  CHECK(c.parse(saved));
  CHECK(c.salt == 0xdeadbeef);
  CHECK(c.list.size() == b.list.size());
  CHECK(std::string(c.nameOf(home)) == "家" && std::string(c.nameOf(cafe)) == "咖啡店");
  CHECK(c.list[c.find(home)].fp.size() == b.list[b.find(home)].fp.size());
  CHECK(c.list[c.find(home)].fp[0].key == b.list[b.find(home)].fp[0].key);
  bool rv2 = false;
  const uint16_t again = stay(c, 1000, 30, 3, &rv2);
  CHECK(rv2 && again == home);  // 重新開機後也認得家
  const uint16_t fresh = stay(c, 30000, 20, 3, &rv2);
  CHECK(!rv2 && c.find(fresh) < 0 && fresh != home && fresh != office && fresh != cafe);
  CHECK(!c.parse("garbage"));

  // 刪除
  CHECK(c.forget(office));
  CHECK(c.find(office) < 0);
  CHECK(!c.forget(office));
  // 刪除目前所在的地點：改用新的編號，繼續停留也不會用原本的編號存回來
  Book e;
  const uint16_t here = stay(e, 40000, 20, 25);
  CHECK(e.find(here) >= 0);
  CHECK(e.forget(here));
  CHECK(e.curId != here && e.cur < 0);
  for (int r = 0; r < 25; r++) e.observe(scanAt(40000, 20, r));
  CHECK(e.find(here) < 0 && e.find(e.curId) >= 0);

  // 滿了：移除最久沒來、沒有名稱的地點，有名稱的留著
  Book d;
  const uint16_t first = stay(d, 100000, 20, 12);
  d.rename(first, "第一個");
  const uint16_t second = stay(d, 110000, 20, 12);
  for (int i = 0; i < MAX_PLACES + 3; i++) stay(d, 200000 + i * 1000, 20, 12);
  stay(d, 900000, 20, 1);  // 收尾最後一個
  CHECK(d.list.size() == MAX_PLACES);
  CHECK(d.find(first) >= 0);
  CHECK(d.find(second) < 0);

  // 恢復原廠設定：警示歷史裡的地點名稱清掉，其他內容不動
  const std::string hist =
      "[{\"trk\":[{\"pl\":[3,9]}],\"places\":{\"3\":\"家\",\"9\":\"有 } 與 \\\" 的名稱\"},\"marks\":[{\"label\":\"places\"}]},"
      "{\"trk\":[],\"places\":{},\"marks\":[]}]";
  const std::string clean = stripNames(hist);
  CHECK(clean ==
        "[{\"trk\":[{\"pl\":[3,9]}],\"places\":{},\"marks\":[{\"label\":\"places\"}]},"
        "{\"trk\":[],\"places\":{},\"marks\":[]}]");
  CHECK(clean.find("家") == std::string::npos);
  CHECK(stripNames("{\"trk\":[]}") == "{\"trk\":[]}");  // 舊版沒有 places 的紀錄
  CHECK(stripNames("") == "");

  std::printf("地點測試：%d / %d 通過\n", total - fails, total);
  return fails ? 1 : 0;
}
