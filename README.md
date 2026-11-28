<a id="zh-tw"></a>

# anoni.net ESP32 工坊

**正體中文** | [English](#en)

**只想使用工具、不會寫程式的話**：到 [anoni.net/esp32](https://anoni.net/esp32/) 用電腦的 Chrome 或 Edge [安裝韌體](https://anoni.net/esp32/install/)，接上 USB 按一下就好，再照[連上板子](https://anoni.net/esp32/connect/)的步驟用手機操作。這份 README 寫給要看原始碼或自己建置的人。

匿名網路社群 anoni.net 用 ESP32-S3 開發板做的隱私工具。反跟蹤工具需要讓人能檢查板子送出了什麼、記了什麼，所以原始碼公開。從這個 repo 建置、發布在 assets.anoni.net 的韌體都寫著建置它的 commit 與 SHA-256，可以對照這裡的原始碼。工作坊的參加者可以當場燒錄帶回家，想自己做一塊的人也可以照這裡的說明。

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

不會寫程式的人用電腦的 Chrome 或 Edge 打開[安裝頁](https://anoni.net/esp32/install/)，接上板子按一下就能安裝。從原始碼建置時，每個工具都是一個 PlatformIO 專案。裝好 [PlatformIO](https://platformio.org/install/cli) 之後：

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

發布時一起產生安裝頁（ESP Web Tools）用的 `manifest.json` 與開機程式、分割表、otadata 三個檔案，程式槽用的是同一個韌體檔。網頁安裝只寫這四段，不碰 NVS 的設定與 flash 記錄。

每個工具的通道目錄與建置設定寫在 `<工具>/publish.env`。上傳用 ssh 與 rsync，主機由 `PUBLISH_HOST`、`PUBLISH_ROOT` 指定，或寫在不進版控的 `tools/publish.local.env`。正式通道只放實機測試通過、從測試通道原封不動複製過去的檔案，流程見 [pocket-detector 的說明](pocket-detector/README.md#發布韌體到-assetsanoninet)。

## 測試

```bash
cd pocket-detector
./tools/run_tests.sh                      # 單元測試、面板測試、對比測試
./tools/run_tests.sh --hil /dev/cu.xxx    # 加上接在電腦上的板子做實機測試
```

GitHub Actions 在每個 PR 編譯兩個工具，並執行不需要板子的測試。

## 專案網頁

[anoni.net/esp32/](https://anoni.net/esp32/) 由 `site/build.py` 產生純 HTML，不載入 JavaScript、外部字型與第三方資源，clearnet 與 onion 各一份。首頁的工具清單來自各工具資料夾的 `site.toml`，介紹頁的內文是同一個資料夾的 `site.md`，新增一個工具資料夾就多一張卡片。安裝頁分成選工具（`/esp32/install/`）與各工具的安裝頁（`/esp32/install/<工具>/`），各語系的韌體寫在 `site.toml` 的 `[[firmware]]`，每個語系一個安裝按鈕。

```bash
uv run site/build.py            # 產生到 site/public/
uv run site/build.py --check    # 產生到暫存目錄並檢查站內連結、onion 版本與 320、390、1280 寬的版面
uv run site/shots.py            # 重新拍模擬面板的截圖（需要 Chromium 系的瀏覽器）
```

模擬面板是真正的面板網頁加上虛構的資料，情境跟面板測試共用（`pocket-detector/tests/panel/scenarios.py`）。改了面板或情境之後執行 `site/shots.py`，截圖連同 PNG 一起 commit。OLED 的畫面要接著板子，用序列埠的 `oled demo 名稱` 印出韌體裡寫死的假畫面，再用 `pocket-detector/tools/oled_to_png.py` 轉成 PNG，放在 `pocket-detector/site/oled/`。網站的做法照社群其他網站：靜態檔的網址帶內容雜湊（anoni-net/www）、版面檢查（anoni-net/news 的 `layout_check.py`）、onion 版本與 404、分享卡與 sitemap（www）、CSP 與安全標頭（vessel）。網頁由主機的 cron 拉 main 之後建置（`tools/deploy-m6.sh`），nginx 的設定與兩組 CSP 在 `tools/nginx-esp32.conf`。

## 授權

程式碼以 [MIT 授權](LICENSE)釋出。用到的程式庫與資料的授權見 [NOTICE](NOTICE)。

---

<a id="en"></a>

# anoni.net ESP32 Workshop

[正體中文](#zh-tw) | **English**

**If you only want to use the tools and don't write code**: go to [anoni.net/esp32](https://anoni.net/esp32/) and [install the firmware](https://anoni.net/esp32/install/) with Chrome or Edge on a computer. Plug in the USB cable and click once, then follow the steps to [connect to the board](https://anoni.net/esp32/connect/) and use it from your phone. The site is in Traditional Chinese for now, and the Pocket Detector firmware comes in Traditional Chinese, Simplified Chinese and English. This README is for people who want to read the source or build it themselves.

Privacy tools built by the anoni.net community on ESP32-S3 development boards. An anti-stalking tool has to let people check what the board sends and what it records, so the source is public. Every firmware built from this repository and published on assets.anoni.net states the commit it was built from and its SHA-256, so you can compare it against the source here. Workshop participants can flash a board on the spot and take it home, and anyone who wants to make one can follow the instructions here.

## Tools

| Directory | Contents |
|---|---|
| [pocket-detector](pocket-detector/) | Pocket Detector: finds AirTag, SmartTag, Tile and DULT trackers that keep following you, and can also check a hotel room for cameras (room check) and a closed-door meeting for devices whose wireless is still on (meeting room check). Connect your phone to the board's hotspot and use it from a web panel |
| [playground](playground/) | Demo firmware for trying out the ESP32-S3: chip information, LED, touch, Wi-Fi and Bluetooth scanning, compute benchmarks and OTA updates |
| [lib](lib/) | Shared components: firmware updates from a URL (`netupdate`) and a Bluetooth text channel with pairing (`phonelink`) |
| [tools](tools/) | Publishing scripts, given the tool to publish |
| [site](site/) | Build script, templates and shared pages for the project site at anoni.net/esp32/ |

The READMEs in each directory are in Traditional Chinese.

## Hardware

| Item | Notes |
|---|---|
| Development board | ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM), a 44-pin DevKitC-compatible board with two USB-C ports |
| GPIO expansion board (optional) | The development board plugs straight in, so the buzzer and OLED need no breadboard |
| Buzzer, OLED (optional) | Pocket Detector uses them for alerts and display; see [pocket-detector](pocket-detector/README.md#接線) for wiring (in Chinese) |

Only the ESP32-S3 N16R8 is officially supported. Other chips (ESP32-C3, the original ESP32, ESP32-C6) were test-compiled against the version of 2026-11-26 with a 4 MB flash partition table, but have not been run on real hardware.

## Flashing

If you don't write code, open the [install page](https://anoni.net/esp32/install/) in Chrome or Edge on a computer, plug in the board and click once. To build from source, each tool is a PlatformIO project. After installing [PlatformIO](https://platformio.org/install/cli):

```bash
cd pocket-detector
pio run -t upload
pio device monitor
```

Plug the USB cable into the port marked `COM` on the board. If the upload fails, hold the BOOT button, press the RST button once and release BOOT to put the board into download mode, then try again. Each tool's README covers the Arduino IDE; copy the folders under `lib/` into your Arduino `libraries` folder first.

Once flashed, Pocket Detector can update itself from a URL with "Check and install update" on the phone panel, without connecting to a computer again.

## Publishing

```bash
./tools/publish_firmware.sh pocket-detector            # build and publish to the test channel
./tools/publish_firmware.sh pocket-detector --dry-run  # build only and print latest.json
./tools/promote_firmware.sh pocket-detector            # promote to the stable channel after hardware testing passes
```

Publishing also produces the `manifest.json` for the install page (ESP Web Tools) and three files: the bootloader, the partition table and otadata. The app partition uses the same firmware file. Installing from the web writes only these four parts and leaves the NVS settings and flash log alone.

Each tool's channel directories and build settings are in `<tool>/publish.env`. Uploads use ssh and rsync, with the host given by `PUBLISH_HOST` and `PUBLISH_ROOT` or in `tools/publish.local.env`, which is not under version control. The stable channel only holds files that passed hardware testing and were copied unchanged from the test channel; see [the Pocket Detector notes](pocket-detector/README.md#發布韌體到-assetsanoninet) for the process (in Chinese).

## Tests

```bash
cd pocket-detector
./tools/run_tests.sh                      # unit, panel and color contrast tests
./tools/run_tests.sh --hil /dev/cu.xxx    # add hardware-in-the-loop tests with a board connected to the computer
```

GitHub Actions compiles both tools on every PR and runs the tests that don't need a board.

## Project site

[anoni.net/esp32/](https://anoni.net/esp32/) is plain HTML generated by `site/build.py`, with no JavaScript, external fonts or third-party resources, in one clearnet and one onion copy. The tool list on the home page comes from the `site.toml` in each tool's directory, and the tool page's text is the `site.md` in the same directory, so adding a tool directory adds a card. Installation is split into choosing a tool (`/esp32/install/`) and a page per tool (`/esp32/install/<tool>/`). The firmware for each language is listed under `[[firmware]]` in `site.toml`, with one install button per language.

```bash
uv run site/build.py            # build into site/public/
uv run site/build.py --check    # build into a temporary directory and check internal links, the onion copy and the layout at 320, 390 and 1280 px
uv run site/shots.py            # retake the screenshots of the simulated panel (needs a Chromium-based browser)
```

The simulated panel is the real panel page with made-up data, sharing its scenarios with the panel tests (`pocket-detector/tests/panel/scenarios.py`). After changing the panel or the scenarios, run `site/shots.py` and commit the screenshots along with the PNGs. OLED screens need a board: use `oled demo <name>` on the serial port to print the fake screens built into the firmware, then convert them to PNG with `pocket-detector/tools/oled_to_png.py` and put them in `pocket-detector/site/oled/`. The site follows the community's other sites: content-hashed URLs for static files (anoni-net/www), layout checks (`layout_check.py` in anoni-net/news), the onion copy and 404 page, share cards and sitemap (www), and CSP and security headers (vessel). The host's cron pulls main and builds the site (`tools/deploy-m6.sh`), and the nginx configuration with both CSPs is in `tools/nginx-esp32.conf`.

## License

The code is released under the [MIT License](LICENSE). Licenses for the libraries and data it uses are listed in [NOTICE](NOTICE).
