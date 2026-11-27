#!/usr/bin/env python3
"""面板測試：在無頭瀏覽器載入 panel.html 的面板，餵假的狀態，檢查畫面。

不必接板子。每個案例把面板 HTML 寫成一個檔案，在面板的程式之前插入一段腳本，
把 fetch 換成回傳假資料，載入後執行檢查，結果寫進 <pre id="__r">，再用 --dump-dom 讀回來。
沒有讀到結果也算失敗。

    python3 -I tests/panel/test_panel.py              # 全部案例
    python3 -I tests/panel/test_panel.py find_toast   # 只跑指定的案例
    CHROME=/path/to/chrome python3 -I tests/panel/test_panel.py

需要 Chromium 系的瀏覽器（Chrome、Chromium、Brave、Edge），找不到時用 CHROME 指定。
假資料裡的位址、名稱都是虛構的，不要換成實際收到的資料。
"""
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAGE_SRC = Path(os.environ.get("PANEL_SRC", ROOT / "pocket_detector" / "panel.html"))

# 虛構的狀態與假的 fetch 放在 scenarios.py，網站的截圖與模擬面板用同一份
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scenarios import (BASE, FETCH_STUB, HIST_PATH, MEET_ALERT, MEET_BASE, PLACES, PROBES,  # noqa: E402
                       ROOM_IDLE, WIFI_ON)

