+++
title = "給開發者"
lead = "原始碼、從原始碼建置、開發板的腳位，以及韌體怎麼發布與驗證。只想使用工具的話，看安裝韌體與連上裝置就夠了。"
+++

## 原始碼 {#source}

所有工具的原始碼在 [anoni-net/esp32](https://github.com/anoni-net/esp32)，以 MIT 授權釋出：

| 資料夾 | 內容 |
|---|---|
| `pocket-detector/` | 隨身偵測器，PlatformIO 專案。韌體在 `pocket_detector/`，手機面板的網頁是 `pocket_detector/panel.html` |
| `playground/` | 示範韌體，體驗 ESP32-S3 各項功能 |
| `lib/` | 共用元件：從網址更新韌體（`netupdate`）、藍牙文字通道與配對（`phonelink`） |
| `tools/` | 發布腳本 |
| `site/` | 這個網站的建置腳本 |

各工具的完整說明（序列埠指令、研究用的模式、面板的每個功能、記錄的格式）在各資料夾的 README，例如[隨身偵測器的說明](https://github.com/anoni-net/esp32/tree/main/pocket-detector)。

## 從原始碼建置與燒錄 {#build}

裝好 [PlatformIO Core](https://platformio.org/install/cli) 之後：

```bash
git clone https://github.com/anoni-net/esp32.git
cd esp32/pocket-detector
pio run -t upload       # 建置並燒錄
pio device monitor      # 看序列埠的輸出，鮑率 115200
```

平台與程式庫的版本固定在 `platformio.ini`（espressif32 8.0.0，Arduino 核心 3.3.12，NimBLE-Arduino 2.5.2，U8g2 2.36.18）。上傳失敗時，按住 `BOOT`、按一下 `RST`、放開 `BOOT`，讓開發板進入下載模式再試一次。

用 Arduino IDE 的話，先把 `lib/` 底下的資料夾複製到 Arduino 的 `libraries` 資料夾，開發板選 `ESP32S3 Dev Module`，Flash Size 16MB、PSRAM 選 OPI PSRAM、Partition Scheme 選 16M Flash（3MB APP/9.9MB FATFS），細節見[隨身偵測器的說明](https://github.com/anoni-net/esp32/tree/main/pocket-detector#arduino-ide)。

## 開發板與腳位 {#pins}

目前只正式支援 ESP32-S3 N16R8（16 MB flash、8 MB Octal PSRAM）的 44 腳 DevKitC 相容板，晶片規格見 [ESP32 是什麼]({root}about-esp32/#specs)。隨身偵測器用到的腳位：

| 腳位 | 用途 |
|---|---|
| GPIO0 | BOOT 鍵：長按開關 Wi-Fi、短按確認警示或打記號、雙擊尋找 |
| GPIO4 | 蜂鳴器（有源，高電位觸發） |
| GPIO8、GPIO9 | OLED 的 SDA、SCL（SSD1306，I2C 位址 `0x3C` 或 `0x3D`） |
| GPIO48 | 板上的 RGB LED（部分開發板是 GPIO38） |

接線時避開這幾支：

| 腳位 | 原因 |
|---|---|
| GPIO0、3、45、46 | 開機模式的 strapping pin |
| GPIO19、20 | USB |
| GPIO35 到 37 | N16R8 的 PSRAM |
| GPIO43、44 | `COM` 孔的序列埠 |

OLED 的 `VCC` 接 3.3V，ESP32-S3 的腳位不耐 5V。完整的接線說明見[隨身偵測器的接線](https://github.com/anoni-net/esp32/tree/main/pocket-detector#接線)。

分割表用 `app3M_fat9M_16MB.csv`：

| 分割區 | 位址 | 大小 | 用途 |
|---|---|---|---|
| nvs | `0x9000` | 20 KB | 設定（名稱、密碼、門檻、更新用的 Wi-Fi） |
| otadata | `0xE000` | 8 KB | 目前從哪個程式槽開機 |
| app0、app1 | `0x10000`、`0x310000` | 各 3 MB | 兩個程式槽，更新時寫進另一個，驗證通過才切換 |
| ffat | `0x610000` | 約 9.9 MB | flash 記錄、警示歷史、地點 |

## 韌體的發布與驗證 {#release}

韌體發布在 assets.anoni.net，分成兩個通道：

| 通道 | 網址 | 內容 |
|---|---|---|
| 正式 | `https://assets.anoni.net/esp32-tracker/` | 實機測試通過的版本，網頁安裝與自動更新都讀這裡 |
| 測試 | `https://assets.anoni.net/esp32-tracker-test/` | 每次修改都發布，給開發測試用的開發板 |

每個通道有 `latest.json`（從網路更新時讀）與 `manifest.json`（網頁安裝讀），寫著建置時間、commit、SHA-256 與檔名。韌體檔名帶 SHA-256 的前 12 碼，同一個檔名的內容不會改變。開發板下載時一邊寫進另一個程式槽一邊計算 SHA-256，跟 `latest.json` 相符才切換。網址寫死在韌體裡，知道熱點密碼的人也只能裝上社群發布的版本。

安裝頁分成兩層：`/esp32/install/` 選工具，`/esp32/install/<工具>/` 選語系並安裝。每個工具在 `site.toml` 用 `[[firmware]]` 列出各語系的韌體（語言、按鈕上的名稱、發布通道），新增語系時另外建置、發布到自己的通道，再加一段 `[[firmware]]`，安裝頁就多一個按鈕。

新版本先用 `tools/publish_firmware.sh` 發布到測試通道，開發測試用的開發板從測試通道更新、實機測試（`tests/hil/run_hil.py --update`）通過之後，才用 `tools/promote_firmware.sh` 把同一個檔案原封不動複製到正式通道。

正式通道目前的版本：

<!-- versions -->

韌體裡寫著建置時間，自己從同一個 commit 建置的檔案，SHA-256 不會跟發布的相同。要確認發布的版本，對照 commit 的原始碼，或用 `esptool image-info` 檢視下載的檔案。

## 測試 {#tests}

```bash
cd pocket-detector
./tools/run_tests.sh                    # 單元測試、面板測試、對比測試
./tools/run_tests.sh --hil /dev/cu.xxx  # 加上接在電腦上的開發板做實機測試
```

GitHub Actions 在每個 PR 編譯所有工具，執行不需要開發板的測試，並建置這個網站、用 320、390、1280 三種寬度檢查版面。

## 開發用的工具 {#devtools}

<!-- devtools -->

## 參與 {#contribute}

問題與建議歡迎到 [GitHub 的 issue](https://github.com/anoni-net/esp32/issues) 回報。程式碼的慣例與隱私上的規則寫在 repo 的 [AGENTS.md](https://github.com/anoni-net/esp32/blob/main/AGENTS.md)：實測的記錄不進版控，路人裝置的位址只用加鹽的雜湊。
