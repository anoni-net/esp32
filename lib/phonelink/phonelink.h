// 手機用藍牙連線的文字通道，採用 Nordic UART Service（NUS）。
// 手機用支援 NUS 的藍牙終端機 App 連上，送出文字指令，板子用文字回覆。
//
// 開啟時板子會發出藍牙廣播，周圍的裝置收得到，所以平常關閉，
// 長按 BOOT 鍵才開啟，用完關閉或閒置一段時間後自動關閉。
//
// 連線要先配對：第一次連線時手機跳出配對視窗，輸入 6 位數的配對碼（顯示在 Wi-Fi 面板
// 與序列埠）。連線加密並通過驗證之前，板子不接受指令，也不送出任何文字。
#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

namespace phonelink {

static const char* SVC_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // 手機 → 板子
static const char* TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // 板子 → 手機

struct Msg {
  char s[96];
};

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* tx = nullptr;
static QueueHandle_t inbox = nullptr;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool connected = false;
static volatile bool secure = false;  // 連線已加密，而且通過配對碼驗證
static bool on = false;
static uint32_t passkey = 0;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    conn = info.getConnHandle();
    connected = true;
    secure = info.isEncrypted() && info.isAuthenticated();  // 已配對過的手機重新連線時直接加密
    // 診斷用，只印在序列埠，不寫進 flash 記錄
    Serial.printf("BLEEV,connect,enc=%d,auth=%d,bonded=%d,bonds=%d\n", info.isEncrypted(), info.isAuthenticated(),
                  info.isBonded(), NimBLEDevice::getNumBonds());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    // reason 是 NimBLE 的錯誤碼，0x200 加上 HCI 的原因碼，例如 0x213 對方中斷、0x23d MIC 錯誤（金鑰不符）
    Serial.printf("BLEEV,disconnect,reason=0x%x\n", reason);
    connected = false;
    secure = false;
    conn = BLE_HS_CONN_HANDLE_NONE;
  }
  // 板子沒有螢幕可以即時顯示，配對碼是事先產生、存在 NVS 的固定 6 位數
  uint32_t onPassKeyDisplay() override {
    Serial.println("BLEEV,passkey_display");
    return passkey;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    secure = info.isEncrypted() && info.isAuthenticated();
    Serial.printf("BLEEV,auth_complete,enc=%d,auth=%d,bonded=%d,keysize=%d\n", info.isEncrypted(),
                  info.isAuthenticated(), info.isBonded(), info.getSecKeySize());
    if (!secure) server->disconnect(info.getConnHandle());  // 配對失敗就中斷
  }
};

// 寫入在藍牙的工作執行，這裡只把文字丟進佇列，指令在 loop() 處理
class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    if (!on || !secure) {
      Serial.printf("BLEEV,write_rejected,on=%d,secure=%d\n", on, secure);
      return;
    }
    const std::string v = c->getValue();
    Msg m;
    const size_t n = v.size() < sizeof(m.s) - 1 ? v.size() : sizeof(m.s) - 1;
    memcpy(m.s, v.data(), n);
    m.s[n] = 0;
    xQueueSend(inbox, &m, 0);
  }
};

// 更新藍牙名稱，廣播中就重新開始廣播
static void rename(const String& name) {
  if (!server) return;
  NimBLEDevice::setDeviceName(name.c_str());
  NimBLEAdvertisementData sr;
  sr.setName(name.c_str());
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setScanResponseData(sr);
  if (on) {
    adv->stop();
    adv->start();
  }
}

// 更新配對碼，之後的配對使用新的配對碼
static void setPasskey(uint32_t pin) {
  passkey = pin;
  if (server) NimBLEDevice::setSecurityPasskey(pin);
}

// 清除所有已配對的手機，連線中的也中斷
static void clearBonds() {
  NimBLEDevice::deleteAllBonds();
  if (connected && server) server->disconnect(conn);
}

static int bondCount() { return NimBLEDevice::getNumBonds(); }

// 第一次開啟時才建立服務。呼叫前要先 NimBLEDevice::init()
static void enable(const String& name, uint32_t pin) {
  passkey = pin;
  if (on) return;
  if (!server) {
    inbox = xQueueCreate(6, sizeof(Msg));
    NimBLEDevice::setMTU(247);
    // 配對並記住手機（bonding）、防止中間人（MITM，要輸入配對碼）、LE Secure Connections
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    NimBLEDevice::setSecurityPasskey(pin);
    server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCB());
    NimBLEService* svc = server->createService(SVC_UUID);
    // 讀寫都要求加密並通過驗證，手機寫入時會自動跳出配對視窗
    tx = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                                NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
    NimBLECharacteristic* rx = svc->createCharacteristic(
        RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC |
                     NIMBLE_PROPERTY::WRITE_AUTHEN);
    rx->setCallbacks(new RxCB());
    svc->start();

    // 128 位元的 UUID 加上名稱超過 31 bytes，名稱放在掃描回應
    NimBLEAdvertisementData ad;
    ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    ad.addServiceUUID(SVC_UUID);
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAdvertisementData(ad);
    adv->enableScanResponse(true);
  }
  rename(name);
  on = true;
  server->advertiseOnDisconnect(true);
  NimBLEDevice::getAdvertising()->start();
}

static void disable() {
  if (!on) return;
  on = false;
  server->advertiseOnDisconnect(false);
  NimBLEDevice::getAdvertising()->stop();
  if (connected) server->disconnect(conn);
}

// 依連線協商出的 MTU 切段送出
static void send(const String& text) {
  if (!on || !connected || !secure || !tx) return;
  const uint16_t mtu = server->getPeerMTU(conn);
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

}  // namespace phonelink
