import re
import zipfile
from pathlib import Path

apks = [
    Path(r"e:\Co2Root\uptodown-com.sony.playmemories.mobile.apk"),
    Path(r"e:\Co2Root\com.sony.playmemories.mobile_7.1.0-710_minAPI23(arm64-v8a,armeabi-v7a,x86,x86_64)(nodpi)_apkmirror.com.apk"),
]

# interesting protocol terms
terms = [
    b"startLiveview",
    b"stopLiveview",
    b"startMovieRec",
    b"stopMovieRec",
    b"liveviewstream",
    b"/sony/camera",
    b"image/jpeg",
    b"getAvailableApiList",
    b"startRecMode",
    b"X-Sony-",
    b"DmsDescPush.xml",
    b"DigitalImaging",
]

pat_ascii = re.compile(rb"[ -~]{6,}")


def extract_ascii_chunks(data: bytes):
    return pat_ascii.findall(data)


for apk in apks:
    print("=" * 80)
    print("APK:", apk.name)
    if not apk.exists():
        print("Missing")
        continue

    with zipfile.ZipFile(apk, "r") as zf:
        dex_names = [n for n in zf.namelist() if n.endswith('.dex')]
        print("DEX files:", dex_names)

        found = {t: set() for t in terms}

        for dex_name in dex_names:
            data = zf.read(dex_name)
            chunks = extract_ascii_chunks(data)
            for c in chunks:
                for t in terms:
                    if t in c:
                        s = c.decode("utf-8", errors="ignore")
                        if len(s) > 220:
                            s = s[:220] + "..."
                        found[t].add(s)

        for t in terms:
            vals = sorted(found[t])
            if vals:
                print(f"\n[{t.decode()}] matches={len(vals)}")
                for v in vals[:25]:
                    print(" ", v)

print("\nDone")
