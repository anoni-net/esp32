#!/usr/bin/env python3
"""假的板子：開一個 pty，照韌體的序列埠格式回應指令，不接板子也能檢查 run_hil.py 的解析。

    python3 -I tests/hil/fake_board.py            # 印出 pty 的路徑後持續執行
    python3 -I tests/hil/run_hil.py --port <路徑> --reset-cmd __reset --long

只模擬 run_hil.py 用到的指令，內容都是虛構的。BROKEN=項目 讓某一項故意回錯，確認測試會失敗，
例如 BROKEN=history、BROKEN=json、BROKEN=room。
"""
import json
import os
import pty
import sys
import time
import tty

BUILD = os.environ.get("FAKE_BUILD", "Jan  1 2000 00:00:00")
NET = os.environ.get("FAKE_NET", "")  # latest：模擬設定好 Wi-Fi、已經是最新版
BROKEN = set(filter(None, os.environ.get("BROKEN", "").split(",")))
st = {"part": "app0", "fake": False, "fake_n": 0, "preset": 1, "marks": [], "prev": [], "testchan": False}


def out(fd, line):
    os.write(fd, (line + "\r\n").encode())


def boot(fd):
    for line in ["ESP-ROM:esp32s3-20210327", "HIST,0,archived,2,512", f"BUILD,{BUILD},{st['part']},abc1234",
                 "長按 BOOT 2 秒開啟手機連線"]:
        out(fd, line)
        time.sleep(0.05)


def status():
    trk = []
    if st["fake"]:
        st["fake_n"] += 1
        trk.append({"addr": "fa:ke:00:00:00:01", "type": "Apple", "rssi": -60, "alerted": st["preset"] == 3 and st["fake_n"] > 2})
    j = {"scan": 10, "trk": trk, "build": BUILD, "rev": "abc1234", "netState": 0, "autoupd": False,
         "testchan": st["testchan"], "roomState": 0, "interval": 1, "duration": 1, "preset": st["preset"]}
    if "json" in BROKEN:
        del j["testchan"]
    return json.dumps(j, ensure_ascii=False)


def handle(fd, cmd):
    if cmd == "__reset":
        st["prev"] = st["marks"] + st["prev"]
        st["marks"] = []
        boot(fd)
    elif cmd == "json":
        out(fd, status())
    elif cmd == "channel test":
        st["testchan"] = True
        out(fd, "OTA,10,channel_test")
    elif cmd == "channel":
        out(fd, "更新通道：%s（https://assets.anoni.net/esp32-tracker%s/）" % (("測試", "-test") if st["testchan"] else ("正式", "")))
    elif cmd == "meeting start -70":
        out(fd, "MEETING,10,start,-70")
    elif cmd == "meeting":
        out(fd, "會議室檢查：建立基準中，門檻 -70 dBm，基準 2 個，多出 0 個，近距離探測請求每分鐘 0.0 個（基準 0.0）")
    elif cmd == "meeting stop":
        out(fd, "MEETING,10,done,0,-70,0,0")
    elif cmd == "probes start":
        st["probe"] = True
        out(fd, "PROBE,10,start,6")
    elif cmd == "probes":
        out(fd, "探測請求：%s，頻道 6，請求 40 個，來源 12 個（固定位址 2 個），帶名稱 5 個，不同的名稱 3 個" % ("監聽中" if st.get("probe") else "沒在監聽"))
    elif cmd == "probes stop":
        st["probe"] = False
        out(fd, "PROBE,10,done,20,6,40,12,2,5,3,0")
    elif cmd == "mem":
        out(fd, "MEM,10,cmd,88064,33792,70000,8126464,8126464,2,5,1")
    elif cmd == "places":
        out(fd, "目前在地點 #3，存了 1 個地點（最多 32 個）")
        out(fd, "  #3 （未命名），來過 2 次，指紋 24 個基地台")
    elif cmd == "autoupdate":
        out(fd, "自動更新：關，更新用的 Wi-Fi：（未設定），下次檢查約 -1 秒後")
    elif cmd == "netupdate":
        if not NET:
            out(fd, "先用 netwifi 設定可以上網的 Wi-Fi")
            return
        test = st["testchan"] and "channel" not in BROKEN
        out(fd, "OTA,10,net_start,https://assets.anoni.net/esp32-tracker%s/" % ("-test" if test else ""))
        time.sleep(0.3)
        out(fd, "OTA,11,net_latest,已經是最新版")
    elif cmd in ("autoupdate on", "autoupdate off"):
        out(fd, "OTA,10,auto_%s" % cmd[11:])
    elif cmd == "autoupdate now":
        if "autodefer" not in BROKEN:
            time.sleep(0.3)
            out(fd, "OTA,12,auto_latest,已經是最新版" if NET else "OTA,12,auto_nonet,附近沒有 Wi-Fi「TestNet」")
    elif cmd == "oled dump":
        out(fd, "OLEDBUF," + "00" * (1000 if "oled" in BROKEN else 1024))
    elif cmd.startswith("mark "):
        st["marks"].append(cmd[5:])
        out(fd, f"MARK,1,10,1000,{cmd[5:]}")
    elif cmd == "history":
        marks = [] if "history" in BROKEN else st["marks"]
        out(fd, json.dumps({"marks": [{"n": i + 1, "label": m} for i, m in enumerate(marks)]}, ensure_ascii=False))
    elif cmd == "history prev":
        out(fd, json.dumps([{"marks": [{"label": m} for m in st["prev"]]}], ensure_ascii=False))
    elif cmd in ("fake on", "fake off"):
        st["fake"], st["fake_n"] = cmd == "fake on", 0
        out(fd, f"FAKE,10,{cmd[5:]},fa:ke:00:00:00:01")
    elif cmd.startswith("preset "):
        st["preset"] = int(cmd[7:])
    elif cmd == "ack":
        out(fd, "已確認警示")
    elif cmd == "room":
        out(fd, "ROOM,11,ble")
        time.sleep(0.5)
        out(fd, "ROOM,12,%s,4,3,7,0,1,122" % ("cancelled" if "room" in BROKEN else "done"))
    elif cmd == "room json":
        out(fd, '{"ver":1,"state":"done","items":[]}')
    elif cmd == "ota selftest":
        out(fd, "OTA 自我測試：從 %s 複製 1630624 bytes" % st["part"])
        st["part"] = "app1" if st["part"] == "app0" else "app0"
        time.sleep(0.3)
        boot(fd)


def main():
    master, slave = pty.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    buf = b""
    while True:
        try:
            d = os.read(master, 1024)
        except OSError:
            time.sleep(0.05)
            continue
        buf += d
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            handle(master, raw.decode("utf-8", "replace").strip())


if __name__ == "__main__":
    sys.exit(main())
