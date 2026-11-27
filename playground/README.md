# ESP32-S3 能力展示韌體

前期開發用的探索工具，燒進 N16R8 開發板後，用手機或筆電連上板子的熱點，就能在網頁上直接操作各項功能，了解這塊板子實際做得到什麼。

## 功能

| 區塊 | 內容 |
|---|---|
| 晶片資訊 | 型號、核心與時脈、晶片溫度、flash 與 PSRAM 用量、可用記憶體、程式大小，每 2 秒更新 |
| RGB LED | 用調色盤控制板上的 WS2812 |
| 觸控腳位 | 即時讀取 GPIO1、2、5、6、7、10 到 14 的電容觸控數值 |
| 傳輸速度 | 從板子下載 512 KB 到 8 MB 的測試資料，算出 Mbit/s |
| Wi-Fi 掃描 | 周圍的基地台、訊號、頻道與加密方式 |
| BLE 掃描 | 被動掃描 3 秒，列出位址、訊號、名稱與廠商資料 |
| 運算效能 | 整數、單精度與雙精度浮點、硬體加速的 SHA-256 與 AES、SRAM 與 PSRAM 的搬移速度、雙核心加速倍數，以及 int16 矩陣乘法與 FFT 在一般 C 與向量指令版本的速度對照 |
| 外部 Wi-Fi | 讓板子同時連上家裡的網路，熱點維持開啟 |
| OTA 更新 | 上傳 `.bin` 檔、從網址下載，或設定定期檢查新版 |

連上熱點後手機會自動跳出控制頁（captive portal），沒有跳出的話用瀏覽器開 `http://192.168.4.1`。熱點名稱是 `S3-Playground-` 加上第一次開機時隨機產生的四碼，密碼 `playground`。熱點與連外用的 Wi-Fi 每次開機都換一組隨機的 MAC，藍牙對外使用定期更換的隨機位址（RPA），周圍的人與連上的路由器都沒辦法用固定的位址認出這塊板子。

## 手機藍牙

板子同時用藍牙廣播，名稱與熱點相同，提供 Nordic UART Service（NUS）的文字通道。手機用支援 NUS 的藍牙終端機 App 連上，輸入文字指令，結果用文字傳回來，不必連 Wi-Fi。Android 可以用 Serial Bluetooth Terminal，iOS 可以用 Bluefruit Connect 的 UART 功能或 nRF Toolbox。

| 指令 | 作用 |
|---|---|
| `help` | 列出指令 |
| `info` | 晶片、溫度、記憶體、版本 |
| `temp` | 晶片溫度 |
| `led 255 0 0` | 設定 LED 顏色，`led off` 關燈 |
| `touch` | 觸控腳位數值 |
| `wifi` | 掃描 Wi-Fi，列出最強的 8 個 |
| `ble` | 掃描 BLE 3 秒，列出最強的 8 個 |
| `bench` | 運算效能測試 |
| `ip` | 熱點與外部 Wi-Fi 的 IP |
| `ssid 名稱`、`wpass 密碼` | 藍牙配網，見下方 |
| `forget` | 忘記外部 Wi-Fi |

連線要先配對，手機跳出配對視窗時輸入 6 位數的配對碼。配對碼第一次開機時隨機產生、存在 NVS，顯示在控制頁的晶片資訊與序列埠。

### 藍牙配網

讓板子連上家裡的 Wi-Fi，不必連到板子的熱點。藍牙配對連線後依序送出：

```text
ssid 家裡的網路名稱
wpass 網路密碼
```

板子開始連線，連上後回報 IP，20 秒內沒連上也會通知。設定存在 NVS，重新開機後自動連線，`forget` 忘記。只支援 2.4 GHz 的網路。Wi-Fi 密碼只在加密的配對連線上傳送，不寫進事件紀錄。

ESP32-S3 只支援低功耗藍牙（BLE），沒有傳統藍牙，所以舊 ESP32 範例常用的 `BluetoothSerial` 在 S3 上不能用。

## 燒錄

```bash
pio run -t upload
pio device monitor    # 看開機訊息與事件紀錄
```

## 測試 OTA

1. 把 `playground.ino` 開頭的 `FW_BUILD` 加一，執行 `pio run`，新韌體在 `.pio/build/esp32-s3-n16r8/firmware.bin`
2. 上傳檔案：控制頁選這個檔案，按「上傳並更新」
3. 從網址拉或定期檢查：板子先連上外部 Wi-Fi，在電腦上開一個簡易伺服器

    ```bash
    cd .pio/build/esp32-s3-n16r8
    printf 'build=2\nurl=http://<電腦的 IP>:8000/firmware.bin\n' > manifest.txt
    python3 -m http.server 8000
    ```

    控制頁填入 `http://<電腦的 IP>:8000/manifest.txt` 與檢查間隔，按「立刻檢查」

更新完成後板子會重新開機，控制頁的「目前版本」與「執行中的分區」會跟著改變（`app0` 與 `app1` 輪流使用）。

開發階段的 OTA 沒有驗證簽章，HTTPS 也不驗憑證，要放進正式專案前再補上。
