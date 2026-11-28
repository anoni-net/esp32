# AGENTS.md

給在這個 repo 工作的人與程式助理的規則。工具本身的說明在各工具的 README。

## 隱私

這些工具會收到路人裝置的藍牙位址、Wi-Fi 名稱與廣播的名稱，寫程式與處理資料時守住以下幾點：

- 實測的記錄（序列埠輸出、flash 記錄、`tests/hil/results/`）有路人裝置的位址，也可能看得出住處與行程，不進版控、不貼進 issue 或 PR，也不放進測試資料。測試資料用虛構的名稱與位址
- 新功能需要比對裝置時，位址用加鹽的雜湊，鹽值只放在記憶體。原始位址只在功能需要時放在記憶體，不寫進 flash、不印到序列埠
- 廣播的名稱、WPS 的裝置名稱、手機找過的 Wi-Fi 名稱常帶著主人的名字或去過的地方，只顯示在面板，不寫進 flash 記錄、不印到序列埠
- 密碼、藍牙配對碼不寫進任何記錄
- AirDrop 廣播裡的雜湊片段不解析
- README 與註解寫實測結果時，不寫出可以看出住處、通勤路線或常去地點的描述

## 發布與測試

- 改了 `pocket-detector` 之後執行 `pocket-detector/tools/run_tests.sh`，全部通過才 commit
- commit 之後用 `./tools/publish_firmware.sh <工具>` 發布到測試通道，打開「接收測試版」的開發用板子會裝上它
- 其他語系加上 `--lang zh-CN` 或 `--lang en`，各語系的測試通道與正式通道各自獨立，實機測試要用那個語系的韌體測
- 正式通道只用 `./tools/promote_firmware.sh <工具>` 推，條件是開發用的板子從測試通道裝上那個版本、`tests/hil/run_hil.py --update` 全部通過。`--force` 只在使用者同意的緊急修正時使用
- 已經燒進板子的韌體讀 `assets.anoni.net/esp32-tracker/` 與 `esp32-tracker-test/`。換網址之前，發布腳本要先同時發布新舊路徑，等板子都更新過再停掉舊路徑，否則已經在用的板子收不到更新

## 寫程式

- 平台與程式庫的版本固定在 `platformio.ini`。升級 Arduino 核心之後要重新確認網址更新（`lib/netupdate`）的 TLS 記憶體配置與實機的網址更新
- `.ino` 會被自動產生函式宣告：用到自訂型別的函式，型別要定義在標頭檔，函式宣告集中放在檔案前面
- 標頭檔裡的函式用 `static inline`，沒用到的 `static` 函式會被 `-Werror` 擋下
- 只依賴 C++ 標準函式庫的邏輯（版本判斷、地點比對、探測請求、裝置資訊、會議室檢查）寫在獨立的標頭檔，`tests/unit/` 在電腦上編譯同一份
- 面板網頁改 `panel.html`，PlatformIO 建置時會重新產生 `panel_gz*.h`，一起 commit
- 隨身偵測器有正體中文、簡體中文與英文三個版本（`pocket-detector/README.md` 的「多語系」）。面板與藍牙文字通道上的新字串用 `TR("…")` 包起來，改完執行 `tools/make_i18n.py --extract` 補上翻譯。序列埠的輸出維持正體中文，實機測試比對它的文字。面板與韌體之間不要比對顯示的文字，用欄位或狀態碼

## 專案網頁的兩種讀者

`site/` 產生的 anoni.net/esp32/ 有兩種讀者：不會寫程式、接上 USB 就要能裝好並用手機或電腦連上裝置的人，以及會寫程式的工程師。每一頁都先寫給前者，用操作上看得到的說法（操作頁面、Wi-Fi、BOOT 鍵），腳位、建置、分割表、版本雜湊這些技術內容放在頁尾的「給開發者」段落或 `developers/` 頁。指實體的硬體時寫「開發板」，裝好韌體之後可以寫工具名稱或「裝置」，不寫口語的「板子」。開發用的工具在 `site.toml` 寫 `audience = "dev"`，不列在首頁與導覽列。網頁上引用的按鈕名稱與燈號要對照 `panel.html` 與韌體，不要只照 README 寫。

## 文字

README、註解與面板文字用正體中文，照 [anoni.net 寫作風格規範](https://anoni.net/join/writing-style/)。根目錄的 `README.md` 後半是英文版，改中文段落時一起更新，英文段引用的按鈕名稱照 `pocket-detector/i18n/panel.toml` 的 `en`。
