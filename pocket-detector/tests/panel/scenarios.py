"""面板的虛構狀態，面板測試（test_panel.py）、網站的截圖與模擬面板（site/build.py）共用。

欄位對應 pocket_detector/webpanel.h 的 statusJson() 與各個 /api 的 JSON。名稱與位址都是虛構的，
不要換成實際收到的資料。
"""

# 虛構的狀態。欄位對應 webpanel.h 的 statusJson()
BASE = {
    "scan": 412, "uptime": 7200, "trackers": 2, "alert": True, "marks": 1, "timeKnown": True, "epoch": 1795538292,
    "logMounted": True, "logOn": True, "log": True, "logFull": False, "logKB": 812, "wifiNow": 0, "quiet": False,
    "captive": True, "stats": False, "statsOn": False, "statsAllowed": True, "statNow": 0, "statPersist": 0,
    "statTotal": 0, "bleOn": False, "bleConnected": False, "bleSecure": True, "bleBonds": 0, "blePin": 123456,
    "needSpan": 600, "needNear": 6, "needPresence": 70, "needRssi": -85, "needPlaces": 0, "placeId": 0, "placeName": "",
    "moving": False, "appleDevices": 3, "mode": "detect", "automode": 0, "autosound": 0, "wifiscan": False,
    "preset": 1, "interval": 15, "duration": 5, "idle": 10, "led": True, "apName": "Test-Panel-0000",
    "defaultPass": False,
    "trk": [
        {"addr": "02:00:00:00:00:01", "type": "Apple", "rssi": -58, "span": 1520, "ago": 0, "win": 40, "near": 31,
         "pres": 92, "alerted": True, "acked": False, "places": 0, "rh": [-80, -78, -79, -70, -66, -60]},
        {"addr": "02:00:00:00:00:02", "type": "Tile", "rssi": -90, "span": 120, "ago": 30, "win": 5, "near": 0,
         "pres": 40, "alerted": False, "acked": False, "places": 0, "rh": [-62, -64, -63, -75, -80, -88, -90]},
    ],
    "recent": [{"n": 1, "uptime": 1750, "epoch": 1795532842, "label": "出門"}], "histVer": 1,
    "findOn": False, "findAddr": "", "findLabel": "", "findType": "", "findWifi": False, "findRssi": 0,
    "findSeen": False, "findAgo": 0, "findLeft": 0,
    "soundBusy": False, "soundAddr": "", "soundMsg": "", "soundAgo": 0,
    "roomState": 0, "roomVer": 0, "roomLeft": 0, "roomEst": 130, "roomlog": False,
    "build": "Nov 27 2026 10:00:00", "rev": "abc1234", "part": "app0", "otaState": 0, "otaLeft": 0,
    "netSsid": "TestNet", "netState": 0, "netMsg": "", "netBuild": "", "netRev": "", "netGot": 0, "netTotal": 0,
    "probeOn": False, "probeVer": 0, "meetPhase": 0, "meetVer": 0, "meetAlert": False, "every": 30,
    "scanAgo": 12000, "bleNow": 43, "mem": [88064, 33792, 70000, 8126464, 0, 0],
    "testchan": False, "autoupd": False, "netLastAuto": False, "netAgo": 0, "autoNext": 0, "autoFrom": "",
}
ROOM_IDLE = {"ver": 0, "state": "idle", "items": []}
PLACES = {"cur": 3, "curSaved": True, "max": 32, "list": [
    {"id": 3, "name": "家", "visits": 4, "aps": 24}, {"id": 7, "name": "", "visits": 1, "aps": 18}]}
HIST_PATH = {"name": "Test-Panel-0000", "epoch": 1795538292, "uptime": 7200, "trk": [
    {"addr": "02:00:00:00:00:01", "type": "Apple", "first": 600, "last": 7000, "alert": 3000, "acked": False,
     "maxRssi": -55, "scans": 120, "sound": -1, "autoOff": False, "iv": [[600, 7000]], "pl": [3, 5, 9]}],
    "places": {"3": "家", "9": "公司"}, "marks": []}
MEET_BASE = {"ver": 2, "phase": 1, "rssi": -70, "sec": 20, "baseLeft": 40, "base": 3, "new": 0, "lvl": 0, "alert": False, "rate": 0,
             "baseRate": 0, "probes": 0, "maxNew": 0, "devs": []}
