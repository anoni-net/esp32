"""建置時修改 ESP Web Tools 的兩件事，static/esp-web-tools/ 裡的原始檔不動。

1. 一般安裝不清除整片 flash：ESP Web Tools 遇到沒有 Improv 的板子會當成全新安裝，先 eraseFlash() 再寫入，
   板子上的設定（名稱、密碼、更新用的 Wi-Fi）、警示歷史與記錄都會被清掉。這個專案的決定是不預設清除，
   只寫開機程式、分割表、otadata 與程式槽（anoni-net-ws#519），改成跟「更新」一樣的流程
2. 對話框的文字翻成正體中文：ESP Web Tools 沒有多語系，安裝選項會變成「Install 隨身偵測器」這種中英混雜

每一條寫出預期比對到的次數，升級 ESP Web Tools 之後字串變了，建置會失敗，不會默默少翻或沒有改到清除的流程。
"""
import re
from pathlib import Path


def ws(text):
    """一段英文句子的正規表示式：壓縮後的模板字串可能在任何空白處換行。"""
    return r"\s+".join(re.escape(w) for w in text.split())


# (檔名開頭, 正規表示式, 取代, 預期次數)。空白用 \s+，壓縮後的模板字串裡保留了換行與縮排
PATCHES = [
    # --- 1. 不清除 flash：沒有 Improv 時 _startInstall(!0)（清除）改成 _startInstall(!1) ---
    ("install-dialog", r'this\._manifest\.new_install_prompt_erase\?this\._state="ASK_ERASE":this\._startInstall\(!0\)',
     'this._manifest.new_install_prompt_erase?this._state="ASK_ERASE":this._startInstall(!1)', 2),
    # --- 2. 對話框的文字 ---
    ("install-dialog", r'`Install \$\{this\._manifest\.name\}`', '`安裝${this._manifest.name}`', 2),
    ("install-dialog", r'`Update \$\{this\._manifest\.name\}`', '`更新${this._manifest.name}`', 1),
    ("install-dialog", r'>Logs & Console<', '>查看板子的輸出<', 2),
    ("install-dialog", r'_renderProgress\("Connecting"\)', '_renderProgress("連線中")', 1),
    ("install-dialog", r't="Confirm Installation"', 't="確認安裝"', 1),
    ("install-dialog", r'"update to":"install"', '"更新成":"安裝"', 1),
    ("install-dialog", r'Your device is running\s+\$\{this\._info\.firmware\}&nbsp;\$\{this\._info\.version\}\.',
     '板子目前的韌體是 ${this._info.firmware}&nbsp;${this._info.version}。', 1),
    ("install-dialog", r'Do you want to \$\{s\}\s+\$\{this\._manifest\.name\}&nbsp;\$\{this\._manifest\.version\}\?',
     '要${s}${this._manifest.name}（${this._manifest.version}）嗎？板子上的設定與記錄會保留。', 1),
    ("install-dialog", r'All data on the device will be erased\.', '板子上所有的資料都會被清除。', 1),
    ("install-dialog", r'>\s*Back\s*</ew-text-button>', '>返回</ew-text-button>', 5),
    ("install-dialog", r'>\s*Install\s*</ew-text-button>', '>安裝</ew-text-button>', 1),
    ("install-dialog", r'>\s*Next\s*</ew-text-button>', '>下一步</ew-text-button>', 2),
    ("install-dialog", r'>\s*Close\s*</ew-text-button>', '>關閉</ew-text-button>', 1),
    ("install-dialog", r'_renderProgress\("Preparing installation"\)', '_renderProgress("準備安裝")', 1),
    ("install-dialog", r't="Installing"', 't="安裝中"', 3),
    ("install-dialog", r's="Installing"', 's="寫入中"', 1),
    ("install-dialog", r's="Wrapping up"', 's="收尾中"', 1),
    ("install-dialog", r'_renderProgress\("Erasing"\)', '_renderProgress("清除中")', 1),
    ("install-dialog", r'This\s+will\s+take\s+\$\{"ESP8266"===this\._installState\.chipFamily\?"a minute":"2 minutes"\}\.<br />\s*'
                       + ws("Keep this page visible for fastest installation."),
     '大約需要${"ESP8266"===this._installState.chipFamily?"1 分鐘":"2 分鐘"}，過程中不要拔掉 USB 線。<br />'
     '讓這個分頁留在畫面上，安裝會比較快。', 1),
    ("install-dialog", r'label="Installation complete!"', 'label="安裝完成！"', 1),
    ("install-dialog", r't="Installation failed"', 't="安裝失敗"', 1),
    ("install-dialog", r'\["Error",', '["發生錯誤",', 1),
    ("install-dialog", r'\["Logs",', '["板子的輸出",', 1),
    ("install-dialog", r'>\s*Reset Device\s*</ew-text-button>', '>重新啟動板子</ew-text-button>', 1),
    ("install-dialog", r'>\s*Download Logs\s*</ew-text-button>', '>下載輸出</ew-text-button>', 1),
    ("install-dialog", r'"Failed to initialize\. Try resetting your device or holding the BOOT button while clicking INSTALL\."',
     '"連不上板子。拔掉 USB 線，按住板子上的 BOOT 鍵再接上線，放開之後重新按安裝。"', 1),
    ("install-dialog", r'`Your \$\{o\} board is not supported\.`', '`不支援這塊板子（${o}），請確認是 ESP32-S3 開發板。`', 1),
    # 沒選到序列埠時的說明
    ("no-port-picked", r'>No port selected<', '>沒有選到板子<', 1),
    ("no-port-picked", ws("If you didn't select a port because you didn't see your device listed, try the following steps:"),
     '視窗裡沒有看到板子的話，試試下面幾件事：', 1),
    ("no-port-picked", ws('Make sure that the device is connected to this computer (the one that runs the browser that shows this website)'),
     '確認板子接在這台電腦上（正在開這個網頁的電腦）', 1),
    ("no-port-picked", ws('Most devices have a tiny light when it is powered on. If yours has one, make sure it is on.'),
     '板子接上電之後燈會亮，確認燈有亮起來。', 1),
    ("no-port-picked", ws('Make sure that the USB cable you use can be used for data and is not a power-only cable.'),
     '確認 USB 線能傳資料，只能充電的線電腦看不到板子。', 1),
    ("no-port-picked", ws('Make sure you have the right drivers installed. Below are the drivers for common chips used in ESP devices:'),
     '確認裝了需要的驅動程式。下面是 ESP 開發板常見的 USB 晶片的驅動程式：', 1),
]