# 每個案例：(覆寫的欄位, 分頁, 檢查)。檢查是一段 JS，回傳失敗原因的陣列，空陣列代表通過
CASES = {
    "scan_basic": ({}, "scan", """
        const f=[];
        if(!$('foot').textContent.includes('abc1234')) f.push('頁尾沒有版本號');
        if(!$('chips')) f.push('沒有狀態標籤');
        return f;"""),
    # 2026-11-27：iOS 點兩下或點內容會放大。每個元素都要 touch-action: manipulation
    # 2026-11-27：自動更新與自動發聲用了同一個 id，說明寫到別的列
    "unique_ids": ({}, "set", """
        const seen={},dup=[];
        document.querySelectorAll('[id]').forEach(e=>{if(seen[e.id])dup.push(e.id);seen[e.id]=1;});
        return dup.map(i=>'重複的 id：'+i);"""),
    "auto_sound_info": ({"automode": 0}, "set", """
        const t=$('autoInfo').textContent;
        return t.startsWith('只有手動按')?[]:['自動發聲的說明被蓋掉：'+t];"""),
    "touch_action": ({}, "set", """
        const bad=[...document.querySelectorAll('body *')].filter(e=>getComputedStyle(e).touchAction!='manipulation');
        return bad.slice(0,3).map(e=>'touch-action 不是 manipulation：'+e.tagName+(e.id?'#'+e.id:''));"""),
    # iOS 的輸入框字小於 16px 時，點進去會自動放大
    "inputs_16px": ({}, "set", """
        return [...document.querySelectorAll('input,textarea,select')].filter(e=>parseFloat(getComputedStyle(e).fontSize)<16)
          .map(e=>'字小於 16px：'+e.tagName+(e.id?'#'+e.id:''));"""),
    # 2026-11-27：統計模式的標籤出現兩次
    "stats_chip": ({"mode": "統計", "stats": True, "statsOn": True}, "scan", """
        const n=($('chips').innerText.match(/統計模式/g)||[]).length;
        return n>1?['統計模式的標籤出現 '+n+' 次']:[];"""),
    # 2026-11-27：淡出的提示蓋住「結束尋找」，按不下去
    "find_toast": ({"findOn": True, "findAddr": "02:00:00:00:00:01", "findType": "Apple", "findRssi": -60,
                    "findSeen": True}, "scan", """
        const f=[];
        const b=[...document.querySelectorAll('#find button')].find(x=>x.textContent=='結束尋找');
        if(!b) return ['找不到「結束尋找」'];
        if(!$('find').classList.contains('on')) f.push('尋找畫面沒有打開');
        toast('測試提示');
        const t=$('toast'), r=b.getBoundingClientRect();
        // 把提示整個疊在按鈕上，按鈕仍要按得到（提示要 pointer-events: none）
        Object.assign(t.style,{top:r.top+'px',left:r.left+'px',width:r.width+'px',height:r.height+'px',bottom:'auto',transform:'none',maxWidth:'none'});
        const x=document.elementFromPoint(r.left+r.width/2,r.top+r.height/2);
        if(x!==b&&!b.contains(x)) f.push('按鈕上方是 '+(x?x.tagName+'#'+x.id:'null')+'，按不到');
        return f;"""),
    "auto_off": ({}, "set", """
        const f=[];
        if($('sw_autoupd').getAttribute('aria-checked')!='false') f.push('開關應該是關的');
        if(!$('autoNow').hidden) f.push('關閉時不該有「檢查並安裝新版」');
        if($('armBtn').textContent!='開放更新'||$('armBox').hidden) f.push('關閉時要有「開放更新」');
        if(!$('armLbl').hidden) f.push('關閉時不該有「上傳檔案」的小標題');
        if(!$('armNote').textContent.includes('可以從網路更新或上傳')) f.push('說明要寫出開放後能做什麼：'+$('armNote').textContent);
        return f;"""),
    "auto_on": ({"autoupd": True, "autoNext": 17940, "autoFrom": "9999999", "netState": 5, "netLastAuto": True,
                 "netAgo": 420, "netRev": "abc1234"}, "set", """
        const f=[];
        if($('sw_autoupd').getAttribute('aria-checked')!='true') f.push('開關應該是開的');
        if($('autoNow').hidden) f.push('打開後要有「檢查並安裝新版」');
        if($('netNowBtn').disabled||$('netNowBtn').textContent!='檢查並安裝新版') f.push('「檢查並安裝新版」不對');
        // 自動更新開著時，BOOT 只剩上傳檔案要用，按鈕名稱跟著改
        if($('armBtn').textContent!='開放上傳'||$('armLbl').hidden) f.push('打開後要變成「上傳檔案」的「開放上傳」');
        const i=$('autoUpdInfo').textContent, m=$('netMsg').textContent;
        if(!i.includes('從 9999999 自動更新')) f.push('沒寫自動更新的來源：'+i);
        if(!i.includes('5 小時後')) f.push('下次檢查要用小時：'+i);
        if(!m.startsWith('自動檢查（7 分前）：已經是最新版')) f.push('結果訊息不對：'+m);
        if($('armBox').hidden) f.push('檢查完要再顯示「開放上傳」，才能上傳檔案');
        return f;"""),
    "auto_nonet": ({"autoupd": True, "autoNext": 1500, "netState": 7, "netLastAuto": True, "netAgo": 300}, "set", """
        const m=$('netMsg').textContent;
        const f=m.includes('附近沒有 Wi-Fi「TestNet」')?[]:['訊息不對：'+m];
        if($('netBar').style.width!='0%') f.push('沒有網路時進度條應該是 0');
        return f;"""),
    "auto_no_ssid": ({"netSsid": ""}, "set", """
        const f=[];
        if(!$('sw_autoupd').disabled) f.push('沒設定 Wi-Fi 時開關要停用');
        if(!$('autoUpdInfo').textContent.startsWith('先在上方設定')) f.push('要提示先設定 Wi-Fi');
        return f;"""),
    "auto_downloading": ({"autoupd": True, "netState": 3, "netGot": 600000, "netTotal": 1626896, "netRev": "def5678"},
                         "set", """
        const f=[], i=$('autoUpdInfo').textContent, m=$('netMsg').textContent;
        if(i.includes('下次檢查')) f.push('下載中不該顯示下次檢查：'+i);
        if(!m.startsWith('下載 def5678：586 / 1589 KB')) f.push('進度訊息不對：'+m);
        if(!$('netNowBtn').disabled) f.push('下載中要停用「檢查並安裝新版」');
        // 2026-11-27：進度條被「準備更新」與它的說明隔開，看不出提示屬於哪個按鈕
        if(!$('armBox').hidden) f.push('下載中要收起「開放上傳」');
        if($('netBox').parentNode.id!='netSlotIdle') f.push('進度要緊接在「檢查並安裝新版」下方');
        if($('netSlotIdle').previousElementSibling.id!='autoNow') f.push('進度的位置不在按鈕正下方');
        return f;"""),
    "test_channel": ({"testchan": True}, "set", """
        const f=[];
        if($('sw_testchan').getAttribute('aria-checked')!='true') f.push('接收測試版的開關應該是開的');
        if(!$('netHint').textContent.startsWith('測試通道')) f.push('要標明測試通道：'+$('netHint').textContent);
        return f;"""),
    "place_hidden": ({}, "set", """
        return $('placeBox').hidden?[]:['移動判斷關閉時不該顯示地點'];"""),
    "place_named": (WIFI_ON, "set", """
        const f=[];
        if($('placeBox').hidden) f.push('移動判斷開著時要顯示地點');
        if(!$('placeInfo').textContent.startsWith('目前在「家」')) f.push('目前地點不對：'+$('placeInfo').textContent);
        if($('placeSum').textContent!='存過的地點（2 / 32）') f.push('數量不對：'+$('placeSum').textContent);
        if(document.querySelectorAll('#placeList input').length!=2) f.push('清單應該有 2 個地點');
        if($('pn3').value!='家'||$('pn7').placeholder!='地點 #7（未命名）') f.push('名稱或預設文字不對');
        return f;"""),
    # 名稱放在 input 的 value 屬性裡，引號與角括號要跳脫
    "place_escape": (WIFI_ON, "set", """
        const i=$('pn7');
        return i&&i.value=='a"b<c>'?[]:['名稱沒有正確跳脫：'+(i?i.value:'找不到欄位')];""",
                     {"places": dict(PLACES, list=[PLACES["list"][0], {"id": 7, "name": 'a"b<c>', "visits": 1, "aps": 3}])}),
    "place_chip": (WIFI_ON, "scan", """
        const c=$('chips').innerText;
        if(c.includes('基地台')) return ['標籤不該重複寫基地台數，環境數字已經有了：'+c];
        return c.includes('停留中，家')?[]:['標籤沒有地點名稱：'+c];"""),
    "place_chip_unknown": (dict(WIFI_ON, placeId=0, placeName=""), "scan", """
        return $('chips').innerText.includes('還沒掃描到基地台')?[]:['還沒掃描時不該顯示地點 #0：'+$('chips').innerText];"""),
    "hist_places": (WIFI_ON, "hist", """
        const t=$('histList').innerText;
        return t.includes('經過的地點：家 → 地點 #5 → 公司')?[]:['歷史沒有經過的地點：'+t];""", {"hist": HIST_PATH}),
    # 掃描的節拍：倒數、掃描中、延後（房間檢查、更新時不掃描）
    "beat_countdown": ({}, "scan", """
        const f=[],b=$('beat').textContent,w=parseFloat($('beatBar').style.width);
        if(!/^\\d+ 秒後下一次$/.test(b)) f.push('倒數不對：'+b);
        if($('sweep').style.display!='none') f.push('沒在掃描時不該有掃過的光');
        if(!(w>0&&w<100)) f.push('進度條寬度不對：'+w);
        return f;"""),
    "beat_scanning": ({"scanAgo": 1000}, "scan", """
        const f=[];
        if($('beat').textContent!='正在掃描') f.push('掃描的 5 秒內要顯示正在掃描：'+$('beat').textContent);
        if($('sweep').style.display=='none') f.push('掃描中要有掃過的光');
        if(document.querySelectorAll('#radar .me').length!=1) f.push('圓心的點要剛好一個');
        if(document.querySelectorAll('#radar .dot').length!=2) f.push('雷達上的追蹤器數量不對');
        return f;"""),
    "beat_waiting": ({"scanAgo": 45000}, "scan", """
        return $('beat').textContent=='等待下一次掃描'?[]:['超過間隔還沒掃描時要顯示等待下一次掃描：'+$('beat').textContent];"""),
    "env_line": (WIFI_ON, "scan", """
        const t=$('env').textContent;
        return t=='上一次掃描收到藍牙裝置 43 個、Wi-Fi 基地台 12 個'?[]:['環境數字不對：'+t];"""),
    "env_no_wifi": ({}, "scan", """
        const t=$('env').textContent;
        return t=='上一次掃描收到藍牙裝置 43 個'?[]:['移動判斷關著時不該寫基地台：'+t];"""),
    "trend": ({}, "scan", """
        const f=[],rows=[...document.querySelectorAll('#trkList .trk')];
        const t0=rows[0].querySelector('.trend'),t1=rows[1].querySelector('.trend');
        if(!t0||!t0.textContent.includes('訊號變強')) f.push('第一個追蹤器應該是訊號變強：'+(t0&&t0.textContent));
        if(!t1||!t1.textContent.includes('訊號變弱')) f.push('第二個追蹤器應該是訊號變弱：'+(t1&&t1.textContent));
        if(t0&&t0.querySelector('polyline').getAttribute('points').split(' ').length!=6) f.push('折線的點數不對');
        return f;"""),
    "a11y_names_set": (dict(WIFI_ON, autoupd=True), "set", """
        // WCAG 4.1.2：每個可以操作的元件都要有螢幕閱讀器念得出來的名稱
        document.querySelectorAll('details').forEach(d=>d.open=true);
        const vis=e=>{const r=e.getBoundingClientRect();return r.width>0&&r.height>0;};
        const nm=e=>{if(e.getAttribute('aria-label'))return e.getAttribute('aria-label');const lb=e.getAttribute('aria-labelledby');
          if(lb)return lb.split(' ').map(i=>($(i)||{}).textContent||'').join(' ');return (e.textContent||'').trim();};
        const f=[...document.querySelectorAll('button,input,textarea,select,summary,[role=switch]')].filter(vis).filter(e=>!nm(e).trim())
          .map(e=>'沒有名稱：'+e.tagName+(e.id?'#'+e.id:''));
        document.querySelectorAll('.sw[role=switch]').forEach(e=>{if(!e.getAttribute('aria-labelledby'))f.push('開關沒有接到標題：'+e.id);});
        return f;"""),
    "a11y_names_mark": ({}, "mark", """
        // WCAG 4.1.2：每個可以操作的元件都要有螢幕閱讀器念得出來的名稱
        document.querySelectorAll('details').forEach(d=>d.open=true);
        const vis=e=>{const r=e.getBoundingClientRect();return r.width>0&&r.height>0;};
        const nm=e=>{if(e.getAttribute('aria-label'))return e.getAttribute('aria-label');const lb=e.getAttribute('aria-labelledby');
          if(lb)return lb.split(' ').map(i=>($(i)||{}).textContent||'').join(' ');return (e.textContent||'').trim();};
        const f=[...document.querySelectorAll('button,input,textarea,select,summary,[role=switch]')].filter(vis).filter(e=>!nm(e).trim())
          .map(e=>'沒有名稱：'+e.tagName+(e.id?'#'+e.id:''));
        document.querySelectorAll('.sw[role=switch]').forEach(e=>{if(!e.getAttribute('aria-labelledby'))f.push('開關沒有接到標題：'+e.id);});
        return f;"""),
    "a11y_names_room": ({}, "room", """
        // WCAG 4.1.2：每個可以操作的元件都要有螢幕閱讀器念得出來的名稱
        document.querySelectorAll('details').forEach(d=>d.open=true);
        const vis=e=>{const r=e.getBoundingClientRect();return r.width>0&&r.height>0;};
        const nm=e=>{if(e.getAttribute('aria-label'))return e.getAttribute('aria-label');const lb=e.getAttribute('aria-labelledby');
          if(lb)return lb.split(' ').map(i=>($(i)||{}).textContent||'').join(' ');return (e.textContent||'').trim();};
        const f=[...document.querySelectorAll('button,input,textarea,select,summary,[role=switch]')].filter(vis).filter(e=>!nm(e).trim())
          .map(e=>'沒有名稱：'+e.tagName+(e.id?'#'+e.id:''));
        document.querySelectorAll('.sw[role=switch]').forEach(e=>{if(!e.getAttribute('aria-labelledby'))f.push('開關沒有接到標題：'+e.id);});
        return f;"""),
    # WCAG 2.4.11：尋找畫面蓋住整頁，焦點要移進去，後面的頁面不能再被 Tab 到
    "a11y_find_focus": ({"findOn": True, "findAddr": "02:00:00:00:00:01", "findType": "Apple", "findRssi": -60,
                         "findSeen": True}, "scan", """
        const f=[];
        if(document.activeElement!==$('findEnd')) f.push('焦點沒有移到「結束尋找」：'+(document.activeElement&&document.activeElement.id));
        ['header','main','nav'].forEach(t=>{if(!document.querySelector('body>'+t).hasAttribute('inert')) f.push(t+' 沒有設成不能操作');});
        if($('find').getAttribute('aria-modal')!='true') f.push('尋找畫面要標成 aria-modal');
        return f;"""),
    # 固定在底部的分頁不能擋住聚焦的元件
    "a11y_scroll_padding": ({}, "set", """
        const v=parseFloat(getComputedStyle(document.documentElement).scrollPaddingBottom);
        return v>=72?[]:['頁面底部的捲動保留空間不夠：'+v];"""),
    # WCAG 4.1.3：警示要立刻讀出；剛打開面板時其他狀態畫面上就看得到，不另外讀
    "a11y_live_alert": ({}, "scan", """
        const f=[],t=$('srAlert').textContent;
        if(!t.startsWith('警示。有追蹤器一直跟著你')) f.push('警示沒有讀出：'+t);
        if($('srAlert').getAttribute('role')!='alert') f.push('警示的讀出區要是 role=alert');
        if($('sr').getAttribute('aria-live')!='polite') f.push('一般讀出區要是 aria-live=polite');
        if($('sr').getBoundingClientRect().width>1) f.push('讀出區要在畫面上隱藏');
        return f;"""),
    "a11y_live_quiet": ({"alert": False, "trk": [dict(t, alerted=False) for t in BASE["trk"]]}, "scan", """
        return ($('sr').textContent||$('srAlert').textContent)?['剛打開面板時不該讀出觀察中：'+$('sr').textContent]:[];"""),
    "mem_tls": ({"mem": [88064, 33792, 70000, 8126464, 51200, 2048]}, "set", """
        const t=$('memInfo').textContent;
        return t.endsWith('。上次網址更新的加密連線用了 PSRAM 50 KB、內部 2 KB')?[]:['沒有寫出加密連線用的記憶體：'+t];"""),
    "mem_line": ({}, "set", """
        const t=$('memInfo').textContent;
        return t=='記憶體：內部剩 86 KB（最大連續 33 KB，開機以來最低 68 KB），PSRAM 剩 7.8 MB'?[]:['記憶體那一行不對：'+t];"""),
    # 打開自動更新或接收測試版要短按 BOOT 確認，提示在那個開關下方，韌體更新那一列照常
    "confirm_autoupd": ({"otaState": 1, "bootWhat": 1, "otaLeft": 42}, "set", """
        const f=[];
        if($('cf_autoupd').hidden||!$('cf_autoupd').textContent.startsWith('請在 42 秒內短按一下偵測器的 BOOT 鍵確認')) f.push('自動更新下方沒有確認提示：'+$('cf_autoupd').textContent);
        if(!$('cf_testchan').hidden) f.push('接收測試版下方不該有提示');
        if($('otaIdle').hidden||!$('otaWait').hidden) f.push('確認設定時，韌體更新那一列要照常顯示');
        return f;"""),
    "confirm_testchan": ({"otaState": 1, "bootWhat": 2, "otaLeft": 30}, "set", """
        return !$('cf_testchan').hidden&&$('cf_autoupd').hidden?[]:['接收測試版的確認提示不對'];"""),
    # WCAG 2.2.2：倒數可以暫停，再按一次繼續
    "a11y_beat_pause": ({"scanAgo": 1000}, "scan", """
        const f=[],b=$('beat');
        if(b.tagName!='BUTTON') f.push('倒數要是按鈕');
        if(b.getBoundingClientRect().height<24) f.push('倒數按鈕小於 24px');
        toggleBeat();
        if(b.textContent!='倒數已暫停'||b.getAttribute('aria-pressed')!='true') f.push('暫停後的文字或狀態不對：'+b.textContent);
        if($('sweep').style.display!='none') f.push('暫停時不該有掃過的光');
        toggleBeat();
        if(b.textContent=='倒數已暫停'||b.getAttribute('aria-pressed')!='false') f.push('再按一次要繼續倒數');
        return f;"""),
    # WCAG 1.4.1：時間軸不只靠紅色標出警示的追蹤器
    "a11y_timeline_label": ({}, "hist", """
        const t=[...document.querySelectorAll('#timeline svg text')].map(e=>e.textContent);
        return t.some(x=>x.startsWith('警示 Apple'))?[]:['時間軸的警示追蹤器沒有文字標示：'+t.join('|')];""", {"hist": HIST_PATH}),
    # 分頁：方向鍵、Home、End 切換，只有目前的分頁可以用 Tab 到
    "a11y_tabs_keys": ({}, "scan", """
        const f=[],nav=$('tab-scan').parentNode,key=k=>nav.dispatchEvent(new KeyboardEvent('keydown',{key:k,bubbles:true}));
        $('tab-scan').focus();key('ArrowRight');
        if(document.activeElement.id!='tab-hist'||$('t-hist').hidden) f.push('右方向鍵沒有切到歷史');
        if($('tab-hist').tabIndex!==0||$('tab-scan').tabIndex!==-1) f.push('只有目前的分頁可以用 Tab 到');
        key('End');if(document.activeElement.id!='tab-set') f.push('End 沒有切到設定');
        key('ArrowRight');if(document.activeElement.id!='tab-scan') f.push('最後一個再往右要回到第一個');
        key('ArrowLeft');if(document.activeElement.id!='tab-set') f.push('第一個往左要到最後一個');
        key('Home');if(document.activeElement.id!='tab-scan') f.push('Home 沒有切到偵測');
        if($('t-hist').getAttribute('role')!='tabpanel'||$('tab-hist').getAttribute('aria-controls')!='t-hist') f.push('分頁與內容沒有對應起來');
        return f;"""),
    # 會議室檢查
    "meeting_idle": ({}, "room", """
        const f=[];
        if($('meetBtn').textContent!='開始檢查') f.push('按鈕文字不對');
        if(document.querySelectorAll('#seg_meet button').length!=3) f.push('範圍要有三個選項');
        if($('seg_meet').querySelector('[aria-pressed=true]').textContent!='中，-70 dBm') f.push('預設範圍要是中');
        return f;"""),
    "meeting_baseline": ({"meetPhase": 1, "meetVer": 2, "every": 10}, "room", """
        const f=[],t=$('meetSum').textContent;
        if(!t.startsWith('建立基準中，剩下 40 秒。目前收到 3 個訊號強於 -70 dBm')) f.push('基準的說明不對：'+t);
        if(!$('seg_meet').querySelector('button').disabled) f.push('檢查中不能換範圍');
        if($('meetBtn').textContent!='結束檢查') f.push('檢查中要能結束');
        return f;""", {"meeting": MEET_BASE}),
    "meeting_alert": ({"meetPhase": 2, "meetVer": 9, "meetAlert": True, "every": 10}, "room", """
        const f=[],t=$('meetSum').textContent,rows=[...document.querySelectorAll('#meetList .trk')];
        if(!t.startsWith('多了 1 個藍牙裝置，估計 1 種裝置在找 Wi-Fi')) f.push('提醒不對：'+t);
        if(!t.includes('最近每分鐘 1.5 個（基準 0.0 個）')) f.push('Wi-Fi 的說明不對：'+t);
        if(rows.length!=3) f.push('應該有 2 個藍牙裝置、1 組 Wi-Fi：'+rows.length);
        else {
          const r0=rows[0].textContent,r1=rows[1].textContent,r2=rows[2].textContent;
          if(rows[0].querySelector('.name').textContent!='iPhone 或 iPad（有行動網路）（估計）') f.push('類型不對：'+rows[0].querySelector('.name').textContent);
          if(!r0.includes('廣播的名稱：Test <的> iPhone')) f.push('廣播的名稱沒有顯示或沒有跳脫');
          if(!r0.includes('正在使用')||!r0.includes('可以分享行動網路')||!r0.includes('狀態：螢幕亮著（估計）')) f.push('使用中的跡象不對：'+r0);
          if(!rows[0].querySelector('.trend')) f.push('沒有訊號的變化');
          if(rows[0].querySelector('[data-mf]').dataset.mf!='7') f.push('尋找按鈕的編號不對');
          if(!r1.includes('已經離開')||rows[1].querySelector('[data-mf]')) f.push('離開的裝置要標出來，不能尋找');
          if(!r2.includes('samsung SM-S911B')||!r2.includes('裝置名稱：測試的 S23')||!r2.includes('4 個請求')) f.push('Wi-Fi 的分組不對：'+r2);
        }
        if(document.querySelectorAll('#meetList svg.tl rect').length!=3) f.push('時間軸要有 3 列');
        if(!$('chips').innerText.includes('會議室檢查：有多出來的裝置')) f.push('頂端沒有提醒');
        if(!$('sr').textContent.startsWith('會議室檢查：多了 1 個藍牙裝置')) f.push('螢幕閱讀器沒有讀出提醒：'+$('sr').textContent);
        const why=[...document.querySelectorAll('#meetSum button')].find(b=>b.textContent.includes('一直在找 Wi-Fi'));
        if(!why) f.push('近距離有探測請求時，要能打開原因的說明');
        else { why.click(); if($('probeWhy').hidden||!$('probeWhy').open) f.push('按了沒有打開說明'); }
        return f;""", {"meeting": MEET_ALERT}),
    # 只有 Wi-Fi 的動靜：黃色的留意，不是紅色的提醒
    "meeting_caution": ({"meetPhase": 2, "meetVer": 4, "meetCaution": True, "every": 10}, "room", """
        const f=[],b=$('meetSum').querySelector('b');
        if(!b||!b.textContent.startsWith('留意：近距離有裝置在找 Wi-Fi')) f.push('留意的說明不對：'+(b&&b.textContent));
        if(b&&getComputedStyle(b).color==getComputedStyle(document.body).getPropertyValue('--alert')) f.push('留意不該用紅色');
        const c=$('chips').innerText;
        if(!c.includes('會議室檢查：留意 Wi-Fi 的動靜')||c.includes('有多出來的裝置')) f.push('頂端的標籤不對：'+c);
        return f;""", {"meeting": dict(MEET_ALERT, new=0, lvl=1, devs=[], groups=1)}),
    # 周圍手機洩漏的 Wi-Fi 名稱
    "probes_idle": ({}, "room", """
        const f=[];
        if($('probeBtn').textContent!='開始監聽') f.push('按鈕文字不對：'+$('probeBtn').textContent);
        if($('probeSum').textContent) f.push('還沒監聽時不該有統計');
        if(!$('probeWhy').hidden) f.push('還沒有結果時不該顯示原因的說明');
        return f;"""),
    "probes_running": ({"probeOn": True, "probeVer": 3}, "room", """
        const f=[],t=$('probeSum').textContent,rows=[...document.querySelectorAll('#probeList .trk')];
        if($('probeBtn').textContent!='停止監聽') f.push('監聽中要顯示停止監聽');
        if(!t.includes('收到 523 個探測請求，來自 87 個位址（其中 12 個是固定位址')) f.push('統計不對：'+t);
        if(!t.includes('41 個請求帶出了 Wi-Fi 名稱，共 2 個不同的名稱')) f.push('名稱的統計不對：'+t);
        if(rows.length!=2) f.push('名稱應該有 2 列');
        // 先看到名稱，緊接著是為什麼會送出、怎麼關掉
        if($('probeWhy').hidden) f.push('有結果時要顯示原因的說明');
        if($('probeWhy').previousElementSibling.id!='probeList') f.push('說明要放在名稱清單的正下方');
        if(!$('probeWhy').textContent.includes('私人 Wi-Fi 位址')) f.push('說明少了怎麼關掉');
        else {
          if(rows[1].querySelector('.name').textContent!='Home "5G" <b>') f.push('名稱沒有正確跳脫：'+rows[1].querySelector('.name').innerHTML);
          if(!rows[1].textContent.includes('有固定位址的手機送出')) f.push('固定位址沒有標出來');
        }
        return f;""", {"probes": PROBES}),
    "ota_wait": ({"otaState": 1, "otaLeft": 42}, "set", """
        const f=[];
        if($('otaWait').hidden) f.push('等待 BOOT 的畫面沒出現');
        if($('otaWaitLeft').textContent!='42') f.push('倒數不對');
        if(!$('otaIdle').hidden) f.push('等待時不該顯示「開放更新」');
        return f;"""),
    "ota_ready": ({"otaState": 2, "otaLeft": 280}, "set", """
        const f=[];
        if($('otaReady').hidden) f.push('開放更新的畫面沒出現');
        if($('netBtn').disabled||$('netBtn').textContent!='檢查並安裝新版') f.push('「檢查並安裝新版」不對');
        if($('netBox').parentNode.id!='netSlotReady') f.push('開放更新時，進度要在「檢查並安裝新版」下方');
        return f;"""),
}

