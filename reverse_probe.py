import json
import re
import subprocess
import time
from urllib.request import Request, urlopen

MODEL = "ILCE-6400"
PASSWORD = "QDvADbLp"


def run(cmd: str):
    p = subprocess.run(cmd, shell=True, text=True, capture_output=True)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def find_camera_ssid(max_wait=90):
    end = time.time() + max_wait
    while time.time() < end:
        for iface in ["WLAN 2", "WLAN"]:
            _, txt = run(f'netsh wlan show networks mode=bssid interface="{iface}"')
            m = re.search(r"DIRECT-[^\r\n]*" + re.escape(MODEL), txt)
            if m:
                return iface, m.group(0)
        time.sleep(2)
    return None, None


def connect(iface, ssid):
    xml = f'''<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>{ssid}</name>
  <SSIDConfig><SSID><name>{ssid}</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security><authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>{PASSWORD}</keyMaterial></sharedKey></security></MSM>
</WLANProfile>
'''
    path = r"e:\Co2Root\_tmp_cam_profile.xml"
    with open(path, "w", encoding="ascii") as f:
        f.write(xml)

    run(f'netsh wlan add profile filename="{path}" interface="{iface}" user=current')
    run(f'netsh wlan disconnect interface="{iface}"')
    time.sleep(1)
    _, out = run(f'netsh wlan connect name="{ssid}" ssid="{ssid}" interface="{iface}"')
    time.sleep(5)
    return out


def sony(method, params=None, version="1.0"):
    if params is None:
        params = []
    url = "http://192.168.122.1:10000/sony/camera"
    body = json.dumps({"method": method, "params": params, "id": 1, "version": version}).encode("utf-8")
    req = Request(url, data=body, headers={"Content-Type": "application/json"})
    with urlopen(req, timeout=6) as r:
        return json.loads(r.read().decode("utf-8", errors="replace"))


def read_live_chunks(url, max_chunks=8):
    req = Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urlopen(req, timeout=12) as r:
        chunks = []
        for _ in range(max_chunks):
            b = r.read(65536)
            if not b:
                break
            chunks.append(b)
    return chunks


iface, ssid = find_camera_ssid()
print("SSID_FOUND:", iface, ssid)
if not ssid:
    raise SystemExit("Camera SSID not visible")

print("RECONNECT_OUT:", connect(iface, ssid).strip())
print("INTERFACES:\n", run("netsh wlan show interfaces")[1][:1400])

versions = sony("getVersions", [])
print("getVersions:", versions)
api_list = sony("getAvailableApiList", [])
api_json = json.dumps(api_list)
print("has startLiveview:", "startLiveview" in api_json)
print("has startMovieRec:", "startMovieRec" in api_json)

try:
    print("startRecMode:", sony("startRecMode", []))
except Exception as e:
    print("startRecMode exception:", e)

time.sleep(2)
lv = sony("startLiveview", [])
print("startLiveview:", lv)
if "result" not in lv or not lv["result"]:
    raise SystemExit("No liveview URL returned")

url = lv["result"][0]
print("LIVEVIEW_URL:", url)

chunks = read_live_chunks(url, max_chunks=8)
print("chunks_read:", len(chunks))
for i, c in enumerate(chunks):
    has_soi = b"\xff\xd8" in c
    has_eoi = b"\xff\xd9" in c
    print(f"chunk{i}: len={len(c)} soi={has_soi} eoi={has_eoi}")

all_data = b"".join(chunks)
print("aggregate_len:", len(all_data))
print("aggregate_has_soi:", b"\xff\xd8" in all_data)
print("aggregate_has_eoi:", b"\xff\xd9" in all_data)

with open(r"e:\Co2Root\liveview-url.txt", "w", encoding="ascii") as f:
    f.write(url)
with open(r"e:\Co2Root\liveview-sample.bin", "wb") as f:
    f.write(all_data[:500000])

print("WROTE: e:/Co2Root/liveview-url.txt")
print("WROTE: e:/Co2Root/liveview-sample.bin")
