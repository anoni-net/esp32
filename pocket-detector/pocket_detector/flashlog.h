// 序列埠輸出的同時，把同一份內容暫存起來，每次掃描完寫進 flash。
// 寫在獨立的標頭檔，原因同 types.h

#pragma once

#include <Arduino.h>

#include <string>

class Tee : public Print {
 public:
  void captureToBuffer(bool on) { capture_ = on; }

  size_t write(uint8_t c) override {
    Serial.write(c);
    if (capture_) buf_.push_back((char)c);
    return 1;
  }

  size_t write(const uint8_t* b, size_t n) override {
    Serial.write(b, n);
    if (capture_) buf_.append((const char*)b, n);
    return n;
  }

  std::string& buffer() { return buf_; }

 private:
  bool capture_ = false;
  std::string buf_;
};