INJECT = """<script>
""" + FETCH_STUB + """window.__errs=[];window.addEventListener('error',e=>window.__errs.push(String(e.message)));
try{localStorage.clear();localStorage.tab=%(tab)s}catch(e){}
window.addEventListener('load',()=>setTimeout(()=>{let f;try{f=(function(){%(check)s})();}catch(e){f=['檢查本身出錯：'+e.message];}
  f=f.concat(window.__errs.map(m=>'JS 錯誤：'+m));const p=document.createElement('pre');p.id='__r';p.textContent=JSON.stringify(f);document.body.appendChild(p);},1200));
</script>
"""


def find_browser():
    cands = [os.environ.get("CHROME"), "google-chrome", "chromium", "chromium-browser",
             "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
             "/Applications/Chromium.app/Contents/MacOS/Chromium",
             "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
             "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"]
    for c in cands:
        if c and (shutil.which(c) or Path(c).exists()):
            return shutil.which(c) or c
    sys.exit("找不到 Chromium 系的瀏覽器，用 CHROME=路徑 指定")


def page_html():
    src = PAGE_SRC.read_text(encoding="utf-8")
    if PAGE_SRC.suffix == ".html":
        return src
    m = re.search(r'R"HTML\((.*?)\)HTML"', src, re.S)  # 舊版把網頁放在 webpanel.h
    if not m:
        sys.exit(f"{PAGE_SRC} 裡找不到面板的 HTML")
    return m.group(1)


