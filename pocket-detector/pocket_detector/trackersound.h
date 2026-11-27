// 讓追蹤器發出聲音：連上追蹤器後，依序嘗試它提供的發聲協定。
//
// 除了 DULT，Apple 與 Pebblebee 各有自己的協定，參考 AirGuard（seemoo-lab/AirGuard 的
// AppleFindMy.kt、AirTag.kt、AirPods.kt、PebbleBee.kt）：
//   - Apple Find My（FD44）：服務 0xFD44，特徵值 4F860003-943B-49EF-BED4-2F730304427A，
//     寫入 01 00 03 開始、01 01 03 停止
//   - AirTag 原始協定：服務 7DFC9000-7D1C-4951-86AA-8D9728F8D66C，特徵值 7DFC9001-…，
//     寫入 0xAF 開始，響完追蹤器會自己斷線
//   - Pebblebee：服務 0xFA25，特徵值 0x2C02，寫入 01 開始（不等回應）、02 停止
// AirGuard 對 AirTag 先試原始協定，對 AirPods 先試 FD44，其他 Apple 裝置先試 DULT。
//
// DULT 的部分
// 依據 draft-ietf-dult-accessory-protocol-01（2026-10-27）的 3.11 與 3.13.4 節：
//   - 服務 15190001-12F4-C226-88ED-2AC5579F2A85，特徵值 8E0C0001-1D68-FB92-BF61-48377421680E
//   - 指令是 2 bytes 的 opcode（little-endian），後面接參數
//   - Sound_Start 0x0300、Sound_Stop 0x0301，追蹤器用 indication 回覆
//     Command_Response 0x0302（參數：對應的 opcode、狀態碼）與 Sound_Completed 0x0303
//   - 只有與主人分開的追蹤器接受，不需要配對，聲音持續 5 到 30 秒（建議 12 秒）
#pragma once
#include "i18n.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

namespace trackersound {

static const NimBLEUUID SVC_UUID("15190001-12F4-C226-88ED-2AC5579F2A85");
static const NimBLEUUID CHR_UUID("8E0C0001-1D68-FB92-BF61-48377421680E");
static const uint16_t OP_SOUND_START = 0x0300;
static const uint16_t OP_SOUND_STOP = 0x0301;
static const uint16_t OP_COMMAND_RESPONSE = 0x0302;
static const uint16_t OP_SOUND_COMPLETED = 0x0303;

enum Result : uint8_t {
  OK,             // 追蹤器回覆成功，正在發出聲音
  NOT_FOUND,      // 短暫掃描沒有找到這個位址
  CONNECT_FAIL,   // 連不上
  NO_SERVICE,     // 沒有 DULT 的非擁有者服務，這個追蹤器不支援
  WRITE_FAIL,     // 寫入指令失敗
  NO_RESPONSE,    // 寫入成功但沒有回覆
  REFUSED,        // 回覆了失敗的狀態碼
  NOT_CONNECTABLE,  // 收到的廣播不接受連線
  SKIPPED,        // 自動發聲跳過：沒收過 DULT 廣播，不支援 DULT 發聲
};

static int lastError = 0;  // 連線失敗時 NimBLE 回傳的錯誤碼

static volatile bool gotResponse = false, gotCompleted = false;
static volatile uint16_t responseOp = 0, responseStatus = 0;

static void onIndicate(NimBLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
  if (n < 2) return;
  const uint16_t op = d[0] | (d[1] << 8);
  if (op == OP_COMMAND_RESPONSE && n >= 6) {
    responseOp = d[2] | (d[3] << 8);
    responseStatus = d[4] | (d[5] << 8);
    gotResponse = true;
  } else if (op == OP_SOUND_COMPLETED) {
    gotCompleted = true;
  }
}

// 回覆的狀態碼轉成說明
static const char* statusText(uint16_t s) {
  switch (s) {
    case 0x0000: return TR("成功");
    case 0x0001: return TR("狀態不符（可能已經在發聲，或聲音不是這塊偵測器觸發的）");
    case 0x0002: return TR("設定不符");
    case 0x0003: return TR("長度不符");
    case 0x0004: return TR("參數不符");
    case 0xFFFF: return TR("不接受這個指令（可能主人就在附近，或不支援發聲）");
    default: return TR("保留的狀態碼");
  }
}

static const char* lastProtocol = "";  // 最近一次用的協定

static const NimBLEUUID FINDMY_SVC((uint16_t)0xFD44);
static const NimBLEUUID FINDMY_CHR("4F860003-943B-49EF-BED4-2F730304427A");
static const NimBLEUUID AIRTAG_SVC("7DFC9000-7D1C-4951-86AA-8D9728F8D66C");
static const NimBLEUUID AIRTAG_CHR("7DFC9001-7D1C-4951-86AA-8D9728F8D66C");
static const NimBLEUUID PEBBLEBEE_SVC((uint16_t)0xFA25);
static const NimBLEUUID PEBBLEBEE_CHR((uint16_t)0x2C02);

// 發聲協定的嘗試順序依追蹤器類型而定
enum Hint : uint8_t { HINT_GENERAL, HINT_AIRTAG, HINT_AIRPODS };
enum Proto : uint8_t { P_DULT, P_FINDMY, P_AIRTAG, P_PEBBLEBEE };

static NimBLERemoteCharacteristic* findChr(NimBLEClient* c, const NimBLEUUID& svc, const NimBLEUUID& chr) {
  NimBLERemoteService* s = c->getService(svc);
  return s ? s->getCharacteristic(chr) : nullptr;
}

// 讓追蹤器響一段時間，期間對方斷線就提早結束
static void holdWhileConnected(NimBLEClient* c, uint32_t ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms && c->isConnected()) delay(50);
}

