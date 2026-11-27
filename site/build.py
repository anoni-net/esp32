#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["jinja2>=3.1", "markdown>=3.5"]
# ///
"""產生專案網頁 anoni.net/esp32/ 的 clearnet 與 onion 兩份靜態產物。

    uv run site/build.py                 # 產生到 site/public/clearnet 與 site/public/onion
    uv run site/build.py --out DIR       # 產生到 DIR/clearnet 與 DIR/onion，主機部署時用
    uv run site/build.py --check         # 產生到暫存目錄並檢查，CI 執行同一個指令
    uv run site/build.py --offline       # 不讀 assets.anoni.net 的版本資訊

工具的清單來自各工具資料夾的 site.toml，介紹頁的內文是同一個資料夾的 site.md。內文裡可以寫：

    <!-- shot 情境 -->   放一張模擬面板的截圖（site/shots.py 產生，存在 <工具>/site/shots/）
    <!-- demo -->        「在網頁上試試看」的按鈕，連到模擬面板
    <!-- install -->     「前往安裝」的按鈕
    <!-- version -->     正式通道目前的版本（建置時讀 latest.json）
    {root}               網站根目錄的相對路徑，連到站內其他頁面時用

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
import shutil
import sys
import tempfile
import tomllib
import urllib.request
from pathlib import Path

import markdown
from jinja2 import Environment, FileSystemLoader, StrictUndefined

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
        ("https://anoni.net", f"http://{ONION}"),
    ]},
}
UA = "anoni-esp32-site"


def load_tools():
    tools = []
    for f in sorted(ROOT.glob("*/site.toml")):
        t = tomllib.loads(f.read_text(encoding="utf-8"))
        t["slug"] = f.parent.name
        t["dir"] = f.parent
        t["body"] = (f.parent / "site.md").read_text(encoding="utf-8")
        t["scenarios"] = load_scenarios(t)
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
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=15) as r:
            j = json.load(r)
        return {"build": j["build"], "commit": j["commit"], "sha256": j["sha256"], "file": j["file"], "url": url}
    except Exception as e:  # 讀不到時頁面寫「暫時讀不到」，不讓整個建置失敗
        print(f"警告：讀不到 {url}（{e}）", file=sys.stderr)
        return None


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
        self.versions = {}
        for t in self.tools:
            v = fetch_version(t.get("channel"), offline)
            if v:
                v["linked"] = commit_in_repo(v["commit"])
            self.versions[t["slug"]] = v

    # 頁面：(輸出路徑, 模板, 內容)
    def pages(self):
        yield "", "home.html.j2", {"title": "ESP32 小工具", "tools": self.tools}
        for t in self.tools:
            yield f"{t['slug']}/", "tool.html.j2", {"title": t["name"], "tool": t, "html_body": self.body(t, depth=1)}
            if t["scenarios"]:
                yield f"demo/{t['slug']}/", "demo.html.j2", {"title": f"{t['name']}的模擬面板", "tool": t,
                                                             "scenarios": t["scenarios"].DEMO}
        for p in sorted((SITE / "pages").glob("*.md")):
            meta, body = split_front(p.read_text(encoding="utf-8"))
            yield f"{p.stem}/", "page.html.j2", {"title": meta["title"], "lead": meta.get("lead", ""),
                                                 "html_body": self.md(body, depth=1, tool=None),
                                                 "tools": self.tools}

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
        # 表格包一層，窄螢幕時可以左右捲動
        out = out.replace("<table>", '<div class="table"><table>').replace("</table>", "</table></div>")
        return out.replace("{root}", root)

    def scenario(self, tool, key):
        for s in tool["scenarios"].DEMO:
            if s["key"] == key:
                return s
        raise SystemExit(f"{tool['slug']}/site.md：沒有 {key} 這個情境")

    def shot(self, tool, key, root):
        s = self.scenario(tool, key)
        alt = f"模擬面板的「{s['title']}」畫面：{s['desc']}"
        return (f'<figure class="phone"><a href="{root}demo/{tool["slug"]}/#{key}">'
                f'<img src="{root}shots/{tool["slug"]}/{key}.png" width="390" height="844" alt="{html.escape(alt)}" loading="lazy"></a>'
                f'<figcaption>{html.escape(s["title"])}（示範資料）</figcaption></figure>')

    def demo_button(self, tool, root):
        return f'<p class="actions"><a class="btn" href="{root}demo/{tool["slug"]}/">在網頁上試試看面板</a></p>'

    def install_button(self, tool, root):
        return f'<p class="actions"><a class="btn solid" href="{root}install/?tool={tool["slug"]}">前往安裝</a></p>'

    def version_block(self, tool):
        v = self.versions.get(tool["slug"])
        if not v:
            return '<p class="note">目前的版本資訊暫時讀不到，請到原始碼的頁面查看。</p>'
        commit = html.escape(v["commit"])
        src = (f'<a href="{REPO_URL}/tree/{commit}">{commit}</a>' if v["linked"]
               else f'{commit}（搬到這個 repo 之前建置的版本，原始碼裡查不到這個 commit）')
        return ('<dl class="version">'
                f'<dt>建置時間</dt><dd>{html.escape(v["build"])}</dd>'
                f'<dt>commit</dt><dd>{src}</dd>'
                f'<dt>SHA-256</dt><dd><code>{html.escape(v["sha256"])}</code></dd>'
                f'<dt>檔案</dt><dd><a href="https://assets.anoni.net/{tool["channel"]}/{html.escape(v["file"])}">{html.escape(v["file"])}</a></dd>'
                '</dl>')

    def build(self, out, target):
        cfg = TARGETS[target]
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        shutil.copytree(SITE / "static", out / "static")
        for path, tpl, ctx in self.pages():
            depth = path.count("/")
            page = self.env.get_template(tpl).render(
                root="../" * depth, path=path, base=cfg["base"], onion_location=cfg["onion_location"] and cfg["onion_location"] + path,
                repo=REPO_URL, target=target, nav_tools=self.tools, **ctx)
            for old, new in cfg["rewrite"]:
                page = page.replace(old, new)
            dest = out / path / "index.html"
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(page, encoding="utf-8")
        for t in self.tools:
            shots = t["dir"] / "site" / "shots"
            if shots.exists():
                shutil.copytree(shots, out / "shots" / t["slug"])
            if t["scenarios"]:
                self.build_frames(t, out / "demo" / t["slug"] / "frame")
        (out / "robots.txt").write_text("User-agent: *\nDisallow: /demo/\n", encoding="utf-8")

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
    for f in out.rglob("*.html"):
        rel = f.relative_to(out)
        text = f.read_text(encoding="utf-8")
        frame = "frame" in rel.parts
        if not frame:
            if re.search(r"<script|\son[a-z]+=|style=\"", text):
                problems.append(f"{target}/{rel}：頁面有 script、inline 事件或 style，CSP 會擋下")
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
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        sys.exit(f"有 {len(problems)} 個問題")
    print(f"已產生 {base}/clearnet 與 {base}/onion")


if __name__ == "__main__":
    main()
