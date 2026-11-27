// 韌體的語系。原文是正體中文，會顯示在面板或藍牙文字通道的字串用 TR("…") 包起來，
// 建置簡體中文或英文時在編譯期換成 i18n_zh_cn.h、i18n_en.h 裡的翻譯（由 tools/make_i18n.py 從
// i18n/firmware.toml 產生）。表裡沒有的字串編譯會失敗，不會默默留下原文。
//
// 語系由建置旗標決定（platformio.ini 的各個環境）：
//   沒有旗標          正體中文（zh-TW），Arduino IDE 直接編譯也是這個
//   -DFW_LANG_ZH_CN   簡體中文（zh-CN）
//   -DFW_LANG_EN      英文（en）
//
// 序列埠的輸出只給開發者看，實機測試也比對它的文字，維持正體中文，不包 TR。
#pragma once

#if defined(FW_LANG_ZH_CN)
#define FW_LANG "zh-CN"
#include "i18n_zh_cn.h"
#elif defined(FW_LANG_EN)
#define FW_LANG "en"
#include "i18n_en.h"
#else
#define FW_LANG "zh-TW"
#endif

#if defined(FW_LANG_ZH_CN) || defined(FW_LANG_EN)
namespace i18n {
constexpr bool eq(const char* a, const char* b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
constexpr int find(const char* s) {
  for (int i = 0; i < N; i++) {
    if (eq(KEYS[i], s)) return i;
  }
  return -1;
}
template <int I>
struct At {
  static_assert(I >= 0, "TR()：i18n/firmware.toml 沒有這個字串，執行 tools/make_i18n.py --extract 後補上翻譯");
  static constexpr int value = I;
};
}  // namespace i18n
#define TR(s) (::i18n::VALS[::i18n::At<::i18n::find(s)>::value])
#else
#define TR(s) (s)
#endif
