#!/usr/bin/env python3
"""實機測試：板子接在電腦上，經由序列埠送指令、檢查輸出。

    python3 -I tests/hil/run_hil.py --port /dev/cu.usbmodemXXXX            # 基本項目，約 2 分鐘
    python3 -I tests/hil/run_hil.py --port /dev/cu.usbmodemXXXX --update   # 先改接收測試版、從網址更新
    python3 -I tests/hil/run_hil.py --port /dev/cu.usbmodemXXXX --long     # 加上房間檢查、假警示、OTA 自我測試，約 10 分鐘

要用 PlatformIO 的 Python（有 pyserial），例如 ~/.local/share/uv/tools/platformio/bin/python。

注意：
- 一定要指定 --port，不會自己挑。打開序列埠會讓板子重新開機，另一個程式佔著的板子（例如正在記錄的
  那塊）不要拿來測
- 不做破壞性的操作：不恢復原廠設定、不刪記錄。會改的設定（門檻）測完改回原樣。--update 會讓板子改成
  接收測試版，這是開發用的板子該有的設定，不會改回去
- 打記號與假追蹤器會留在板子的警示歷史與 flash 記錄裡，標籤是 HIL- 開頭、位址是 fa:ke:00:00:00:01
- 序列埠的完整輸出寫在 tests/hil/results/（不進版控，裡面有附近裝置的位址）
- 沒有 Wi-Fi 時，網路相關的項目記成略過，不算通過

全部沒有失敗時，把板子開機印出的建置時間寫進 tests/hil/results/passed.tsv。tools/promote_firmware.sh
只推這個檔案裡有的建置時間，所以要用 --update 讓板子從測試通道裝上要推的那個版本再測。
"""
import argparse
import json
import re
import sys
import threading
import time
import urllib.request
from datetime import datetime
from pathlib import Path

RESULTS = Path(__file__).resolve().parent / "results"
# 各語系的韌體從自己的通道更新（publish*.env、pocket_detector.ino 的 NETUPDATE_URL_*）。
# 板子是哪個語系看狀態 JSON 的 lang
CHANNELS = {"zh-TW": "esp32-tracker", "zh-CN": "esp32-tracker-zh-cn", "en": "esp32-tracker-en"}
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import i18n_lib  # noqa: E402


def no_wifi_markers():
    """沒有 Wi-Fi 時的訊息開頭（各語系）。這兩種失敗記成略過，不算韌體的問題"""
    out = ["附近沒有", "連不上 Wi-Fi"]
    for e in i18n_lib.load(i18n_lib.FW_CATALOG):
        if e["tw"].startswith(("附近沒有 Wi-Fi", "連不上 Wi-Fi")):
            out += [e[k].split("%")[0].strip() for k in ("cn", "en") if e.get(k)]
    return [m for m in out if m]
FAKE_ADDR = "fa:ke:00:00:00:01"
BUILD_RE = re.compile(r"^BUILD,(.+?),([\w?]+),(\S+)$")
READY_RE = re.compile(r"長按 BOOT 2 秒開啟手機連線")


class Board:
    """讀取執行緒把每一行收進 lines，wait() 從指定的位置往後找"""

    def __init__(self, port, log, reset_cmd=None):
        import serial  # PlatformIO 的 Python 有 pyserial
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.2
        self.s.dtr = False  # DTR 低、RTS 低：GPIO0 不拉低，不會進燒錄模式
        self.s.rts = False
        self.s.open()
        self.log, self.reset_cmd = log, reset_cmd
        self.lines, self.lock, self.alive = [], threading.Lock(), True
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        while self.alive:
            try:
                d = self.s.read(4096)
            except Exception:
                break
            if not d:
                continue
            buf += d
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                with self.lock:
                    self.lines.append(line)
                self.log.write(f"{time.strftime('%H:%M:%S')} {line}\n")
                self.log.flush()

    def mark(self):
        with self.lock:
            return len(self.lines)

    def send(self, cmd):
        self.log.write(f"{time.strftime('%H:%M:%S')} >>> {cmd}\n")
        self.s.write((cmd + "\n").encode())

    def reset(self):
        """拉一下 EN（RTS）重新開機"""
        if self.reset_cmd:
            self.send(self.reset_cmd)
            return
        self.s.rts = True
        time.sleep(0.15)
        self.s.rts = False

    def wait(self, pattern, timeout, since):
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        end = time.time() + timeout
        i = since
        while time.time() < end:
            with self.lock:
                new = self.lines[i:]
            for k, line in enumerate(new):
                m = rx.search(line)
                if m:
                    return m, i + k + 1
            i += len(new)
            time.sleep(0.1)
        return None, i

    def ask(self, cmd, pattern, timeout=8):
        at = self.mark()
        self.send(cmd)
        m, _ = self.wait(pattern, timeout, at)
        return m

    def ask_json(self, cmd, prefix, timeout=8):
        m = self.ask(cmd, r"^" + re.escape(prefix), timeout)
        if not m:
            return None
        try:
            return json.loads(m.string)
        except ValueError:
            return None

    def close(self):
        self.alive = False
        time.sleep(0.3)
        self.s.close()