// DULT：等 Command_Response，成功後保持連線到 Sound_Completed
static Result ringDult(NimBLEClient* c, NimBLERemoteCharacteristic* chr, uint32_t holdMs, uint16_t& status) {
  gotResponse = gotCompleted = false;
  chr->subscribe(false, onIndicate, true);  // false：訂閱 indication
  const uint8_t cmd[2] = {(uint8_t)(OP_SOUND_START & 0xFF), (uint8_t)(OP_SOUND_START >> 8)};
  if (!chr->writeValue(cmd, sizeof(cmd), true)) return WRITE_FAIL;
  const uint32_t t0 = millis();
  while (!gotResponse && millis() - t0 < 3000) delay(20);
  if (!gotResponse) return NO_RESPONSE;
  status = responseStatus;
  if (responseStatus != 0x0000) return REFUSED;
  const uint32_t t1 = millis();
  while (!gotCompleted && millis() - t1 < holdMs && c->isConnected()) delay(50);
  return OK;
}

static Result ringFindMy(NimBLEClient* c, NimBLERemoteCharacteristic* chr, uint32_t holdMs) {
  chr->subscribe(!chr->canIndicate(), nullptr, true);  // 有 indication 就訂 indication
  const uint8_t start[3] = {0x01, 0x00, 0x03}, stop[3] = {0x01, 0x01, 0x03};
  if (!chr->writeValue(start, sizeof(start), true)) return WRITE_FAIL;
  holdWhileConnected(c, holdMs);
  if (c->isConnected()) chr->writeValue(stop, sizeof(stop), true);
  return OK;
}

static Result ringAirTag(NimBLEClient* c, NimBLERemoteCharacteristic* chr, uint32_t holdMs) {
  const uint8_t start = 0xAF;
  if (!chr->writeValue(&start, 1, true)) return WRITE_FAIL;
  holdWhileConnected(c, holdMs);  // 響完 AirTag 會自己斷線
  return OK;
}

static Result ringPebblebee(NimBLEClient* c, NimBLERemoteCharacteristic* chr, uint32_t holdMs) {
  chr->subscribe(true, nullptr, true);
  const uint8_t start = 0x01, stop = 0x02;
  if (!chr->writeValue(&start, 1, false)) return WRITE_FAIL;
  holdWhileConnected(c, holdMs);
  if (c->isConnected()) chr->writeValue(&stop, 1, true);
  return OK;
}

// 找到追蹤器、連線，依序嘗試它提供的發聲協定。成功時保持連線約 holdMs，
// 斷線後有些追蹤器會停止發聲。會阻塞最多約 findMs + 6 + 3 + holdMs/1000 秒
static Result ring(NimBLEScan* scan, const std::string& addr, uint32_t findMs, uint32_t holdMs,
                   uint16_t& status, Hint hint = HINT_GENERAL) {
  status = 0;
  lastError = 0;
  lastProtocol = "";
  NimBLEScanResults results = scan->getResults(findMs, false);
  const NimBLEAdvertisedDevice* dev = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    if (results.getDevice(i)->getAddress().toString() == addr) dev = results.getDevice(i);
  }
  if (!dev) {
    scan->clearResults();
    return NOT_FOUND;
  }
  // 廣播本身不接受連線時立刻回報，不必白等連線逾時
  if (!dev->isConnectable()) {
    scan->clearResults();
    return NOT_CONNECTABLE;
  }

  NimBLEClient* c = NimBLEDevice::createClient();
  c->setConnectTimeout(6000);
  const bool connected = c->connect(dev, true);
  scan->clearResults();  // dev 指向掃描結果，連線後才清除
  if (!connected) {
    lastError = c->getLastError();
    NimBLEDevice::deleteClient(c);
    return CONNECT_FAIL;
  }

  // 第一個找得到服務與特徵值的協定就用它
  static const Proto ORDER_GENERAL[] = {P_DULT, P_AIRTAG, P_FINDMY, P_PEBBLEBEE};
  static const Proto ORDER_AIRTAG[] = {P_AIRTAG, P_DULT, P_FINDMY, P_PEBBLEBEE};
  static const Proto ORDER_AIRPODS[] = {P_FINDMY, P_DULT, P_AIRTAG, P_PEBBLEBEE};
  const Proto* order = hint == HINT_AIRTAG ? ORDER_AIRTAG : hint == HINT_AIRPODS ? ORDER_AIRPODS : ORDER_GENERAL;
  Result r = NO_SERVICE;
  for (int i = 0; i < 4 && r == NO_SERVICE && c->isConnected(); i++) {
    NimBLERemoteCharacteristic* chr = nullptr;
    switch (order[i]) {
      case P_DULT:
        if ((chr = findChr(c, SVC_UUID, CHR_UUID))) { lastProtocol = "DULT"; r = ringDult(c, chr, holdMs, status); }
        break;
      case P_FINDMY:
        if ((chr = findChr(c, FINDMY_SVC, FINDMY_CHR))) { lastProtocol = "FindMy"; r = ringFindMy(c, chr, holdMs); }
        break;
      case P_AIRTAG:
        if ((chr = findChr(c, AIRTAG_SVC, AIRTAG_CHR))) { lastProtocol = "AirTag"; r = ringAirTag(c, chr, holdMs); }
        break;
      case P_PEBBLEBEE:
        if ((chr = findChr(c, PEBBLEBEE_SVC, PEBBLEBEE_CHR))) { lastProtocol = "Pebblebee"; r = ringPebblebee(c, chr, holdMs); }
        break;
    }
  }
  if (c->isConnected()) c->disconnect();
  NimBLEDevice::deleteClient(c);
  return r;
}

}  // namespace trackersound
