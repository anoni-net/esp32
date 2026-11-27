+++
title = "安裝韌體"
lead = "把韌體寫進 ESP32-S3 開發板，只要做一次，之後的更新在手機面板上按一下就好。"
+++

直接在網頁上燒錄的功能還在準備，完成之後用電腦上的 Chrome 或 Edge 打開這一頁，接上板子按一下就能安裝。在那之前，先用 PlatformIO 自行建置、燒錄，步驟如下。

## 準備

- ESP32-S3 N16R8 開發板，挑選方式見 [ESP32 是什麼]({root}about-esp32/)
- 能傳資料的 USB-C 線
- 一台電腦（Windows、macOS 或 Linux），裝好 [Git](https://git-scm.com/) 與 [PlatformIO Core](https://platformio.org/install/cli)

## 燒錄隨身偵測器

1. 下載原始碼：

    ```bash
    git clone https://github.com/anoni-net/esp32.git
    cd esp32/pocket-detector
    ```

2. USB 線接開發板標著 `COM` 的那個孔（另一個標 `USB` 的孔接的是晶片本身，燒錄時不用）
3. 建置並燒錄，第一次會下載建置工具，需要幾分鐘：

    ```bash
    pio run -t upload
    ```

4. 看到 `SUCCESS` 就完成了。想看板子的開機訊息，執行 `pio device monitor`

上傳失敗或偵測不到板子時，按住板子上的 BOOT 鍵、按一下 RST 鍵、放開 BOOT 鍵，讓板子進入燒錄模式再試一次。Mac 用 `COM` 孔偵測不到時，可能要另外安裝 USB 轉序列埠的驅動程式。

習慣用 Arduino IDE 的話，設定方式寫在[原始碼的說明](https://github.com/anoni-net/esp32/tree/main/pocket-detector#arduino-ide)。

## 燒好之後

1. 長按 BOOT 鍵 2 秒，板子打開 Wi-Fi 熱點，名稱是 `BLE-Tracker-` 加上四個英數字
2. 手機連上熱點，預設密碼是 `anoni-tracker`，面板會自動跳出來。沒有跳出來時，用瀏覽器打開 `http://192.168.4.1`
3. 第一件事到「設定」改掉熱點的名稱與密碼，預設密碼公開在這裡，任何人都知道
4. 之後要更新，在面板「設定 → 韌體」設定一個能上網的 Wi-Fi，按「檢查並安裝新版」

## 示範韌體

示範韌體只提供 PlatformIO 的燒錄方式，在原始碼的 `playground` 資料夾執行 `pio run -t upload`，說明見[示範韌體]({root}playground/)。

## 用 Tor Browser 或手機打開這一頁

網頁燒錄要用到瀏覽器的 Web Serial，只有電腦上的 Chrome 與 Edge 支援。Tor Browser、Safari、Firefox 與手機的瀏覽器都沒有這個功能，請改用上面 PlatformIO 的方式。
