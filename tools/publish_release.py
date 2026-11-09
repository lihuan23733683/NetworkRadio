"""把编译产物发布为 GitHub Release（附件可直接下载烧录）。

用法：
    python tools/publish_release.py 0.7.5

前置：
    1. dist/ 下放好 NetworkRadio-v<版本>-ESP32S3-full.bin 与 -app-only.bin
       （由 build-cache 里的 NetworkRadio.ino.merged.bin / .ino.bin 复制改名而来）
    2. tools/release-body.md 写完发布说明
    3. git 已保存 github.com 凭据（git credential fill 可取到）
"""
import json, os, subprocess, sys, urllib.request, urllib.error

REPO = "lihuan23733683/NetworkRadio"
VER = (sys.argv[1] if len(sys.argv) > 1 else "0.7.5").lstrip("v")
TAG = "v" + VER
NAME = f"{TAG} 固件下载（免编译，直接烧录）"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
BODY = open(os.path.join(ROOT, "tools", "release-body.md"), encoding="utf-8").read()
PROXY = "http://127.0.0.1:7890"

def token():
    out = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        if line.startswith("password="):
            return line[len("password="):]
    raise SystemExit("no token")

TOK = token()
opener = urllib.request.build_opener(
    urllib.request.ProxyHandler({"http": PROXY, "https": PROXY}))

def api(url, data=None, method=None, upload=False):
    if data is not None and not isinstance(data, bytes):
        data = json.dumps(data).encode()
    req = urllib.request.Request(url, data=data, method=method or ("POST" if data else "GET"))
    req.add_header("Authorization", "token " + TOK)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "NetworkRadio-release-script")
    req.add_header("Content-Type", "application/octet-stream" if upload else "application/json")
    try:
        with opener.open(req, timeout=900) as r:
            return json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        raise SystemExit(f"HTTP {e.code} on {url}\n{body}")

# 1) 若已有同名 release 则复用
try:
    existing = api(f"https://api.github.com/repos/{REPO}/releases/tags/{TAG}")
    print("release exists:", existing["id"], existing["html_url"])
except SystemExit:
    existing = None

if existing:
    rel = existing
else:
    rel = api(f"https://api.github.com/repos/{REPO}/releases", {
        "tag_name": TAG,
        "target_commitish": "main",
        "name": NAME,
        "body": BODY,
        "draft": False,
        "prerelease": False,
    })
    print("created release:", rel["id"], rel["html_url"])

# 2) 上传附件（跳过已存在的同名 asset）
have = {a["name"] for a in rel.get("assets", [])}
for fn in [f"NetworkRadio-v{VER}-ESP32S3-full.bin", f"NetworkRadio-v{VER}-app-only.bin"]:
    if fn in have:
        print("skip existing asset:", fn)
        continue
    path = os.path.join(DIST, fn)
    data = open(path, "rb").read()
    url = f"https://uploads.github.com/repos/{REPO}/releases/{rel['id']}/assets?name={fn}"
    a = api(url, data=data, upload=True)
    print("uploaded:", a["name"], a["size"], "bytes ->", a["browser_download_url"])

print("DONE", rel["html_url"])
