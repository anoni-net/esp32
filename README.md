# anoni.net ESP32 小工具

匿名網路社群 anoni.net 用 ESP32-S3 開發板做的隱私小工具。反跟蹤工具需要讓人能檢查板子送出了什麼、記了什麼，所以原始碼公開。從這個 repo 建置、發布在 assets.anoni.net 的韌體都寫著建置它的 commit 與 SHA-256，可以對照這裡的原始碼。工作坊的參加者可以當場燒錄帶回家，想自己做一塊的人也可以照這裡的說明。

## 工具

| 資料夾 | 內容 |
|---|---|
| [pocket-detector](pocket-detector/) | 隨身偵測器：找出一直跟著你的 AirTag、SmartTag、Tile 與 DULT 追蹤器，也能檢查旅館房間裡的攝影機（房間檢查）與閉門會議裡還開著無線的裝置（會議室檢查）。手機連上板子的熱點，就能在網頁面板上操作 |
| [playground](playground/) | 體驗 ESP32-S3 各種功能的示範韌體：晶片資訊、LED、觸控、Wi-Fi 與藍牙掃描、運算效能、OTA 更新 |
| [lib](lib/) | 共用元件：從網址更新韌體（`netupdate`）、藍牙文字通道與配對（`phonelink`） |
| [tools](tools/) | 發布腳本，指定要發布哪個工具 |

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

## 授權

程式碼以 [MIT 授權](LICENSE)釋出。用到的程式庫與資料的授權見 [NOTICE](NOTICE)。