def apply(out_dir: Path) -> list[str]:
    """修改 out_dir（輸出的 static/esp-web-tools/）裡的檔案，回傳問題清單。

    改過的檔案換一個帶內容雜湊的檔名，並改寫其他檔案裡 import 它的路徑。ESP Web Tools 用固定的檔名載入
    這些檔案、網址不帶版本參數，內容改了檔名不變的話，瀏覽器與 Cloudflare 會給舊的檔案（未翻譯、會清除 flash）。
    """
    import hashlib
    problems = []
    changed = set()
    for prefix, pattern, repl, expected in PATCHES:
        files = sorted(out_dir.glob(f"{prefix}-*.js")) if prefix != "index" else sorted(out_dir.glob("index-*.js"))
        if len(files) != 1:
            problems.append(f"ESP Web Tools：找不到 {prefix}-*.js")
            continue
        f = files[0]
        text = f.read_text(encoding="utf-8")
        new, n = re.subn(pattern, lambda m: repl, text)
        if n != expected:
            problems.append(f"ESP Web Tools：{f.name} 的「{pattern[:50]}」比對到 {n} 次，預期 {expected} 次")
            continue
        if new != text:
            f.write_text(new, encoding="utf-8")
            changed.add(f)
    if problems:
        return problems
    renames = {}
    for f in sorted(changed):
        h = hashlib.sha256(f.read_bytes()).hexdigest()[:8]
        new_name = f"{f.stem}.zh-{h}.js"
        f.rename(f.with_name(new_name))
        renames[f.name] = new_name
    for js in out_dir.glob("*.js"):
        text = js.read_text(encoding="utf-8")
        new = text
        for old, nn in renames.items():
            new = new.replace(f'"./{old}"', f'"./{nn}"')
        if new != text:
            js.write_text(new, encoding="utf-8")
    for old in renames:
        for js in out_dir.glob("*.js"):
            if f'"./{old}"' in js.read_text(encoding="utf-8"):
                problems.append(f"ESP Web Tools：{js.name} 還在載入改名前的 {old}")
    return problems
