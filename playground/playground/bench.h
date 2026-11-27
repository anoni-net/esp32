// 運算效能測試：向量指令（PIE）與一般 C 的對照、整數、單精度與雙精度浮點、硬體加速的 SHA-256 與 AES、
// 內部 SRAM 與 PSRAM 的搬移速度、雙核心同時運算
#pragma once
#include <Arduino.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "mbedtls/aes.h"
#include "dspm_mult.h"
#include "dsps_fft2r.h"

namespace bench {

static volatile uint32_t sink;

static inline int64_t nowUs() { return esp_timer_get_time(); }

// 線性同餘加上 XOR 與加法，結果互相依賴，編譯器無法省略
__attribute__((noinline)) static uint32_t intLoop(uint32_t n) {
  uint32_t a = 1, b = 2, c = 3;
  for (uint32_t i = 0; i < n; i++) {
    a = a * 1664525u + 1013904223u;
    b ^= a >> 3;
    c += b;
  }
  return a ^ b ^ c;
}

template <typename T>
__attribute__((noinline)) static T matmul(const T* A, const T* B, T* C, int n) {
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++) {
      T s = 0;
      for (int k = 0; k < n; k++) s += A[i * n + k] * B[k * n + j];
      C[i * n + j] = s;
    }
  return C[n + 1];
}