class Run:
    def __init__(self, board):
        self.b, self.results, self.build, self.part, self.rev = board, [], None, None, None
        self.update_ok = False  # 從測試通道更新、新版本身也讀得到測試通道
        self.label = "HIL-" + datetime.now().strftime("%m%d%H%M%S")

    def rec(self, name, status, detail=""):
        self.results.append((name, status, detail))
        print(f"{ {'pass': '通過', 'fail': '失敗', 'skip': '略過'}[status] } {name}" + (f"：{detail}" if detail else ""),
              flush=True)
        return status == "pass"

    def boot(self, name="開機"):
        at = self.b.mark()
        self.b.reset()
        m, i = self.b.wait(BUILD_RE, 30, at)
        if not m:
            return self.rec(name, "fail", "30 秒內沒有 BUILD 行")
        self.build, self.part, self.rev = m.group(1), m.group(2), m.group(3)
        if not self.b.wait(READY_RE, 30, i)[0]:
            return self.rec(name, "fail", "開機沒有完成")
        time.sleep(1.5)
        return self.rec(name, "pass", f"{self.rev}，建置於 {self.build}，{self.part}")

    def status(self):
        return self.b.ask_json("json", '{"scan"', 10)

    # ---- 項目 ----

    def t_status(self):
        j = self.status()
        if not j:
            return self.rec("狀態 JSON", "fail", "json 指令沒有回傳可以解析的 JSON")
        need = ["scan", "trk", "build", "rev", "lang", "netState", "autoupd", "testchan", "roomState", "interval", "duration"]
        miss = [k for k in need if k not in j]
        if miss:
            return self.rec("狀態 JSON", "fail", "少了欄位 " + "、".join(miss))
        if j["build"] != self.build:
            return self.rec("狀態 JSON", "fail", f"建置時間跟 BUILD 行不同：{j['build']}")
        return self.rec("狀態 JSON", "pass")

    def netupdate(self, timeout=120):
        """送 netupdate，傳回 (結果, 說明, 讀的網址, 結果之後的行號)。結果是 done、latest、fail、nowifi 或 None"""
        at = self.b.mark()
        self.b.send("netupdate")
        m, i = self.b.wait(r"^OTA,\d+,net_(done|latest|fail),?(.*)$|^先用 netwifi", timeout, at)
        start = self.b.wait(r"^OTA,\d+,net_start,(\S+)", 0.5, at)[0]
        url = start.group(1) if start else ""
        if not m:
            return None, "", url, i
        if m.group(0).startswith("先用 netwifi"):
            return "nowifi", "", url, i
        return m.group(1), m.group(2), url, i

    def t_update(self):
        name = "從測試通道更新"
        if not self.b.ask("channel test", r"^OTA,\d+,channel_test"):
            return self.rec("改接收測試版", "fail", "沒有 channel_test")
        self.rec("改接收測試版", "pass")
        lang = (self.status() or {}).get("lang", "zh-TW")
        test_dir = CHANNELS.get(lang, CHANNELS["zh-TW"]) + "-test/"
        try:
            req = urllib.request.Request(f"https://assets.anoni.net/{test_dir}latest.json?r={time.time_ns()}", headers={"User-Agent": "anoni-tracker-hil"})
            remote = json.loads(urllib.request.urlopen(req, timeout=15).read())
        except Exception as e:
            return self.rec(name, "skip", f"電腦讀不到測試通道：{e}")
        kind, msg, url, i = self.netupdate()
        if kind is None:
            return self.rec(name, "fail", "120 秒內沒有結果")
        if kind == "nowifi":
            return self.rec(name, "skip", "板子沒有設定更新用的 Wi-Fi")
        if not url.endswith("/" + test_dir):
            return self.rec(name, "fail", f"讀的不是 {lang} 的測試通道：{url or '沒有 net_start'}")
        if kind == "fail":
            if any(m in msg for m in no_wifi_markers()):
                return self.rec(name, "skip", msg)
            return self.rec(name, "fail", msg)
        if kind == "done":
            m2, i2 = self.b.wait(BUILD_RE, 60, i)
            if not m2:
                return self.rec(name, "fail", "下載完成後 60 秒內沒有重新開機")
            self.build, self.part, self.rev = m2.group(1), m2.group(2), m2.group(3)
            self.b.wait(READY_RE, 30, i2)
            time.sleep(1.5)
        if self.build != remote["build"]:
            return self.rec(name, "fail" if kind == "done" else "skip",
                            f"板子是 {self.build}，測試通道是 {remote['build']}（用 USB 燒的較新版本不能推正式版）")
        self.rec(name, "pass", f"{'下載並換上' if kind == 'done' else '已經是'} {remote['commit']}")
        # 換上的新版本自己的更新程式也要能讀測試通道，回報已經是最新版
        kind2, msg2, url2, _ = self.netupdate()
        if kind2 != "latest" or not url2.endswith("/" + test_dir):
            return self.rec("新版的網址更新", "fail", f"{kind2}：{msg2 or url2}")
        self.update_ok = True
        return self.rec("新版的網址更新", "pass", "已經是最新版")

    def t_auto(self):
        """自動更新照條件檢查一次。延後的條件（有警示、手機連著熱點）成立時不會有結果，記成略過"""
        name = "自動更新"
        was = (self.status() or {}).get("autoupd", False)
        self.b.send("autoupdate on")  # 連續沒有網路時只記一次，先重新打開讓結果一定會印出來
        time.sleep(0.5)
        at = self.b.mark()
        self.b.send("autoupdate now")
        m, i = self.b.wait(r"^OTA,\d+,auto_(latest|done|fail|nonet),?(.*)$", 120, at)
        try:
            if not m:
                return self.rec(name, "skip", "120 秒內沒有檢查（可能有警示或手機連著熱點而延後）")
            kind, msg = m.group(1), m.group(2)
            if kind == "done":
                m2, i2 = self.b.wait(BUILD_RE, 60, i)
                ok = self.b.wait(r"^OTA,0,auto_ok", 10, i2)[0] if m2 else None
                if not ok:
                    return self.rec(name, "fail", "自動安裝後沒有 auto_ok")
                self.build, self.part, self.rev = m2.group(1), m2.group(2), m2.group(3)
                self.b.wait(READY_RE, 30, i2)
                time.sleep(1.5)
                return self.rec(name, "pass", "自動安裝並重新開機")
            if kind == "nonet":
                return self.rec(name, "skip", msg)
            return self.rec(name, "pass" if kind == "latest" else "fail", msg)
        finally:
            if not was:
                self.b.send("autoupdate off")

    def t_fake(self):
        j = self.status() or {}
        wait = 2 * (j.get("interval", 15) + j.get("duration", 5)) + 10
        self.b.send("fake on")
        end, found = time.time() + wait, False
        while time.time() < end and not found:
            time.sleep(5)
            s = self.status() or {}
            found = any(t.get("addr") == FAKE_ADDR for t in s.get("trk", []))
        self.b.send("fake off")
        return self.rec("假追蹤器出現在清單", "pass" if found else "fail", "" if found else f"{wait} 秒內沒出現")

    def t_mark(self):
        self.b.send("mark " + self.label)
        time.sleep(1)
        h = self.b.ask_json("history", "{", 10)
        ok = bool(h) and self.label in json.dumps(h, ensure_ascii=False)
        return self.rec("打記號寫進歷史", "pass" if ok else "fail", self.label if ok else "history 裡沒有這個記號")

    def t_oled(self):
        m = self.b.ask("oled dump", r"^OLEDBUF,([0-9a-f]+)$", 8)
        ok = bool(m) and len(m.group(1)) == 2048
        return self.rec("OLED 畫面輸出", "pass" if ok else "fail", "" if ok else "OLEDBUF 不是 1024 bytes")

    def t_status_cmds(self):
        a = self.b.ask("autoupdate", r"^自動更新：", 5)
        c = self.b.ask("channel", r"^更新通道：", 5)
        mem = self.b.ask("mem", r"^MEM,\d+,cmd,(\d+),(\d+),(\d+),(\d+),", 5)
        if not mem:
            self.rec("記憶體", "fail", "mem 沒有回應")
        else:
            big = int(mem.group(2))
            self.rec("記憶體", "pass", f"內部剩 {int(mem.group(1)) // 1024} KB，最大連續 {big // 1024} KB，"
                     f"開機以來最低 {int(mem.group(3)) // 1024} KB，PSRAM 剩 {int(mem.group(4)) // 1048576} MB"
                     + ("（內部最大連續不到 40 KB，網址更新要靠 PSRAM）" if big < 40000 else ""))
        pl = self.b.ask("places", r"^目前在地點 #\d+", 5)
        if not pl:
            self.rec("地點清單", "fail", "places 沒有回應")
        else:
            self.rec("地點清單", "pass", pl.string)
        ok = bool(a and c)
        return self.rec("自動更新與通道的狀態", "pass" if ok else "fail", f"{a.string if a else '沒有回應'}；{c.string if c else '沒有回應'}")

    def t_persist(self):
        at = self.b.mark()
        if not self.boot("重新開機"):
            return False
        arch = self.b.wait(r"^HIST,0,archived", 1, at)[0]
        p = self.b.ask("history prev", r"^\[", 10)
        ok = bool(p) and self.label in p.string
        if not arch:
            return self.rec("重新開機後保留歷史", "fail", "開機時沒有 HIST,0,archived")
        return self.rec("重新開機後保留歷史", "pass" if ok else "fail", "" if ok else "之前的歷史裡沒有剛才的記號")

    def t_room(self):
        at = self.b.mark()
        self.b.send("room")
        m, _ = self.b.wait(r"^ROOM,\d+,(done|cancelled),(\d+),(\d+),(\d+)|^現在不能開始房間檢查", 240, at)
        if not m or m.group(1) != "done":
            return self.rec("房間檢查", "fail", "240 秒內沒有完成" if not m else m.string)
        r = self.b.ask_json("room json", "{", 10)
        ok = bool(r) and r.get("state") == "done"
        detail = f"基地台 {m.group(2)}、連網裝置 {m.group(3)}、藍牙 {m.group(4)}"
        return self.rec("房間檢查", "pass" if ok else "fail", detail if ok else "room json 不是 done")

    def t_fake_alert(self):
        j = self.status() or {}
        preset = j.get("preset", 1)
        alerted = False
        try:
            self.b.send("preset 3")  # 快速測試門檻，約 2 分鐘
            time.sleep(1)
            self.b.send("fake on")
            end = time.time() + 240
            while time.time() < end and not alerted:
                time.sleep(10)
                s = self.status() or {}
                alerted = any(t.get("addr") == FAKE_ADDR and t.get("alerted") for t in s.get("trk", []))
        finally:
            # 中途停下也要改回原本的門檻
            self.b.send("fake off")
            self.b.send("ack")
            self.b.send(f"preset {preset}")
            time.sleep(1)
        if (self.status() or {}).get("preset") != preset:
            self.rec("門檻改回原樣", "fail", f"沒有改回 {preset}")
        return self.rec("假追蹤器觸發警示", "pass" if alerted else "fail", "" if alerted else "240 秒內沒有警示")

    def t_probes(self):
        """監聽 20 秒周圍的探測請求，序列埠與記錄只有總數，不該出現名稱"""
        if not self.b.ask("probes start", r"^PROBE,\d+,start,(\d+)", 8):
            return self.rec("探測請求監聽", "fail", "沒有開始")
        time.sleep(20)
        st = self.b.ask("probes", r"^探測請求：監聽中，頻道 (\d+)，請求 (\d+) 個，來源 (\d+) 個", 5)
        at = self.b.mark()
        self.b.send("probes stop")
        done, _ = self.b.wait(r"^PROBE,\d+,done,(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)$", 8, at)
        if not st or not done:
            return self.rec("探測請求監聽", "fail", "沒有統計" if not st else "停止時沒有 PROBE done")
        return self.rec("探測請求監聽", "pass", f"頻道 {st.group(1)}，20 秒收到 {st.group(2)} 個請求、{st.group(3)} 個來源，"
                        f"帶名稱 {done.group(6)} 個、不同的名稱 {done.group(7)} 個")

    def t_meeting(self):
        """會議室檢查開始、建立基準、結束，序列埠與記錄只有總數"""
        if not self.b.ask("meeting start -70", r"^MEETING,\d+,start,-70", 8):
            return self.rec("會議室檢查", "fail", "沒有開始")
        time.sleep(15)
        st = self.b.ask("meeting", r"^會議室檢查：建立基準中，門檻 -70 dBm，基準 (\d+) 個", 5)
        at = self.b.mark()
        self.b.send("meeting stop")
        done, _ = self.b.wait(r"^MEETING,\d+,done,\d+,-70,(\d+),(\d+)$", 8, at)
        if not st or not done:
            return self.rec("會議室檢查", "fail", "沒有狀態" if not st else "結束時沒有 MEETING done")
        return self.rec("會議室檢查", "pass", f"15 秒內基準收到 {st.group(1)} 個強訊號的藍牙裝置")

    def t_ota_selftest(self):
        before, build = self.part, self.build
        at = self.b.mark()
        self.b.send("ota selftest")
        m, i = self.b.wait(BUILD_RE, 90, at)
        if not m:
            return self.rec("OTA 自我測試", "fail", "90 秒內沒有重新開機")
        self.build, self.part, self.rev = m.group(1), m.group(2), m.group(3)
        self.b.wait(READY_RE, 30, i)
        time.sleep(1.5)
        ok = self.part != before and self.build == build
        return self.rec("OTA 自我測試", "pass" if ok else "fail", f"{before} → {self.part}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", required=True, help="板子的序列埠，例如 /dev/cu.usbmodem1101")
    ap.add_argument("--update", action="store_true", help="先改接收測試版，從測試通道更新到最新")
    ap.add_argument("--long", action="store_true", help="加上房間檢查、假警示與 OTA 自我測試")
    ap.add_argument("--reset-cmd", help=argparse.SUPPRESS)  # 測試這支腳本用：用指令代替 RTS 重新開機
    a = ap.parse_args()

    RESULTS.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = RESULTS / f"run-{stamp}.log"
    with open(log_path, "w", encoding="utf-8") as log:
        b = Board(a.port, log, a.reset_cmd)
        r = Run(b)
        try:
            if r.boot():
                r.t_status()
                if a.update:
                    r.t_update()
                    r.t_auto()
                r.t_status_cmds()
                r.t_oled()
                r.t_mark()
                r.t_fake()
                if a.long:
                    r.t_fake_alert()
                    r.t_room()
                    r.t_probes()
                    r.t_meeting()
                r.t_persist()
                if a.long:
                    r.t_ota_selftest()
        finally:
            b.close()

    fails = [x for x in r.results if x[1] == "fail"]
    skips = [x for x in r.results if x[1] == "skip"]
    print(f"\n實機測試：{len(r.results) - len(fails) - len(skips)} 通過、{len(fails)} 失敗、{len(skips)} 略過，"
          f"序列埠記錄在 {log_path}")
    if fails or not r.build:
        return 1
    if a.reset_cmd:
        print("（模擬的板子，不記錄通過的版本）")
        return 0
    with open(RESULTS / "passed.tsv", "a", encoding="utf-8") as f:
        # 欄位：建置時間、版本、時間、範圍、從測試通道更新的結果、略過幾項。
        # promote_firmware.sh 要求第 5 欄是 update_pass
        f.write("\t".join([r.build, r.rev or "", stamp, "long" if a.long else "basic",
                           "update_pass" if r.update_ok else "update_none", f"skip={len(skips)}"]) + "\n")
    print(f"已記錄通過的版本：{r.rev}，建置於 {r.build}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