MEET_ALERT = {"ver": 9, "phase": 2, "rssi": -70, "sec": 900, "baseLeft": 0, "base": 3, "new": 1, "lvl": 2, "alert": True, "rate": 1.5,
              "baseRate": 0, "probes": 3, "maxNew": 1, "groups": 1,
              "devs": [{"i": 7, "r": -55, "best": -50, "ago": 2, "since": 120, "first": 780, "last": 898, "now": True, "fl": 20,
                        "use": 10, "air": -1, "rh": [-70, -66, -60, -58, -55, -55], "l": "iPhone 或 iPad（有行動網路）",
                        "nm": "Test <的> iPhone", "act": "螢幕亮著"},
                       {"i": 9, "r": -68, "best": -65, "ago": 300, "since": 600, "first": 300, "last": 600, "now": False, "fl": 0,
                        "use": -1, "air": -1, "rh": [-68], "l": "Windows 筆電", "nm": "", "act": ""}],
              "wg": [{"fp": "1a2b3c4d", "n": 4, "first": 800, "last": 890, "r": -52, "now": True, "apple": False,
                      "m": "samsung SM-S911B", "nm": "測試的 S23"}]}
PROBES = {"ver": 3, "on": True, "sec": 130, "left": 170, "ch": 6, "frames": 523, "src": 87, "fixed": 12, "directed": 41,
          "full": False, "ssids": [{"src": 5, "f": 12, "fx": False, "r": -55, "n": "Free-Public"},
                                   {"src": 1, "f": 9, "fx": True, "r": -48, "n": 'Home "5G" <b>'}]}
WIFI_ON = {"wifiscan": True, "needPlaces": 3, "placeId": 3, "placeName": "家", "wifiNow": 12}

# 把面板的 fetch 換成回傳假資料。%(fx)s 是 /api/status，其餘依網址開頭。POST 一律回 ok
FETCH_STUB = """const __FX=%(fx)s,__RX=%(rx)s,__HX=%(hx)s,__PX=%(px)s,__QX=%(qx)s,__MX=%(mx)s;
window.fetch=async(u,o)=>{u=String(u);return {ok:true,status:200,json:async()=>u.startsWith('/api/room')?__RX:u.startsWith('/api/history/prev')?[]:u.startsWith('/api/history')?__HX:u.startsWith('/api/places')?__PX:u.startsWith('/api/probes')?__QX:u.startsWith('/api/meeting')?__MX:__FX,text:async()=>'ok'};};
"""


# 網站的截圖與模擬面板用的情境。tab 是一開始顯示的分頁，scroll 是截圖時捲到的元素 id
_NO_TRACKER = {"trackers": 0, "alert": False, "trk": [], "recent": [], "marks": 0, "apName": "BLE-Tracker-0000",
               "appleDevices": 4, "bleNow": 37, "rev": "demo", "build": "Nov 27 2026 12:00:00"}
_DEMO_BASE = dict(BASE, apName="BLE-Tracker-0000", rev="demo", build="Nov 27 2026 12:00:00", netSsid="",
                  logOn=False, log=False)
_ROOM_DONE = {"ver": 4, "state": "done", "at": 7100, "dur": 128, "chs": [1, 6, 11], "aps": 18, "stas": 9, "rand": 4,
              "ble": 21, "drop": 0, "items": [
    {"i": 0, "k": "sta", "lv": 2, "name": "示範品牌 的裝置", "sub": "連在「Room-1203」", "rssi": -48, "ch": 6, "up": 38912,
     "why": "MAC 位址屬於攝影機品牌 示範品牌\n持續上傳：3 輪監聽有 3 輪在送資料，平均 38 KB/s，送出是收到的 21 倍\n"},
    {"i": 1, "k": "ble", "lv": 1, "name": "藍牙裝置", "sub": "示範模組廠", "rssi": -71, "ch": 0, "up": 0,
     "why": "藍牙廣播的公司代碼或服務屬於 示範模組廠，這家的晶片或模組常用在便宜的 Wi-Fi 攝影機，也用在插座、燈泡等智慧家電\n"},
    {"i": 2, "k": "ap", "lv": 0, "name": "「Room-1203」", "sub": "", "rssi": -40, "ch": 6, "up": 0, "why": ""},
    {"i": 3, "k": "sta", "lv": 0, "name": "隨機位址的裝置", "sub": "連在「Room-1203」", "rssi": -60, "ch": 6, "up": 0,
     "why": "位址是隨機產生的，多半是手機、平板或電腦\n"}]}
_PROBES_DEMO = {"ver": 3, "on": False, "sec": 300, "left": 0, "ch": 6, "frames": 486, "src": 63, "fixed": 9, "directed": 57,
                "full": False, "ssids": [
    {"src": 4, "f": 14, "fx": False, "r": -52, "n": "Cafe_Free_WiFi"},
    {"src": 3, "f": 9, "fx": False, "r": -61, "n": "Hotel-Guest"},
    {"src": 2, "f": 7, "fx": True, "r": -49, "n": "示範公司-員工"},
    {"src": 1, "f": 6, "fx": True, "r": -55, "n": "王小明的家"},
    {"src": 1, "f": 3, "fx": False, "r": -70, "n": "Airport-Free"}]}
