# anoni.net ESP32 小工具

匿名網路社群 anoni.net 用 ESP32-S3 開發板做的隱私小工具。反跟蹤工具需要讓人能檢查板子送出了什麼、記了什麼，所以原始碼公開。從這個 repo 建置、發布在 assets.anoni.net 的韌體都寫著建置它的 commit 與 SHA-256，可以對照這裡的原始碼。工作坊的參加者可以當場燒錄帶回家，想自己做一塊的人也可以照這裡的說明。

## 工具

| 資料夾 | 內容 |
|---|---|
| [pocket-detector](pocket-detector/) | 隨身偵測器：找出一直跟著你的 AirTag、SmartTag、Tile 與 DULT 追蹤器，也能檢查旅館房間裡的攝影機（房間檢查）與閉門會議裡還開著無線的裝置（會議室檢查）。手機連上板子的熱點，就能在網頁面板上操作 |
| [playground](playground/) | 體驗 ESP32-S3 各種功能的示範韌體：晶片資訊、LED、觸控、Wi-Fi 與藍牙掃描、運算效能、OTA 更新 |
| [lib](lib/) | 共用元件：從網址更新韌體（`netupdate`）、藍牙文字通道與配對（`phonelink`） |
| [tools](tools/) | 發布腳本，指定要發布哪個工具 |
| [site](site/) | 專案網頁 anoni.net/esp32/ 的建置腳本、模板與共用頁面 |

## 硬體

| 項目 | 說明 |
|---|---|
| 開發板 | ESP32-S3 N16R8（16 MB flash、8 MB PSRAM），雙 Type-C 的 44 腳 DevKitC 相容板 |
| GPIO 擴展板（選用） | 開發板直接插上去，接蜂鳴器與 OLED 時不必用麵包板 |
| 蜂鳴器、OLED（選用） | 隨身偵測器用來提醒與顯示，接線見 [pocket-detector](pocket-detector/README.md#接線) |

目前只正式支援 ESP32-S3 N16R8。其他晶片（ESP32-C3、初代 ESP32、ESP32-C6）在 2026-11-26 用 4 MB flash 的分割表試編譯過當時的版本，還沒在實機執行過。

## 燒錄

每個工具都是一個 PlatformIO 專案。裝好 [PlatformIO](https://platformio.org/install/cli) 之後：

```bash
cd pocket-detector
pio run -t upload
pio device monitor
```

USB 線接開發板標 `COM` 的那個孔。上傳失敗時，按住 BOOT 鍵、按一下 RST 鍵、放開 BOOT 鍵，讓板子進入下載模式再試一次。用 Arduino IDE 的做法寫在各工具的 README，要先把 `lib/` 底下的資料夾複製到 Arduino 的 `libraries` 資料夾。

燒好之後，隨身偵測器可以在手機面板按「檢查並安裝新版」從網址更新，不必再接電腦。

## 發布

```bash
./tools/publish_firmware.sh pocket-detector            # 建置並發布到測試通道
./tools/publish_firmware.sh pocket-detector --dry-run  # 只建置，印出 latest.json
./tools/promote_firmware.sh pocket-detector            # 實機測試通過後推到正式通道
```

每個工具的通道目錄與建置設定寫在 `<工具>/publish.env`。上傳用 ssh 與 rsync，主機由 `PUBLISH_HOST`、`PUBLISH_ROOT` 指定，或寫在不進版控的 `tools/publish.local.env`。正式通道只放實機測試通過、從測試通道原封不動複製過去的檔案，流程見 [pocket-detector 的說明](pocket-detector/README.md#發布韌體到-assetsanoninet)。

## 測試

```bash
cd pocket-detector
./tools/run_tests.sh                      # 單元測試、面板測試、對比測試
./tools/run_tests.sh --hil /dev/cu.xxx    # 加上接在電腦上的板子做實機測試
```

GitHub Actions 在每個 PR 編譯兩個工具，並執行不需要板子的測試。

## 專案網頁

[anoni.net/esp32/](https://anoni.net/esp32/) 由 `site/build.py` 產生純 HTML，不載入 JavaScript、外部字型與第三方資源，clearnet 與 onion 各一份。首頁的工具清單來自各工具資料夾的 `site.toml`，介紹頁的內文是同一個資料夾的 `site.md`，新增一個工具資料夾就多一張卡片。

```bash
uv run site/build.py            # 產生到 site/public/
uv run site/build.py --check    # 產生到暫存目錄並檢查站內連結與 onion 版本，CI 執行同一個指令
uv run site/shots.py            # 重新拍模擬面板的截圖（需要 Chromium 系的瀏覽器）
```

模擬面板是真正的面板網頁加上虛構的資料，情境跟面板測試共用（`pocket-detector/tests/panel/scenarios.py`）。改了面板或情境之後執行 `site/shots.py`，截圖連同 PNG 一起 commit。網頁由主機的 cron 拉 main 之後建置（`tools/deploy-m6.sh`），nginx 的設定與兩組 CSP 在 `tools/nginx-esp32.conf`。

## 授權

程式碼以 [MIT 授權](LICENSE)釋出。用到的程式庫與資料的授權見 [NOTICE](NOTICE)。