// 回傳 MFLOPS，矩陣放在內部 SRAM
template <typename T>
static double matmulMflops(int n, int rounds) {
  const size_t bytes = sizeof(T) * n * n;
  T* A = (T*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  T* B = (T*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  T* C = (T*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  double r = -1;
  if (A && B && C) {
    for (int i = 0; i < n * n; i++) { A[i] = (T)((i * 7 % 13) - 6) / 3; B[i] = (T)((i * 5 % 11) - 5) / 7; }
    const int64_t t = nowUs();
    for (int k = 0; k < rounds; k++) sink ^= (uint32_t)matmul<T>(A, B, C, n);
    const int64_t us = nowUs() - t;
    r = 2.0 * n * n * n * rounds / us;
  }
  free(A); free(B); free(C);
  return r;
}

// 回傳 MB/s
static double memcpyMBps(uint32_t caps, size_t size, int rounds) {
  uint8_t* a = (uint8_t*)heap_caps_malloc(size, caps);
  uint8_t* b = (uint8_t*)heap_caps_malloc(size, caps);
  double r = -1;
  if (a && b) {
    memset(a, 0x5a, size);
    const int64_t t = nowUs();
    for (int k = 0; k < rounds; k++) { memcpy(b, a, size); a[k % size] ^= b[(k * 31) % size]; }
    const int64_t us = nowUs() - t;
    r = (double)size * rounds / us;
  }
  free(a); free(b);
  return r;
}

struct DualArg {
  uint32_t n;
  SemaphoreHandle_t done;
};

static void dualTask(void* p) {
  DualArg* a = (DualArg*)p;
  sink ^= intLoop(a->n);
  xSemaphoreGive(a->done);
  vTaskDelete(NULL);
}

// 同一個運算各跑一般 C（ansi）與 S3 向量指令最佳化（aes3）的版本，回傳兩者的微秒數
struct Pair { double ansiUs, simdUs; };

static Pair matS16(int n, int rounds) {
  const size_t bytes = sizeof(int16_t) * n * n;
  int16_t* A = (int16_t*)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t* B = (int16_t*)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t* C = (int16_t*)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Pair r = {-1, -1};
  if (A && B && C) {
    for (int i = 0; i < n * n; i++) { A[i] = (int16_t)((i * 37 % 201) - 100); B[i] = (int16_t)((i * 53 % 151) - 75); }
    int64_t t = nowUs();
    for (int k = 0; k < rounds; k++) dspm_mult_s16_ansi(A, B, C, n, n, n, 0);
    r.ansiUs = (double)(nowUs() - t) / rounds;
    sink ^= C[3];
    vTaskDelay(1);
    t = nowUs();
    for (int k = 0; k < rounds; k++) dspm_mult_s16_aes3(A, B, C, n, n, n, 0);
    r.simdUs = (double)(nowUs() - t) / rounds;
    sink ^= C[5];
  }
  heap_caps_free(A); heap_caps_free(B); heap_caps_free(C);
  return r;
}

static Pair fftF32(int n, int rounds) {
  Pair r = {-1, -1};
  if (dsps_fft2r_init_fc32(NULL, n) != ESP_OK) return r;
  float* d = (float*)heap_caps_aligned_alloc(16, sizeof(float) * 2 * n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (d) {
    for (int i = 0; i < 2 * n; i++) d[i] = (float)((i * 13 % 29) - 14) / 7;
    int64_t t = nowUs();
    for (int k = 0; k < rounds; k++) dsps_fft2r_fc32_ansi(d, n);
    r.ansiUs = (double)(nowUs() - t) / rounds;
    vTaskDelay(1);
    t = nowUs();
    for (int k = 0; k < rounds; k++) dsps_fft2r_fc32_aes3(d, n);
    r.simdUs = (double)(nowUs() - t) / rounds;
    sink ^= (uint32_t)d[1];
  }
  heap_caps_free(d);
  dsps_fft2r_deinit_fc32();
  return r;
}

static String num(double v, int digits = 1) { return String(v, digits); }

// 全部跑完約 5 到 10 秒，回傳 JSON
static String run() {
  String j = "{";
  j += "\"mhz\":" + String(getCpuFrequencyMhz());

  // 整數
  const uint32_t N = 20000000;
  int64_t t = nowUs();
  sink ^= intLoop(N);
  int64_t us = nowUs() - t;
  j += ",\"intMps\":" + num(N / (double)us);  // 每秒百萬次迴圈
  j += ",\"intNs\":" + num(us * 1000.0 / N, 2);
  vTaskDelay(1);

  // 浮點矩陣乘法
  j += ",\"f32\":" + num(matmulMflops<float>(64, 40));
  vTaskDelay(1);
  j += ",\"f64\":" + num(matmulMflops<double>(48, 10));
  vTaskDelay(1);

  // SHA-256（硬體加速）
  const size_t BUF = 64 * 1024;
  uint8_t* in = (uint8_t*)heap_caps_malloc(BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint8_t* out = (uint8_t*)heap_caps_malloc(BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (in && out) {
    for (size_t i = 0; i < BUF; i++) in[i] = (uint8_t)(i * 131 + 17);
    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    t = nowUs();
    mbedtls_sha256_starts(&sha, 0);
    for (int k = 0; k < 32; k++) mbedtls_sha256_update(&sha, in, BUF);
    mbedtls_sha256_finish(&sha, digest);
    us = nowUs() - t;
    mbedtls_sha256_free(&sha);
    sink ^= digest[0];
    j += ",\"sha\":" + num(BUF * 32.0 / us);
    vTaskDelay(1);

    // AES-128-CTR（硬體加速）
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    mbedtls_aes_setkey_enc(&aes, key, 128);
    uint8_t nonce[16] = {0}, stream[16];
    size_t off = 0;
    t = nowUs();
    for (int k = 0; k < 32; k++) mbedtls_aes_crypt_ctr(&aes, BUF, &off, nonce, stream, in, out);
    us = nowUs() - t;
    mbedtls_aes_free(&aes);
    sink ^= out[7];
    j += ",\"aes\":" + num(BUF * 32.0 / us);
  }
  free(in); free(out);
  vTaskDelay(1);

  // 記憶體搬移
  j += ",\"sram\":" + num(memcpyMBps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 32 * 1024, 256));
  vTaskDelay(1);
  j += ",\"psram\":" + num(memcpyMBps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 512 * 1024, 16));
  vTaskDelay(1);

  // 雙核心：先單核跑一次，再兩個核心同時各跑一份
  const uint32_t N2 = 10000000;
  t = nowUs();
  sink ^= intLoop(N2);
  const int64_t single = nowUs() - t;
  DualArg a0 = {N2, xSemaphoreCreateBinary()}, a1 = {N2, xSemaphoreCreateBinary()};
  t = nowUs();
  xTaskCreatePinnedToCore(dualTask, "bench0", 4096, &a0, 2, nullptr, 0);
  xTaskCreatePinnedToCore(dualTask, "bench1", 4096, &a1, 2, nullptr, 1);
  xSemaphoreTake(a0.done, portMAX_DELAY);
  xSemaphoreTake(a1.done, portMAX_DELAY);
  const int64_t dual = nowUs() - t;
  vSemaphoreDelete(a0.done);
  vSemaphoreDelete(a1.done);
  j += ",\"dual\":" + num(2.0 * single / dual, 2);
  vTaskDelay(1);

  // 向量指令對照
  const Pair m = matS16(64, 10);
  j += ",\"mAnsi\":" + num(m.ansiUs / 1000, 2) + ",\"mSimd\":" + num(m.simdUs / 1000, 2);
  const Pair f = fftF32(1024, 20);
  j += ",\"fAnsi\":" + num(f.ansiUs / 1000, 2) + ",\"fSimd\":" + num(f.simdUs / 1000, 2);

  j += "}";
  return j;
}

}  // namespace bench