_MEET_DEMO = dict(MEET_ALERT, devs=[dict(MEET_ALERT["devs"][0], nm="示範用的 iPhone"), MEET_ALERT["devs"][1]],
                  wg=[dict(MEET_ALERT["wg"][0], nm="示範用的 Galaxy")])
_HIST_DEMO = dict(HIST_PATH, name="BLE-Tracker-0000", places={"3": "家", "9": "辦公室"})

DEMO = [
    {"key": "clear", "title": "沒有發現", "desc": "附近沒有追蹤器，畫面顯示最近一次掃描與周圍的裝置數",
     "tab": "scan", "status": _NO_TRACKER},
    {"key": "alert", "title": "警示", "desc": "一個與主人分開的 Find My 配件持續跟著你，觸發警示",
     "tab": "scan", "status": {}},
    {"key": "history", "title": "警示歷史", "desc": "觸發過警示的追蹤器、出現的時段與依序經過的地點",
     "tab": "hist", "status": {}, "hist": _HIST_DEMO},
    {"key": "room", "title": "房間檢查", "desc": "旅館房間裡可能是攝影機的裝置，每一個都寫出判斷理由",
     "tab": "room", "status": dict(_NO_TRACKER, roomVer=4), "room": _ROOM_DONE, "scroll": "roomHead"},
    {"key": "meeting", "title": "會議室檢查", "desc": "建立基準之後，房間裡多出一支還開著藍牙的手機",
     "tab": "room", "status": dict(_NO_TRACKER, meetPhase=2, meetVer=9, meetAlert=True), "meeting": _MEET_DEMO},
    {"key": "probes", "title": "周圍手機洩漏的 Wi-Fi 名稱", "desc": "手機找 Wi-Fi 時送出的網路名稱，看得出去過哪些地方",
     "tab": "room", "status": dict(_NO_TRACKER, probeVer=3), "probes": _PROBES_DEMO, "scroll": "probeSum"},
]


def demo_script(scn, shot=False):
    """模擬面板與截圖用的 <script>：換掉 fetch，打開指定的分頁。

    模擬面板放在 sandbox 的 iframe 裡，讀不到 localStorage，所以載入後直接呼叫面板的 showTab()。
    shot=True 時不加「示範資料」的標示，捲到情境指定的元素，給截圖用。
    """
    import json
    fx = dict(_DEMO_BASE, **scn.get("status", {}))
    stub = FETCH_STUB % {"fx": json.dumps(fx, ensure_ascii=False), "rx": json.dumps(scn.get("room", ROOM_IDLE), ensure_ascii=False),
                         "hx": json.dumps(scn.get("hist", _HIST_DEMO), ensure_ascii=False),
                         "px": json.dumps(scn.get("places", PLACES), ensure_ascii=False),
                         "qx": json.dumps(scn.get("probes", {"ver": 0}), ensure_ascii=False),
                         "mx": json.dumps(scn.get("meeting", {"ver": 0}), ensure_ascii=False)}
    note = ("" if shot else "document.body.insertAdjacentHTML('afterbegin','<p role=\"note\" style=\"margin:0;padding:6px 12px;"
            "font:14px/1.5 system-ui,sans-serif;background:#fff3cd;color:#3d2e00;text-align:center\">示範資料：名稱與位址都是虛構的，"
            "按鈕不會連到任何裝置</p>');")
    # 無頭瀏覽器捲動之後截圖，畫面常常沒畫完。改成把目標所在區塊之前的兄弟元素藏起來，不必捲動
    scroll = ("setTimeout(()=>{let e=document.getElementById(%s);while(e&&e.parentElement&&e.parentElement.tagName!='SECTION')"
              "e=e.parentElement;for(let x=e&&e.previousElementSibling;x;x=x.previousElementSibling)x.hidden=true;},300);"
              % json.dumps(scn["scroll"]) if shot and scn.get("scroll") else "")
    # 不經過 fetch 的請求也擋掉：上傳韌體用 XMLHttpRequest，下載記錄直接換網址
    block = ("window.XMLHttpRequest=function(){this.open=this.send=this.setRequestHeader=()=>{};this.upload={};};"
             "addEventListener('click',e=>{const b=e.target.closest&&e.target.closest('[onclick*=\"location.href\"]');"
             "if(b){e.stopImmediatePropagation();e.preventDefault();toast('示範資料沒有記錄可以下載');}},true);")
    return ("<script>\n%s%s"
            "addEventListener('load',()=>{showTab(%s);%s%s});\n</script>\n") % (stub, block, json.dumps(scn["tab"]), note, scroll)