def run_case(browser, page, name, tmp):
    over, tab, check, *extra = CASES[name]
    extra = extra[0] if extra else {}
    fx = dict(BASE, **over)
    inj = INJECT % {"tab": json.dumps(tab), "fx": json.dumps(fx, ensure_ascii=False),
                    "rx": json.dumps(ROOM_IDLE), "check": check,
                    "hx": json.dumps(extra.get("hist", {}), ensure_ascii=False),
                    "px": json.dumps(extra.get("places", PLACES), ensure_ascii=False),
                    "qx": json.dumps(extra.get("probes", {"ver": 0}), ensure_ascii=False),
                    "mx": json.dumps(extra.get("meeting", {"ver": 0}), ensure_ascii=False)}
    marker = "<script>\nconst $"
    if marker not in page:
        return [f"面板 HTML 裡找不到插入點 {marker!r}"]
    p = tmp / f"{name}.html"
    p.write_text(page.replace(marker, inj + marker, 1), encoding="utf-8")
    try:
        r = subprocess.run([browser, "--headless=new", "--disable-gpu", "--no-sandbox", "--window-size=390,844",
                            f"--user-data-dir={tmp / 'profile'}", "--virtual-time-budget=5000", "--dump-dom",
                            p.as_uri()], capture_output=True, text=True, timeout=90)
    except subprocess.TimeoutExpired:
        return ["瀏覽器逾時"]
    m = re.search(r'<pre id="__r">(.*?)</pre>', r.stdout, re.S)
    if not m:
        return ["沒有讀到檢查結果（頁面可能沒載入完成）"]
    return json.loads(html.unescape(m.group(1)))


def main(names):
    unknown = [n for n in names if n not in CASES]
    if unknown:
        sys.exit("沒有這些案例：" + "、".join(unknown))
    browser, page = find_browser(), page_html()
    failed = 0
    with tempfile.TemporaryDirectory() as d:
        for name in names or list(CASES):
            fails = run_case(browser, page, name, Path(d))
            print(("通過 " if not fails else "失敗 ") + name)
            for f in fails:
                print("    " + f)
            failed += bool(fails)
    total = len(names or CASES)
    print(f"面板測試：{total - failed} / {total} 通過")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
