// 控制頁的 HTML，獨立成標頭檔，避免 Arduino 的前處理把裡面的 JavaScript 當成函式
#pragma once
#include <Arduino.h>

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 能力展示</title>
<style>
:root{--bg:#f6f7f9;--fg:#1d2330;--card:#fff;--mute:#6b7280;--acc:#2563eb;--bd:#e5e7eb}
@media (prefers-color-scheme:dark){:root{--bg:#111418;--fg:#e6e8eb;--card:#1b1f26;--mute:#9aa3af;--acc:#60a5fa;--bd:#2b313b}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,"PingFang TC","Noto Sans TC",sans-serif}
main{max-width:720px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 12px}h2{font-size:16px;margin:0 0 8px}
section{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:14px;margin:0 0 12px}
button{background:var(--acc);color:#fff;border:0;border-radius:8px;padding:8px 14px;font-size:14px;margin:2px 4px 2px 0}
input,select{font-size:14px;padding:7px;border:1px solid var(--bd);border-radius:8px;background:var(--bg);color:var(--fg);max-width:100%}
table{width:100%;border-collapse:collapse;font-size:13px}td,th{text-align:left;padding:3px 6px;border-bottom:1px solid var(--bd);vertical-align:top}
.mute{color:var(--mute);font-size:13px}.big{font-size:26px;font-weight:600}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:6px 12px}
.bar{height:8px;background:var(--bd);border-radius:4px;overflow:hidden}.bar i{display:block;height:100%;background:var(--acc)}
pre{white-space:pre-wrap;font-size:12px;margin:0;max-height:240px;overflow:auto}
.scroll{max-height:320px;overflow:auto}
</style></head><body><main>
<h1>ESP32-S3 能力展示</h1>
<p class="mute">在小視窗裡下載與上傳可能受限，請改用瀏覽器開啟 <b>http://192.168.4.1</b>。</p>

<section><h2>晶片資訊</h2><div id="info" class="grid">讀取中…</div></section>

<section><h2>RGB LED</h2>
<input type="color" id="color" value="#2040ff"> <button onclick="led(0)">關燈</button>
<p class="mute">板上的 WS2812，GPIO48（部分板子是 38）。</p></section>

<section><h2>觸控腳位</h2>
<button id="tbtn" onclick="toggleTouch()">開始讀取</button>
<div id="touch" class="grid"></div>
<p class="mute">用手指碰擴展板上對應 GPIO 的 S 針，數值會明顯上升。</p></section>

<section><h2>傳輸速度</h2>
<select id="kb"><option value="512">512 KB</option><option value="2048" selected>2 MB</option><option value="8192">8 MB</option></select>
<button onclick="speed()">測下載速度</button>
<div id="speed" class="big"></div><div id="speedNote" class="mute"></div></section>

<section><h2>Wi-Fi 掃描</h2><button onclick="wscan()">掃描</button>
<div id="wscan" class="scroll"></div></section>

<section><h2>BLE 掃描（3 秒）</h2><button onclick="bscan()">掃描</button>
<div id="bscan" class="scroll"></div></section>

<section><h2>運算效能</h2>
<button id="bbtn" onclick="runBench()">開始測試（約 10 秒）</button>
<div id="bench" class="scroll"></div>
<p class="mute">測試期間控制頁與熱點會暫停回應，屬正常現象。</p></section>

<section><h2>連上外部 Wi-Fi</h2>
<p class="mute">從網址更新與定期檢查需要板子能連上網路，熱點會同時保持開啟。</p>
<input id="ssid" placeholder="網路名稱"> <input id="pass" type="password" placeholder="密碼">
<button onclick="joinWifi()">連線</button> <button onclick="forgetWifi()">忘記</button>
<div id="wifiMsg" class="mute"></div></section>

<section><h2>韌體更新（OTA）</h2>
<p>目前版本 <b id="build">-</b>，執行中的分區 <b id="part">-</b></p>
<h2>上傳檔案</h2>
<input type="file" id="fw" accept=".bin"> <button onclick="upload()">上傳並更新</button>
<div class="bar"><i id="upbar" style="width:0"></i></div><div id="upMsg" class="mute"></div>
<h2 style="margin-top:12px">從網址拉</h2>
<input id="pullUrl" size="40" placeholder="http://192.168.x.x:8000/firmware.bin"> <button onclick="pull()">下載並更新</button>
<div id="pullMsg" class="mute"></div>
<h2 style="margin-top:12px">定期檢查新版</h2>
<input id="autoUrl" size="40" placeholder="http://192.168.x.x:8000/manifest.txt">
<input id="autoMin" type="number" min="0" style="width:80px" placeholder="分鐘"> 分鐘
<button onclick="saveAuto()">儲存</button> <button onclick="checkNow()">立刻檢查</button>
<p class="mute">manifest.txt 兩行：<code>build=2</code> 與 <code>url=http://…/firmware.bin</code>。build 比目前版本大就更新，分鐘填 0 停用。</p>
<div id="autoMsg" class="mute"></div></section>

<section><h2>事件紀錄</h2><pre id="log"></pre></section>
</main>
<script>
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const fmtB=b=>b>=1048576?(b/1048576).toFixed(1)+' MB':(b/1024).toFixed(0)+' KB';
let filled=false;
async function info(){
  try{
    const r=await (await fetch('/api/info',{cache:'no-store'})).json();
    const rows=[['晶片',r.model+' rev '+r.rev],['核心',r.cores+' 核 '+r.mhz+' MHz'],['溫度',r.temp.toFixed(1)+' °C'],
      ['Flash',fmtB(r.flash)],['PSRAM',fmtB(r.psramFree)+' / '+fmtB(r.psram)],['可用記憶體',fmtB(r.heap)],
      ['程式大小',fmtB(r.sketch)+' / '+fmtB(r.sketch+r.sketchFree)],['開機時間',Math.floor(r.uptime/60)+' 分 '+(r.uptime%60)+' 秒'],
      ['熱點',r.ap+'，'+r.apClients+' 台連線'],['外部 Wi-Fi',r.sta?r.sta+' '+r.staIp+' '+r.staRssi+' dBm':'未連線'],['MAC',r.mac],['藍牙',r.ble?(r.bleSecure?'手機已配對連線':'手機已連線，等待配對'):'等待連線'],['藍牙配對碼',r.blePin]];
    $('info').innerHTML=rows.map(x=>'<div><div class="mute">'+x[0]+'</div>'+esc(x[1])+'</div>').join('');
    $('build').textContent=r.build;$('part').textContent=r.part;
    $('log').textContent=r.log.join('\n');
    if(!filled){$('autoUrl').value=r.autoUrl;$('autoMin').value=r.autoMin;filled=true;}
  }catch(e){}
}
setInterval(info,2000);info();
function led(on){const c=$('color').value;const q=on?`r=${parseInt(c.substr(1,2),16)}&g=${parseInt(c.substr(3,2),16)}&b=${parseInt(c.substr(5,2),16)}`:'r=0&g=0&b=0';fetch('/api/led?'+q);}
$('color').addEventListener('input',()=>led(1));
let tt=null,base={};
async function readTouch(){
  const r=await (await fetch('/api/touch',{cache:'no-store'})).json();
  $('touch').innerHTML=r.map(x=>{if(!(x.pin in base))base[x.pin]=x.v;const d=x.v-base[x.pin];const hot=d>base[x.pin]*0.2;
    return '<div><div class="mute">GPIO'+x.pin+'</div><span style="font-weight:'+(hot?700:400)+';color:'+(hot?'var(--acc)':'inherit')+'">'+x.v+'</span></div>';}).join('');
}
function toggleTouch(){if(tt){clearInterval(tt);tt=null;$('tbtn').textContent='開始讀取';}else{base={};readTouch();tt=setInterval(readTouch,300);$('tbtn').textContent='停止';}}
async function speed(){
  const kb=$('kb').value;$('speed').textContent='測試中…';$('speedNote').textContent='';
  const t0=performance.now();const res=await fetch('/speed?kb='+kb+'&t='+Date.now(),{cache:'no-store'});
  const rd=res.body.getReader();let n=0;
  for(;;){const {done,value}=await rd.read();if(done)break;n+=value.length;
    const s=(performance.now()-t0)/1000;$('speed').textContent=(n*8/s/1e6).toFixed(2)+' Mbit/s';}
  const s=(performance.now()-t0)/1000;
  $('speed').textContent=(n*8/s/1e6).toFixed(2)+' Mbit/s';
  $('speedNote').textContent=fmtB(n)+'，'+s.toFixed(1)+' 秒，約 '+(n/s/1024).toFixed(0)+' KB/s';
}
async function runBench(){
  $('bbtn').disabled=true;$('bench').textContent='測試中…';
  try{
    const r=await (await fetch('/api/bench',{cache:'no-store'})).json();
    const rows=[['時脈',r.mhz+' MHz',''],
      ['整數運算',r.intMps+' 百萬次迴圈/秒','每次迴圈約 '+r.intNs+' ns（乘、加、位移、XOR、加）'],
      ['單精度浮點',r.f32+' MFLOPS','64×64 矩陣乘法，有硬體 FPU'],
      ['雙精度浮點',r.f64+' MFLOPS','48×48 矩陣乘法，靠軟體模擬'],
      ['SHA-256',r.sha+' MB/s','硬體加速'],
      ['AES-128-CTR',r.aes+' MB/s','硬體加速'],
      ['內部 SRAM 搬移',r.sram+' MB/s','memcpy 32 KB'],
      ['PSRAM 搬移',r.psram+' MB/s','memcpy 512 KB'],
      ['雙核心加速',r.dual+' 倍','兩核同時跑整數運算，理想值 2 倍'],
      ['int16 矩陣乘法 64×64',r.mAnsi+' → '+r.mSimd+' ms','一般 C → 向量指令，快 '+(r.mAnsi/r.mSimd).toFixed(1)+' 倍'],
      ['FFT 1024 點',r.fAnsi+' → '+r.fSimd+' ms','一般 C → S3 最佳化版本，快 '+(r.fAnsi/r.fSimd).toFixed(1)+' 倍']];
    $('bench').innerHTML='<table><tr><th>項目</th><th>結果</th><th>說明</th></tr>'+rows.map(x=>'<tr><td>'+x[0]+'</td><td><b>'+x[1]+'</b></td><td class="mute">'+x[2]+'</td></tr>').join('')+'</table>';
  }catch(e){$('bench').textContent='測試失敗：'+e;}
  $('bbtn').disabled=false;
}
async function wscan(){
  $('wscan').textContent='掃描中…';
  const r=await (await fetch('/api/wifiscan',{cache:'no-store'})).json();
  $('wscan').innerHTML='<p class="mute">'+r.length+' 個基地台</p><table><tr><th>名稱</th><th>訊號</th><th>頻道</th><th>加密</th></tr>'+
    r.map(x=>'<tr><td>'+(esc(x.ssid)||'<span class="mute">（隱藏）</span>')+'</td><td>'+x.rssi+'</td><td>'+x.ch+'</td><td>'+x.enc+'</td></tr>').join('')+'</table>';
}
async function bscan(){
  $('bscan').textContent='掃描中…';
  const r=await (await fetch('/api/blescan',{cache:'no-store'})).json();
  r.sort((a,b)=>b.rssi-a.rssi);
  $('bscan').innerHTML='<p class="mute">'+r.length+' 個裝置</p><table><tr><th>位址</th><th>訊號</th><th>名稱</th><th>廠商資料</th></tr>'+
    r.map(x=>'<tr><td>'+x.addr+'</td><td>'+x.rssi+'</td><td>'+esc(x.name)+'</td><td>'+(x.mfg?x.mfg.substr(0,4)+'…':'')+'</td></tr>').join('')+'</table>';
}
async function post(url,data){const f=new URLSearchParams(data);return (await fetch(url,{method:'POST',body:f})).text();}
async function joinWifi(){$('wifiMsg').textContent=await post('/api/wifi',{ssid:$('ssid').value,pass:$('pass').value});}
async function forgetWifi(){$('wifiMsg').textContent=await post('/api/wifi',{ssid:'',pass:''});}
function upload(){
  const f=$('fw').files[0];if(!f){$('upMsg').textContent='先選擇 .bin 檔';return;}
  const fd=new FormData();fd.append('fw',f,f.name);
  const x=new XMLHttpRequest();x.open('POST','/update');
  x.upload.onprogress=e=>{if(e.lengthComputable){$('upbar').style.width=(e.loaded/e.total*100)+'%';$('upMsg').textContent=fmtB(e.loaded)+' / '+fmtB(e.total);}};
  x.onload=()=>{$('upMsg').textContent=x.responseText;};x.onerror=()=>{$('upMsg').textContent='連線中斷，板子可能已經重新開機';};
  x.send(fd);
}
async function pull(){$('pullMsg').textContent='下載中，完成後板子會重新開機…';$('pullMsg').textContent=await post('/api/pull',{url:$('pullUrl').value});}
async function saveAuto(){$('autoMsg').textContent=await post('/api/auto',{url:$('autoUrl').value,min:$('autoMin').value||'0'});}
async function checkNow(){$('autoMsg').textContent='檢查中…';$('autoMsg').textContent=await post('/api/check',{});}
</script></body></html>)HTML";
