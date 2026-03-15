# Sony a6400 Liveview Reverse Engineering Report

Date: 2026-03-14

## 1) Verified endpoint and reconnect status

- Reconnected repeatedly to camera hotspot: `DIRECT-n6E1:ILCE-6400` on `WLAN 2`.
- Sony API endpoint confirmed reachable: `http://192.168.122.1:10000/sony/camera`.
- `getAvailableApiList` includes:
  - `startLiveview`
  - `stopLiveview`
  - `startMovieRec`
  - `stopMovieRec`
- `startLiveview` response is stable across repeated calls and returned:
  - `http://192.168.122.1:60152/liveviewstream?%211234%21%2a%3a%2a%3aimage%2fjpeg%3a%2a%21%21%21%21%21`

## 2) Meaning of the values after `liveviewstream?`

Raw query:

- `%211234%21%2a%3a%2a%3aimage%2fjpeg%3a%2a%21%21%21%21%21`

Decoded query:

- `!1234!*:*:image/jpeg:*!!!!!`

Interpretation:

- This is a Sony-specific token/filter descriptor, not a normal URL query dictionary.
- `1234` appears to be a session/control token in this stream profile format.
- `*:*:image/jpeg:*` indicates payload/media-type selection for JPEG frames.
- Trailing `!` groups are delimiters/fields used by Sony’s stream parser.

Observed behavior:

- The query token remained identical over multiple `startLiveview` calls in the same camera state.
- The stream still functions correctly, so this token is valid and accepted by the camera.

## 3) Stream payload structure (from captured sample)

Sample file:

- `e:/Co2Root/liveview-sample.bin`

Findings:

- First frame starts after a 136-byte header block.
- Data then contains JPEG markers (`FF D8 ... FF D9`) repeatedly.
- In sample:
  - SOI count: 28
  - EOI count: 27
- This confirms the endpoint is a framed Sony stream that embeds JPEG images.

Header observations:

- Byte 0: `FF`
- Byte 1: `01` (payload type likely JPEG frame packet)
- Sequence field (bytes 2..3, big-endian) increments per frame: 0,1,2,3...
- Timestamp-like field (bytes 4..7) increases over frames.
- A size-like field (bytes 12..15) tracks frame size scale; `(value >> 8)` correlates with JPEG lengths.

## 4) Scripts and tools executed

### Scripts

- `e:/Co2Root/reverse_probe.py` (reconnect + API probe + stream sample capture)
- `e:/Co2Root/acquire-liveview.ps1` (auto SSID scan/connect/liveview acquisition)
- `e:/Co2Root/ImagingEdge4Linux/liveview_webui.py` (browser viewing + start/stop recording controls)

### Reverse tools installed

- `mitmproxy` (version 12.2.1)
- `scapy`

## 5) Produced artifacts

- `e:/Co2Root/liveview-url.txt`
- `e:/Co2Root/liveview-sample.bin`
- `e:/Co2Root/reverse_probe.py`
- `e:/Co2Root/ImagingEdge4Linux/liveview_webui.py`

## 6) Practical conclusion

- The endpoint is correct and confirmed with repeated reconnect/probe cycles.
- The suffix after `liveviewstream?` is a Sony stream token descriptor; it is expected and valid.
- Stream payload is not plain MJPEG HTTP headers only; it is Sony-framed data carrying JPEG frames.
- Browser watchability is solved via the local proxy/web UI script.

## 7) Notes after reviewing `alpha-fairy`

Reference considered:

- <https://github.com/frank26080115/alpha-fairy/blob/main/doc/Camera-Reverse-Engineering.md>

What maps to our findings:

- Sony HTTP JSON-RPC is proprietary and camera-dependent in behavior.
- Liveview payload is not a direct browser JPEG stream; it includes Sony framing before JPEG bytes.
- This matches our dump where each frame has a 136-byte preamble before `FF D8`.

Token interpretation refinement:

- `!1234!*:*:image/jpeg:*!!!!!` behaves as an opaque Sony selector string.
- It should be treated as a full opaque token returned by `startLiveview` and not modified.

## 8) About the "dark brown stationary" symptom

Diagnostics run on captured frames:

- Extracted 10 live frames from the stream.
- Mean brightness around ~106/255 (not black).
- Inter-frame absolute difference ~0.686 (small, but non-zero), indicating slight motion/noise.

Interpretation:

- Stream is decoding correctly.
- Symptom is more likely one of:
  1. Browser/player showing only the initial Sony header chunk as a stale image.
  2. Camera scene/exposure producing an almost static dark preview.
  3. Camera in a mode that suppresses preview dynamics.

Mitigation implemented:

- Updated web UI to serve latest decoded JPEG at `/frame.jpg` and poll at ~150ms.
- This avoids browser MJPEG quirks and makes frame updates explicit.

## 9) Search for a higher-quality Wi-Fi liveview path

Additional references reviewed:

- `ma1co/Sony-PMCA-RE`
- `frank26080115/alpha-fairy`
- OpenMemories docs

Specific candidates searched for:

- `startLiveviewWithSize`
- `getAvailableLiveviewSize`
- `liveviewSize`
- `setLiveviewFrameInfo`

Current result:

- No confirmed higher-quality Wi-Fi JSON-RPC liveview selector was found for the a6400 in this workflow.
- Public reverse-engineering references point more toward USB/service-mode/firmware tooling than to a hidden ordinary Wi-Fi method.

Operational conclusion:

- For this project, the live preview path remains the Sony framed JPEG preview stream.
- It is usable for operator preview and Frigate detection input.
- It should not be treated as equivalent to true recording quality.

## 10) Frigate-triggered SD-card recording

Goal:

- Use Frigate detections to trigger recording on the camera SD card, instead of saving the low-quality preview stream.

Implemented pieces:

- Local relay for Frigate ingestion
- Authenticated Frigate event polling
- Bridge endpoints for `startMovieRec` / `stopMovieRec`
- Trigger loop with retry cooldown to avoid hammering the camera control endpoint

Current state:

- Frigate authentication and polling work.
- Bridge preview streaming works.
- Camera control remains the weak point: preview frames can continue while `startMovieRec` intermittently fails or hangs.

Most useful bridge hardening so far:

- Added subprocess timeouts to Windows Wi-Fi helper commands
- Stopped forcing reconnects when recent camera activity proves the link is still alive
- Kept `/api/status` responsive during control-path trouble

Current best interpretation:

- Windows Wi-Fi status reporting and Sony control readiness can temporarily diverge from ongoing preview traffic.
- The bridge should treat recent liveview frame activity as evidence that reconnect is unnecessary.
