import json, time, io
from pathlib import Path
from urllib.request import Request, urlopen
from PIL import Image
import numpy as np


def get_liveview_url():
    base='http://192.168.122.1:10000/sony/camera'
    body=json.dumps({'method':'startLiveview','params':[],'id':1,'version':'1.0'}).encode()
    try:
        with urlopen(Request(base,data=body,headers={'Content-Type':'application/json'}), timeout=6) as r:
            return json.loads(r.read().decode('utf-8','replace')).get('result',[None])[0]
    except Exception:
        return None

url=get_liveview_url()
if not url:
    url=Path(r'e:\Co2Root\liveview-url.txt').read_text(encoding='ascii').strip()
print('url',url)

buf=bytearray(); frames=[]
with urlopen(Request(url,headers={'User-Agent':'Mozilla/5.0'}), timeout=15) as r:
    t0=time.time()
    while len(frames)<10 and time.time()-t0<20:
        c=r.read(32768)
        if not c:
            break
        buf.extend(c)
        while True:
            s=buf.find(b'\xff\xd8')
            if s<0:
                break
            e=buf.find(b'\xff\xd9',s+2)
            if e<0:
                if s>0:
                    del buf[:s]
                break
            frames.append(bytes(buf[s:e+2]))
            del buf[:e+2]
            if len(frames)>=10:
                break

print('frames',len(frames))
out=Path(r'e:\Co2Root\frame_debug'); out.mkdir(exist_ok=True)
means=[]; diffs=[]; prev=None
for i,j in enumerate(frames):
    (out/f'f{i:02d}.jpg').write_bytes(j)
    arr=np.asarray(Image.open(io.BytesIO(j)).convert('RGB'),dtype=np.float32)
    means.append(float(arr.mean()))
    if prev is not None and prev.shape==arr.shape:
        diffs.append(float(np.mean(np.abs(arr-prev))))
    prev=arr
print('means', [round(x,2) for x in means])
print('diffs', [round(x,3) for x in diffs])
print('avg_diff', round(float(np.mean(diffs)),3) if diffs else None)
