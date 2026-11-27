+++
title = "ESP32 是什麼"
lead = "社群的小工具都做在 ESP32-S3 開發板上。買板子之前，先了解它是什麼、做得到什麼，以及要挑哪一款。"
+++

## 一顆有 Wi-Fi 與藍牙的微控制器

微控制器是一顆把處理器、記憶體與各種介面做在一起的小晶片，常見於家電、玩具與感測器裡。它沒有作業系統，通電之後直接執行燒進去的一個程式，所以開機只要一兩秒，耗電也很少，接行動電源就能帶著走。

ESP32 是上海樂鑫科技（Espressif）推出的微控制器系列，特點是 Wi-Fi 與藍牙做在同一顆晶片上，價格便宜、資料公開，開發工具也是開放原始碼。用它做的小工具可以接收周圍的藍牙廣播、掃描 Wi-Fi 基地台，也能自己開一個熱點，讓手機連上來操作。

跟手機相比，它少了很多東西：沒有螢幕（可以另外接小的 OLED）、沒有 GPS、沒有行動網路，也沒辦法安裝 App。對隱私工具來說，少反而是優點，板子只做燒進去的那件事，原始碼公開，任何人都能檢查它送出了什麼、記了什麼。

## 社群選用 ESP32-S3

ESP32 有好幾個型號，社群的工具都做在 ESP32-S3 上。它同時有 Wi-Fi 與藍牙，收得到 AirTag、SmartTag、Tile 這些追蹤器用的低功耗藍牙廣播，處理速度與記憶體也夠放下手機操作頁面與記錄。

買的時候要挑型號後面寫著 `N16R8` 的開發板，代表記憶體夠大：16 MB 用來存韌體與記錄，斷電不會消失，8 MB 給板子運作時暫存資料。記憶體比較小的板子（例如 `N8R2`，或沒有 PSRAM 的版本）裝不進隨身偵測器。

板子的 Wi-Fi 只支援 2.4 GHz。從網路更新時要連 2.4 GHz 的 Wi-Fi，大部分家用路由器兩種頻段都有。

## 怎麼挑板子

<figure class="board"><img src="{static:esp32-s3-devkitc-1.png}" width="750" height="410" alt="Espressif 的 ESP32-S3-DevKitC-1 開發板照片，左邊是 ESP32-S3 模組，右側有兩個 USB 孔與 Reset、Boot 兩顆按鈕"><figcaption>Espressif 原廠的 ESP32-S3-DevKitC-1（照片取自 <a href="https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide.html">Espressif 的說明文件</a>）。社群常用的相容板外型相同，接頭換成 USB-C，下面那個孔的絲印是 <code>COM</code>。</figcaption></figure>

- 型號寫明 `ESP32-S3` 與 `N16R8`，商品名稱常寫成「ESP32-S3-DevKitC-1 N16R8」或「ESP32-S3 N16R8 開發板」
- 選有兩個 USB-C 孔的版本，一個標 `COM`（原廠板標 `UART`），安裝韌體時接這個孔，另一個標 `USB`
- 44 腳的 DevKitC 相容板最好。想加裝蜂鳴器或小螢幕（OLED）又不想焊接，可以加買一片插得上的擴展板，接線方式見[給開發者](../developers/#pins)
- 另外準備一條能傳資料的 USB-C 線，只能充電的線燒不了韌體

## 其他型號

目前只支援 ESP32-S3 N16R8。其他型號（ESP32-C3、第一代 ESP32、ESP32-C6 等）還沒在實際的板子上測試過，先不要買。

## 給開發者：規格 {#specs}

依 Espressif 公開的規格：

| 項目 | 規格 |
|---|---|
| 處理器 | 雙核心 Xtensa LX7，最高 240 MHz，另有支援向量指令的擴充，可以加速訊號處理與神經網路的運算 |
| 記憶體 | 晶片內 512 KB SRAM、384 KB ROM，另外可以接外部的 flash 與 PSRAM |
| Wi-Fi | 2.4 GHz，802.11 b/g/n，不支援 5 GHz |
| 藍牙 | 藍牙 5 的低功耗藍牙（Bluetooth LE），沒有傳統藍牙 |
| USB | 晶片內建全速 USB OTG 與 USB Serial/JTAG 控制器 |
| 腳位 | 45 支可程式化的 GPIO |

其他晶片的狀況：ESP32-C3、第一代 ESP32、ESP32-C6 用 4 MB flash 的分割表試編譯過，還沒在實機上執行。只有藍牙、沒有 Wi-Fi 的晶片做不出操作頁面與網址更新，不在支援的範圍。

### N16R8 的記憶體

同一款晶片的開發板會配不同大小的記憶體，型號後面的 `N16R8` 是 16 MB 的 flash 與 8 MB 的 PSRAM（Octal SPI 介面）：

- **flash**：存放韌體與資料，斷電不會消失。隨身偵測器把它分成兩個各 3 MB 的程式槽與約 9.9 MB 的資料區，更新時先寫進另一個程式槽，確認無誤才切換，更新失敗也不會變磚。資料區存放警示歷史與記錄
- **PSRAM**：額外的工作記憶體，斷電就消失。下載更新時的加密連線、探測請求的統計與會議室檢查的暫存資料都放在這裡

## 參考資料

- [ESP32-S3 產品頁](https://www.espressif.com/en/products/socs/esp32-s3)
- [ESP32-S3 Datasheet](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf)
- [ESP32 產品頁](https://www.espressif.com/en/products/socs/esp32)
