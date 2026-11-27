// 手機用藍牙連線的文字通道，採用 Nordic UART Service（NUS）。
// 手機寫入 RX 特徵值送出指令，板子用 TX 特徵值的 notify 回傳文字。
// 很多藍牙終端機 App 都支援這個服務，不必另外寫 App。
// 連線要先配對：手機跳出配對視窗時輸入 6 位數的配對碼（顯示在控制頁與序列埠），
// 連線加密並通過驗證之前不接受指令，也不送出任何文字。配網會傳送 Wi-Fi 密碼，所以一定要加密。
#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

namespace bleuart {

static const char* SVC_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // 手機 → 板子
static const char* TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // 板子 → 手機

struct Msg {
  char s[160];
};

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* tx = nullptr;
static QueueHandle_t inbox = nullptr;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool connected = false;
static volatile bool secure = false;
static uint32_t passkey = 0;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    conn = info.getConnHandle();
    connected = true;
    secure = info.isEncrypted() && info.isAuthenticated();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    connected = false;
    secure = false;
    conn = BLE_HS_CONN_HANDLE_NONE;
  }
  uint32_t onPassKeyDisplay() override { return passkey; }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    secure = info.isEncrypted() && info.isAuthenticated();
    if (!secure) server->disconnect(info.getConnHandle());
  }
};

// 寫入在藍牙的工作執行，這裡只把文字丟進佇列，指令在 loop() 處理
class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    if (!secure) return;
    const std::string v = c->getValue();
    Msg m;
    const size_t n = v.size() < sizeof(m.s) - 1 ? v.size() : sizeof(m.s) - 1;
    memcpy(m.s, v.data(), n);
    m.s[n] = 0;
    xQueueSend(inbox, &m, 0);
  }
};

// 呼叫前要先 NimBLEDevice::init()
static void begin(const String& name, uint32_t pin) {
  passkey = pin;
  inbox = xQueueCreate(8, sizeof(Msg));
  NimBLEDevice::setDeviceName(name.c_str());
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);  // 配對並記住、輸入配對碼、LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(pin);
  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  server->advertiseOnDisconnect(true);
  NimBLEService* svc = server->createService(SVC_UUID);
  tx = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                              NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
  NimBLECharacteristic* rx = svc->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  rx->setCallbacks(new RxCB());
  svc->start();

  // 128 位元的 UUID 加上名稱超過 31 bytes，名稱放在掃描回應
  NimBLEAdvertisementData ad, sr;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.addServiceUUID(SVC_UUID);
  sr.setName(name.c_str());
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);
  adv->enableScanResponse(true);
  adv->start();
}

// 依連線協商出的 MTU 切段送出
static void send(const String& text) {
  if (!connected || !secure || !tx) return;
  uint16_t mtu = server->getPeerMTU(conn);
  size_t chunk = mtu > 23 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  const char* p = text.c_str();
  size_t left = text.length();
  while (left > 0 && connected) {
    const size_t n = left < chunk ? left : chunk;
    tx->setValue((const uint8_t*)p, n);
    tx->notify();
    p += n;
    left -= n;
    delay(8);
  }
}

// 取出一筆收到的指令，沒有就回傳 false
static bool receive(String& out) {
  Msg m;
  if (!inbox || xQueueReceive(inbox, &m, 0) != pdTRUE) return false;
  out = m.s;
  out.trim();
  return true;
}

}  // namespace bleuart
