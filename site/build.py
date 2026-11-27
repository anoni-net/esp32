#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["jinja2>=3.1", "markdown>=3.5", "websockets>=13", "segno>=1.6"]
# ///
"""產生專案網頁 anoni.net/esp32/ 的 clearnet 與 onion 兩份靜態產物。

    uv run site/build.py                 # 產生到 site/public/clearnet 與 site/public/onion
    uv run site/build.py --out DIR       # 產生到 DIR/clearnet 與 DIR/onion，主機部署時用
    uv run site/build.py --check         # 產生到暫存目錄並檢查，CI 執行同一個指令
    uv run site/build.py --offline       # 不讀 assets.anoni.net 的版本資訊（CI 用）

--check 也用 headless Chrome 以 320、390、1280 三種寬度開每一頁，不能出現橫向捲軸（同 anoni-net/news 的
layout_check.py），截圖存在 site/.cache/screenshots/。找不到 Chrome 時略過，可以用 CHROME 環境變數指定。

工具的清單來自各工具資料夾的 site.toml，介紹頁的內文是同一個資料夾的 site.md。內文裡可以寫：

    <!-- shot 情境 -->   放一張模擬面板的截圖（site/shots.py 產生，存在 <工具>/site/shots/）
    <!-- demo -->        「在網頁上試試看」的按鈕，連到模擬面板
    <!-- install -->     「前往安裝」的按鈕，連到 install/<工具>/
    <!-- version -->     正式通道目前的版本（建置時讀 latest.json）
    <!-- oled -->        site.toml 的 [[oled]] 列出的 OLED 畫面
    {root}               網站根目錄的相對路徑，連到站內其他頁面時用
    {static:檔名}        static/ 底下的檔案（例如圖片），網址帶內容的雜湊

site/pages/ 的頁面另外可以寫 <!-- versions -->（各工具正式通道的版本）與 <!-- devtools -->（開發用的工具）。

讀者分兩種：不會寫程式、接上 USB 就要能裝好並連上裝置的人，以及會寫程式的工程師。每一頁都先寫給前者，
腳位、建置、版本雜湊這些技術內容放在頁尾的「給開發者」段落或 developers/ 頁。site.toml 寫
audience = "dev" 的工具（例如示範韌體）不列在導覽列，首頁的工具列表排在最後並標成開發用，也列在 developers/ 頁。

頁面裡的站內連結一律是相對路徑，clearnet（anoni.net/esp32/）與 onion（esp32.<onion>/）掛在
不同的路徑也能用同一份。onion 那份把連到 anoni.net 的網址改成 onion 位址。頁面不載入任何
JavaScript、外部字型與第三方資源。模擬面板是例外，放在 demo/<工具>/frame/，由主機另外設定 CSP，
在 sandbox 的 iframe 裡執行，不能對外連線。
"""
import argparse
import html
import importlib.util
import json
import re
import secrets
import shutil
import sys
import tempfile
import tomllib
import urllib.request
from pathlib import Path
from urllib.parse import urlparse

import markdown
from jinja2 import Environment, FileSystemLoader, StrictUndefined
from markupsafe import Markup

SITE = Path(__file__).resolve().parent
ROOT = SITE.parent
REPO_URL = "https://github.com/anoni-net/esp32"
ONION = "anoninetru5tflukgfaehun7q6khowgmymcff3gtk5oyesqazhmfxtyd.onion"
TARGETS = {
    "clearnet": {"base": "https://anoni.net/esp32/", "onion_location": f"http://esp32.{ONION}/", "rewrite": []},
    # 由長到短，子網域與路徑排在網域本身之前（同 anoni-net/www 的 site.toml）
    "onion": {"base": f"http://esp32.{ONION}/", "onion_location": "", "rewrite": [
        ("https://assets.anoni.net", f"http://{ONION}/assets"),
        ("https://anoni.net/esp32/", f"http://esp32.{ONION}/"),
        ("https://anoni.net/docs", f"http://docs.{ONION}"),
        ("https://anoni.net/gears", f"http://gears.{ONION}"),
        ("https://form.anoni.net", f"http://form.{ONION}"),
        ("https://anoni.net", f"http://{ONION}"),
    ]},
}
UA = "anoni-esp32-site"
SITE_DESC = "匿名網路社群 anoni.net 用 ESP32-S3 開發板做的隱私工具：隨身偵測器找出跟著你的藍牙追蹤器，也能檢查旅館房間與會議室。原始碼公開。"


