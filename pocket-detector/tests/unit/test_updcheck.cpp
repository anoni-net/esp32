// 網址更新判斷的單元測試，編譯韌體用的同一份 updcheck.h。執行 tests/unit/run.sh。
#include "../../../lib/netupdate/updcheck.h"

#include <cstdio>
#include <string>

static int fails = 0, total = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    total++;                                                           \
    if (!(cond)) {                                                     \
      fails++;                                                         \
      std::printf("失敗 %s:%d：%s\n", __FILE__, __LINE__, #cond);   \
    }                                                                  \
  } while (0)

using namespace updcheck;

static const std::string SHA(64, 'a');
static std::string manifest(const std::string& build, const std::string& file = "tracker-0123456789ab.bin",
                            const std::string& sha = SHA, const std::string& size = "1626896") {
  return "{\"build\": \"" + build + "\", \"file\": \"" + file + "\", \"size\": " + size + ", \"sha256\": \"" + sha +
         "\", \"commit\": \"abc1234\"}";
}

static Verdict v(const std::string& j, const char* cur = "Nov 27 2026 10:00:00", const char* skip = "") {
  Manifest m;
  return decide(j, cur, skip, 24, m);
}

int main() {
  // 建置時間
  CHECK(buildKey("Nov 27 2026 10:51:14") == 20261127105114ULL);
  CHECK(buildKey("Dec  3 2026 01:02:03") == 20261203010203ULL);  // 日期小於 10 時前面補空白
  CHECK(buildKey("Jan  1 2027 00:00:00") > buildKey("Dec 31 2026 23:59:59"));
  CHECK(buildKey("Nov 27 2026 10:00:01") > buildKey("Nov 27 2026 10:00:00"));
  CHECK(buildKey("Foo 27 2026 10:00:00") == 0);
  CHECK(buildKey("anF 27 2026 10:00:00") == 0);  // 月份字串要對齊三個字
  CHECK(buildKey("Nov 27 2026") == 0);
  CHECK(buildKey("") == 0);

  // 欄位
  const std::string j = manifest("Nov 27 2026 10:51:14");
  CHECK(field(j, "build") == "Nov 27 2026 10:51:14");
  CHECK(field(j, "size") == "1626896");
  CHECK(field(j, "commit") == "abc1234");
  CHECK(field(j, "missing").empty());
  CHECK(field("{\"build\":\"x", "build").empty());  // 字串沒有結尾

  // 檔名與雜湊
  CHECK(safeName("tracker-0123456789ab.bin"));
  CHECK(!safeName("../tracker.bin"));
  CHECK(!safeName("tracker.bin?x=1"));
  CHECK(!safeName("tracker.txt"));
  CHECK(!safeName(".bin" + std::string(70, 'a')));
  CHECK(!safeName(""));
  CHECK(isHex64(SHA));
  CHECK(!isHex64(std::string(63, 'a')));
  CHECK(!isHex64(std::string(63, 'a') + "g"));

  // 判斷
  CHECK(v(manifest("Nov 27 2026 10:51:14")) == NEWER);
  CHECK(v(manifest("Nov 27 2026 10:00:00")) == SAME);
  // 2026-11-26：B 板從 USB 燒進較新的版本後，按更新被換回舊版
  CHECK(v(manifest("Nov 26 2026 09:00:00")) == OLDER);
  CHECK(v(manifest("Nov 27 2026 10:51:14"), "Nov 27 2026 10:00:00", "Nov 27 2026 10:51:14") == SKIPPED);
  CHECK(v(manifest("Nov 27 2026 10:51:14"), "Nov 27 2026 10:00:00", "Nov 20 2026 10:51:14") == NEWER);
  CHECK(v(manifest("Nov 27 2026 10:51:14"), "dev build") == BADTIME);
  CHECK(v(manifest("Nov 27 2026 10:51:14", "../evil.bin")) == BAD);
  CHECK(v(manifest("Nov 27 2026 10:51:14", "tracker-0123456789ab.bin", "xyz")) == BAD);
  CHECK(v(manifest("Nov 27 2026 10:51:14", "tracker-0123456789ab.bin", SHA, "99999")) == BAD);
  CHECK(v(manifest("Nov 27 2026 10:51:14", "tracker-0123456789ab.bin", SHA, "4000000")) == BAD);
  CHECK(v(manifest(std::string(30, 'x'))) == BAD);  // 放不進韌體的緩衝區
  CHECK(v("<html>Cloudflare</html>") == BAD);
  Manifest m;
  decide(j, "Nov 27 2026 10:00:00", "", 24, m);
  CHECK(m.size == 1626896 && m.file == "tracker-0123456789ab.bin" && m.commit == "abc1234");

  std::printf("單元測試：%d / %d 通過\n", total - fails, total);
  return fails ? 1 : 0;
}
