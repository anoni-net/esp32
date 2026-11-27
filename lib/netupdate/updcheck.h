// 網址更新的判斷：讀 latest.json、檢查格式、決定要不要下載。
//
// 不依賴 Arduino，韌體（netupdate.h）與電腦上的單元測試（tests/unit/）編譯的是同一份。
// 改這裡之後執行 tests/unit/run.sh。
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace updcheck {

// latest.json 很小，格式也固定，取欄位不必另外裝 JSON 函式庫。字串欄位取到下一個引號，
// 數字欄位取連續的數字，找不到時傳回空字串
static std::string field(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  size_t i = j.find(k);
  if (i == std::string::npos) return "";
  i = j.find(':', i + k.size());
  if (i == std::string::npos) return "";
  i++;
  while (i < j.size() && j[i] == ' ') i++;
  if (i < j.size() && j[i] == '"') {
    const size_t e = j.find('"', i + 1);
    return e == std::string::npos ? "" : j.substr(i + 1, e - i - 1);
  }
  size_t e = i;
  while (e < j.size() && isdigit((unsigned char)j[e])) e++;
  return j.substr(i, e - i);
}

// 韌體的檔名只能有英數字與 . - _，以 .bin 結尾，不能帶路徑
static bool safeName(const std::string& f) {
  if (f.empty() || f.size() > 64) return false;
  for (char c : f) {
    if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') return false;
  }
  return f.size() >= 4 && f.compare(f.size() - 4, 4, ".bin") == 0;
}

static bool isHex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!isxdigit((unsigned char)c)) return false;
  }
  return true;
}

// 建置時間（__DATE__ " " __TIME__，例如 "Nov 26 2026 10:12:03"，日期小於 10 時前面補空白）
// 換成可以比大小的數字 yyyymmddhhmmss，格式不對時傳回 0
static uint64_t buildKey(const char* b) {
  static const char* MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = "";
  int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
  if (sscanf(b, "%3s %d %d %d:%d:%d", mon, &d, &y, &hh, &mm, &ss) != 6) return 0;
  const char* m = strstr(MON, mon);
  if (!m || strlen(mon) != 3 || (m - MON) % 3) return 0;
  const int month = (m - MON) / 3 + 1;
  return ((((uint64_t)y * 100 + month) * 100 + d) * 100 + hh) * 10000ULL + mm * 100 + ss;
}

static const uint32_t MIN_SIZE = 100000;
static const uint32_t MAX_SIZE = 3UL * 1024 * 1024;

struct Manifest {
  std::string build, file, sha, commit;
  uint32_t size = 0;
};

enum Verdict : uint8_t {
  BAD,      // 格式不對，不更新
  SAME,     // 跟目前的建置時間相同
  SKIPPED,  // 自動安裝後沒有正常開機的版本
  BADTIME,  // 建置時間讀不懂
  OLDER,    // 比目前的舊，不換回去
  NEWER,    // 下載
};

// maxBuild：存建置時間的緩衝區大小。skip 可以是空字串
static Verdict decide(const std::string& j, const char* current, const char* skip, size_t maxBuild, Manifest& m) {
  m.build = field(j, "build");
  m.file = field(j, "file");
  m.sha = field(j, "sha256");
  m.commit = field(j, "commit");
  m.size = (uint32_t)strtoul(field(j, "size").c_str(), nullptr, 10);
  if (m.build.empty() || m.build.size() >= maxBuild || !safeName(m.file) || !isHex64(m.sha) || m.size < MIN_SIZE ||
      m.size > MAX_SIZE) {
    return BAD;
  }
  if (m.build == current) return SAME;
  if (skip && *skip && m.build == skip) return SKIPPED;
  // 只往新的版本更新。用 USB 燒進比較新的版本之後，assets 上的反而是舊版，不能換回去
  const uint64_t remoteKey = buildKey(m.build.c_str()), localKey = buildKey(current);
  if (!remoteKey || !localKey) return BADTIME;
  if (remoteKey < localKey) return OLDER;
  return NEWER;
}

}  // namespace updcheck