def load_tools():
    tools = []
    for f in sorted(ROOT.glob("*/site.toml")):
        t = tomllib.loads(f.read_text(encoding="utf-8"))
        t["slug"] = f.parent.name
        t["dir"] = f.parent
        t["body"] = (f.parent / "site.md").read_text(encoding="utf-8")
        t["scenarios"] = load_scenarios(t)
        t.setdefault("audience", "all")
        # 網頁安裝的韌體，每個語系一份（[[firmware]]：lang、label、channel）。還沒寫的工具沿用 channel，只有正體中文
        fw = t.get("firmware") or ([{"lang": "zh-TW", "label": "正體中文", "channel": t["channel"]}] if t.get("channel") else [])
        t["firmware"] = fw
        t.setdefault("channel", fw[0]["channel"] if fw else None)
        tools.append(t)
    return sorted(tools, key=lambda t: t["order"])


def load_scenarios(tool):
    """面板測試與網站共用的虛構情境（例如 pocket-detector/tests/panel/scenarios.py）。"""
    demo = tool.get("demo")
    if not demo:
        return None
    path = tool["dir"] / demo["scenarios"]
    spec = importlib.util.spec_from_file_location(f"scenarios_{tool['slug'].replace('-', '_')}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fetch_version(channel, offline):
    if offline or not channel:
        return None
    url = f"https://assets.anoni.net/{channel}/latest.json"
    try:
        # 加上隨機的查詢字串，避開 Cloudflare 快取的舊版本（同韌體的做法）
        req = urllib.request.Request(f"{url}?r={secrets.token_hex(4)}", headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=15) as r:
            j = json.load(r)
        return {"build": j["build"], "commit": j["commit"], "sha256": j["sha256"], "file": j["file"], "url": url}
    except Exception as e:  # 讀不到時頁面寫「暫時讀不到」，不讓整個建置失敗
        print(f"警告：讀不到 {url}（{e}）", file=sys.stderr)
        return None


def icon(name):
    """site/icons/ 的 SVG 內嵌進 HTML（同 anoni-net/www 的 icon()），不另外發請求，onion 版一樣可用。"""
    svg = (SITE / "icons" / f"{name}.svg").read_text(encoding="utf-8").strip()
    return Markup(svg.replace("<svg ", '<svg class="i" aria-hidden="true" focusable="false" ', 1))


def qr(url):
    """網址的 QR code，內嵌的 SVG。手機掃描後在手機上繼續看接下來的步驟（同 Tails 安裝指引的做法）。"""
    import segno
    return Markup(segno.make(url, error="m").svg_inline(scale=4, dark="#13232a", light="#ffffff", border=2,
                                                        svgclass="qr", omitsize=True))


def commit_in_repo(commit):
    """搬到這個 repo 之前建置的版本，commit 在這裡查不到，就不連到原始碼。"""
    import subprocess
    r = subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e", f"{commit}^{{commit}}"], capture_output=True)
    return r.returncode == 0


class Site:
    def __init__(self, offline):
        self.tools = load_tools()
        self.env = Environment(loader=FileSystemLoader(SITE / "templates"), autoescape=True,
                               undefined=StrictUndefined, trim_blocks=True, lstrip_blocks=True)
        self.env.globals.update(icon=icon, qr=qr)
        # 每個發布通道讀一次 latest.json。工具的版本是它主要通道（第一個語系）的版本
        self.channel_versions = {}
        for t in self.tools:
            for ch in {t["channel"], *(f["channel"] for f in t["firmware"])} - {None}:
                if ch not in self.channel_versions:
                    v = fetch_version(ch, offline)
                    if v:
                        v["linked"] = commit_in_repo(v["commit"])
                    self.channel_versions[ch] = v
        self.versions = {t["slug"]: self.channel_versions.get(t["channel"]) for t in self.tools}

    # 頁面：(輸出路徑, 模板, 內容)
    def pages(self):
        # 首頁列出全部的工具，開發用的排在後面、用不同的樣式標出來。導覽列只放給一般使用者的工具
        yield "", "home.html.j2", {"title": "ESP32 工坊", "description": SITE_DESC,
                                   "tools": self.user_tools + [t for t in self.tools if t["audience"] == "dev"],
                                   "hero_v": {t["slug"]: self.file_v(t["dir"] / "site" / "shots" / "alert.png") for t in self.tools}}
        for t in self.tools:
            yield f"{t['slug']}/", "tool.html.j2", {"title": t["name"], "tool": t, "description": t["summary"],
                                                   "html_body": self.body(t, depth=1)}
            if t["scenarios"]:
                yield f"demo/{t['slug']}/", "demo.html.j2", {"title": f"{t['name']}的操作頁面（示範）", "tool": t,
                                                             "description": f"在網頁上試試{t['name']}的操作頁面，用的是虛構的示範資料。",
                                                             "scenarios": t["scenarios"].DEMO}
        # 安裝頁：install/ 選工具，install/<工具>/ 選語系並安裝。工具與語系變多時每一頁都還是短的
        yield "install/", "install.html.j2", {"title": "安裝韌體", "tools": self.tools,
                                              "description": "用電腦的 Chrome 或 Edge，接上 ESP32-S3 開發板，按一下就能安裝社群的韌體，不需要寫程式。"}
        for t in self.tools:
            if t.get("install") != "web":
                continue
            yield f"install/{t['slug']}/", "install_tool.html.j2", {
                "title": f"安裝{t['name']}", "tool": t, "firmware": [self.firmware_info(t, f) for f in t["firmware"]],
                "description": f"用電腦的 Chrome 或 Edge，接上 ESP32-S3 開發板，按一下就能安裝{t['name']}，不需要寫程式。"}
        for p in sorted((SITE / "pages").glob("*.md")):
            meta, body = split_front(p.read_text(encoding="utf-8"))
            yield f"{p.stem}/", "page.html.j2", {"title": meta["title"], "lead": meta.get("lead", ""),
                                                 "description": meta.get("lead") or SITE_DESC,
                                                 "html_body": self.page_md(body, depth=1),
                                                 "tools": self.tools}

    def firmware_info(self, tool, fw):
        """安裝按鈕要的資料。manifest.json 在 assets.anoni.net 快取 12 小時，網址帶那個通道韌體的 SHA-256，
        換版之後網址就不同。部署腳本在 latest.json 變動時重建網頁，網址跟著換。韌體檔名本身帶雜湊，內容不會變"""
        v = self.channel_versions.get(fw["channel"]) or {}
        return dict(fw, manifest=f"https://assets.anoni.net/{fw['channel']}/manifest.json?v={v.get('sha256', '')[:12] or '0'}",
                    build=v.get("build", ""), version_html=self.version_block(tool, fw["channel"]))

    @property
    def user_tools(self):
        """給一般使用者的工具，列在首頁與導覽列。"""
        return [t for t in self.tools if t["audience"] != "dev"]

    def page_md(self, text, depth):
        root = "../" * depth
        out = self.md(text, depth, None)
        out = re.sub(r"(<p>)?<!-- versions -->(</p>)?",
                     lambda m: "".join(f'<h3>{html.escape(t["name"])}</h3>{self.version_block(t)}' for t in self.tools if t.get("channel")), out)
        out = re.sub(r"(<p>)?<!-- devtools -->(</p>)?", lambda m: self.devtools(root), out)
        return out

    def devtools(self, root):
        items = "".join(f'<li class="card cat-dev"><p class="tag">開發用</p><h3><a href="{root}{t["slug"]}/">{html.escape(t["name"])}</a></h3>'
                        f'<p>{html.escape(t["summary"])}</p><p class="meta">需要：{html.escape(t["hardware"])}，從原始碼建置安裝</p>'
                        f'<p class="more"><a href="#build">從原始碼建置的步驟</a></p></li>'
                        for t in self.tools if t["audience"] == "dev")
        return f'<ul class="cards">{items}</ul>'

    def body(self, tool, depth):
        return self.md(tool["body"], depth, tool)

    def md(self, text, depth, tool):
        root = "../" * depth
        out = markdown.markdown(text, extensions=["tables", "fenced_code", "attr_list"], output_format="html")
        if tool:
            out = re.sub(r"(<p>)?<!-- shot ([\w-]+) -->(</p>)?", lambda m: self.shot(tool, m.group(2), root), out)
            out = re.sub(r"(<p>)?<!-- demo -->(</p>)?", lambda m: self.demo_button(tool, root), out)
            out = re.sub(r"(<p>)?<!-- install -->(</p>)?", lambda m: self.install_button(tool, root), out)
            out = re.sub(r"(<p>)?<!-- version -->(</p>)?", lambda m: self.version_block(tool), out)
            out = re.sub(r"(<p>)?<!-- oled -->(</p>)?", lambda m: self.oled(tool, root), out)
        # {static:檔名}：static/ 底下的檔案，網址帶內容的雜湊（同模板的 asset()）
        out = re.sub(r"\{static:([\w./-]+)\}", lambda m: f"{root}static/{m.group(1)}?v={self.static_v(m.group(1))}", out)
        # 表格包一層，窄螢幕時可以左右捲動
        out = out.replace("<table>", '<div class="table"><table>').replace("</table>", "</table></div>")
        return out.replace("{root}", root)

    def scenario(self, tool, key):
        for s in tool["scenarios"].DEMO:
            if s["key"] == key:
                return s
        raise SystemExit(f"{tool['slug']}/site.md：沒有 {key} 這個情境")

    @staticmethod
    def file_v(path):
        """圖片網址的版本參數：檔案內容的雜湊，重新產生之後網址就不同，不會用到快取的舊圖。"""
        import hashlib
        return hashlib.sha256(path.read_bytes()).hexdigest()[:10] if path.exists() else "0"

    def shot(self, tool, key, root):
        s = self.scenario(tool, key)
        v = self.file_v(tool["dir"] / "site" / "shots" / f"{key}.png")
        alt = f"模擬面板的「{s['title']}」畫面：{s['desc']}"
        return (f'<figure class="phone"><a href="{root}demo/{tool["slug"]}/#{key}">'
                f'<img src="{root}shots/{tool["slug"]}/{key}.png?v={v}" width="390" height="844" alt="{html.escape(alt)}" loading="lazy"></a>'
                f'<figcaption>{html.escape(s["title"])}（示範資料）</figcaption></figure>')

    def oled(self, tool, root):
        items = "".join(f'<figure><img src="{root}oled/{tool["slug"]}/{html.escape(o["file"])}'
                        f'?v={self.file_v(tool["dir"] / "site" / "oled" / o["file"])}" width="512" height="256" '
                        f'alt="OLED 畫面：{html.escape(o["caption"])}" loading="lazy"><figcaption>{html.escape(o["caption"])}</figcaption></figure>'
                        for o in tool.get("oled", []))
        return f'<div class="oled">{items}</div>'

    def demo_button(self, tool, root):
        return f'<p class="actions"><a class="btn" href="{root}demo/{tool["slug"]}/">在網頁上試試看操作頁面</a></p>'

    def install_button(self, tool, root):
        return f'<p class="actions"><a class="btn solid" href="{root}install/{tool["slug"]}/">前往安裝</a></p>'

    def version_block(self, tool, channel=None):
        channel = channel or tool["channel"]
        v = self.channel_versions.get(channel)
        if not v:
            return '<p class="note">目前的版本資訊暫時讀不到，請到原始碼的頁面查看。</p>'
        commit = html.escape(v["commit"])
        src = (f'<a href="{REPO_URL}/tree/{commit}">{commit}</a>' if v["linked"]
               else f'{commit}（搬到這個 repo 之前建置的版本，原始碼裡查不到這個 commit）')
        return ('<dl class="version">'
                f'<dt>建置時間</dt><dd>{html.escape(v["build"])}</dd>'
                f'<dt>commit</dt><dd>{src}</dd>'
                f'<dt>SHA-256</dt><dd><code>{html.escape(v["sha256"])}</code></dd>'
                f'<dt>檔案</dt><dd><a href="https://assets.anoni.net/{channel}/{html.escape(v["file"])}">{html.escape(v["file"])}</a></dd>'
                '</dl>')

    @staticmethod
    def static_v(name):
        """static/ 底下檔案的版本參數（同 anoni-net/www 的 asset()）：每個檔案各自的內容雜湊，改了哪個檔案
        就只換那個網址。ESP Web Tools 的入口檔會再載入同一個資料夾的其他檔案，用整個資料夾的雜湊。"""
        import hashlib
        f = SITE / "static" / name
        files = (sorted(x for x in f.parent.iterdir() if x.is_file()) + [SITE / "esp_web_tools_patch.py"]
                 if name.startswith("esp-web-tools/") else [f])
        return hashlib.sha256(b"".join(x.read_bytes() for x in files)).hexdigest()[:10]

    def build(self, out, target):
        cfg = TARGETS[target]
        prefix = urlparse(cfg["base"]).path
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        shutil.copytree(SITE / "static", out / "static")
        # ESP Web Tools：一般安裝不清除 flash、對話框翻成正體中文（site/esp_web_tools_patch.py）
        import esp_web_tools_patch
        patch_problems = esp_web_tools_patch.apply(out / "static" / "esp-web-tools")
        if patch_problems:
            raise SystemExit("\n".join(patch_problems))
        # 404 放在根目錄，由 nginx 的 error_page 送出，任何深度的網址都可能用到，連結用絕對路徑
        pages = list(self.pages()) + [("404.html", "404.html.j2", {"title": "找不到這一頁", "description": SITE_DESC})]
        sitemap = []
        for path, tpl, ctx in pages:
            root = prefix if path == "404.html" else "../" * path.count("/")
            page = self.env.get_template(tpl).render(
                root=root, path=path, base=cfg["base"], onion_location=cfg["onion_location"] and cfg["onion_location"] + path,
                repo=REPO_URL, target=target, nav_tools=self.user_tools,
                asset=lambda name, root=root: f"{root}static/{name}?v={self.static_v(name)}",
                og_image=f"{cfg['base']}static/og.png?v={self.static_v('og.png')}", **ctx)
            for old, new in cfg["rewrite"]:
                page = page.replace(old, new)
            dest = out / path if path.endswith(".html") else out / path / "index.html"
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(page, encoding="utf-8")
            if path != "404.html":
                sitemap.append(cfg["base"] + path)
        (out / "sitemap.xml").write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
            + "".join(f"<url><loc>{html.escape(u)}</loc></url>\n" for u in sitemap) + "</urlset>\n", encoding="utf-8")
        # clearnet 的 robots.txt 在 anoni.net 的根目錄，由 anoni-net/www 產生，列出這裡的 sitemap。
        # onion 這側網站在子網域的根目錄，自己產生（同 anoni-net/news）
        if target == "onion":
            (out / "robots.txt").write_text(f"User-agent: *\nAllow: /\nSitemap: {cfg['base']}sitemap.xml\n", encoding="utf-8")
        for t in self.tools:
            for kind in ("shots", "oled"):
                src = t["dir"] / "site" / kind
                if src.exists():
                    shutil.copytree(src, out / kind / t["slug"])
            if t["scenarios"]:
                self.build_frames(t, out / "demo" / t["slug"] / "frame")

    def build_frames(self, tool, out):
        """模擬面板：真正的面板網頁，fetch 換成回傳情境的假資料。"""
        out.mkdir(parents=True, exist_ok=True)
        panel = (tool["dir"] / tool["demo"]["page"]).read_text(encoding="utf-8")
        marker = "<script>\nconst $"
        if marker not in panel:
            raise SystemExit(f"{tool['demo']['page']} 裡找不到插入點 {marker!r}")
        for s in tool["scenarios"].DEMO:
            page = panel.replace(marker, tool["scenarios"].demo_script(s) + marker, 1)
            page = page.replace("<head>", '<head>\n<meta name="robots" content="noindex">', 1)
            (out / f"{s['key']}.html").write_text(page, encoding="utf-8")


def split_front(text):
    """頁面開頭的 +++ 區塊是 TOML。"""
    m = re.match(r"\+\+\+\n(.*?)\n\+\+\+\n", text, re.S)
    if not m:
        raise SystemExit("頁面開頭要有 +++ 包住的 title")
    return tomllib.loads(m.group(1)), text[m.end():]


def check(out, target):
    """站內連結都要找得到檔案，onion 那份不能有 clearnet 的 anoni.net 網址，頁面不載入外部資源。"""
    problems = []
    prefix = urlparse(TARGETS[target]["base"]).path
    for f in out.rglob("*.html"):
        rel = f.relative_to(out)
        text = f.read_text(encoding="utf-8")
        frame = "frame" in rel.parts
        if not frame:
            # 只允許安裝頁載入站內的 ESP Web Tools（CSP 是 script-src 'self'），不能有 inline 的 script、事件與 style
            scripts = re.findall(r"<script\b[^>]*>", text)
            if any(not re.search(r'type="module" src="[./]*static/esp-web-tools/install-button\.js(\?v=\w+)?"', s) for s in scripts) \
                    or re.search(r"<script\b[^>]*>[^<]", text) or re.search(r'\son[a-z]+=|style="', text):
                problems.append(f"{target}/{rel}：頁面有 inline 的 script、事件或 style，CSP 會擋下")
            if scripts and not (rel.parts[0] == "install" and len(rel.parts) == 3):
                problems.append(f"{target}/{rel}：只有各工具的安裝頁（install/<工具>/）可以載入 script")
            for m in re.finditer(r'(?:href|src)="([^"]+)"', text):
                url = html.unescape(m.group(1))
                if re.match(r"(https?:|mailto:)", url):
                    # canonical 與一般連結不算，樣式、圖示、圖片、script、iframe 才算載入
                    if re.search(r'<(img|script|iframe|link(?![^>]*rel="canonical"))\b[^>]*(?:href|src)="'
                                 + re.escape(m.group(1)) + '"', text):
                        problems.append(f"{target}/{rel}：載入外部資源 {url}")
                    continue
                if url.startswith("#"):
                    continue
                if url.startswith("/"):
                    # 404 頁用絕對路徑，要在這個網站的路徑底下
                    if not url.startswith(prefix):
                        problems.append(f"{target}/{rel}：連到網站以外的路徑 {url}")
                        continue
                    url = str(out / url[len(prefix):])
                path = (f.parent / url.split("#")[0].split("?")[0]).resolve()
                if path.is_dir():
                    path = path / "index.html"
                if not path.exists():
                    problems.append(f"{target}/{rel}：找不到 {url}")
        if target == "onion" and re.search(r"https://([a-z0-9-]+\.)?anoni\.net", text):
            problems.append(f"{target}/{rel}：onion 版本還有 anoni.net 的 clearnet 網址")
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--offline", action="store_true")
    a = ap.parse_args()
    site = Site(a.offline)
    tmp = tempfile.TemporaryDirectory() if a.check and not a.out else None
    base = Path(tmp.name) if tmp else (a.out or SITE / "public")
    problems = []
    for target in TARGETS:
        site.build(base / target, target)
        problems += check(base / target, target)
    notice = None
    if a.check:
        import layout_check
        pages = [p for p, _, _ in site.pages()] + ["404.html"]
        layout_problems, notice = layout_check.run(base / "clearnet", pages, SITE / ".cache" / "screenshots", prefix="esp32")
        problems += layout_problems
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        sys.exit(f"有 {len(problems)} 個問題")
    if notice:
        print(notice)
    print(f"已產生 {base}/clearnet 與 {base}/onion")


if __name__ == "__main__":
    main()
