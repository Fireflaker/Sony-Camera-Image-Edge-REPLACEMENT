# Sony Camera Imaging Edge Replacement

This workspace contains a Windows-first replacement workflow for Sony Imaging Edge Mobile, centered on a Sony a6400, a local browser bridge, and an optional Frigate integration that can trigger recording on the camera itself.

## What works now

- Local live preview bridge from Sony's Wi-Fi JSON-RPC API via [ImagingEdge4Linux/liveview_webui.py](ImagingEdge4Linux/liveview_webui.py)
- Browser UI for preview, status, half-press, shutter, and start/stop movie recording
- Transfer-mode browsing and bulk ZIP download of files exposed by the camera's SOAP transfer service
- HTTP-to-RTSP relay for Frigate via [ffmpeg_rtsp_relay_loop.ps1](ffmpeg_rtsp_relay_loop.ps1) and [ImagingEdge4Linux/mediamtx.yml](ImagingEdge4Linux/mediamtx.yml)
- Frigate event polling that can request on-camera SD-card recording via [frigate_trigger_sony_sd_record.ps1](frigate_trigger_sony_sd_record.ps1)

## Current architecture

There are two distinct Sony workflows and they must not be mixed:

- Preview Stream Mode: camera menu `Ctrl w/ Smartphone`; used for liveview and JSON-RPC controls
- Transfer Mode: camera menu `Send to Smartphone / Sharing`; used for SOAP file listing and download

Main files:

- [ImagingEdge4Linux/liveview_webui.py](ImagingEdge4Linux/liveview_webui.py): Sony bridge and browser UI
- [start_sony_frigate_wizard_bridge.ps1](start_sony_frigate_wizard_bridge.ps1): launches bridge, MediaMTX, ffmpeg relay, and optional Frigate SD-trigger loop
- [frigate_trigger_sony_sd_record.ps1](frigate_trigger_sony_sd_record.ps1): polls Frigate events and calls the bridge start/stop movie endpoints
- [LIVEVIEW_REVERSE_ENGINEERING_REPORT.md](LIVEVIEW_REVERSE_ENGINEERING_REPORT.md): packet-level notes and quality-limit findings

## Frigate integration

The intended Frigate flow is:

1. Frigate reads the relay stream from the local bridge.
2. Frigate detects objects on that relay stream.
3. Detection events are polled locally.
4. The trigger loop calls the Sony bridge to start or stop movie recording on the camera SD card.

Important: the goal is not to save the low-quality preview stream as the final recording. The preview stream is detection input only; the desired final recording is made by the camera itself.

The current local defaults were set up for this environment:

- Frigate username: `sonyguard`
- Frigate password: `SonyGuard!2026`

If this repository is pushed anywhere outside a private environment, rotate those values first.

## Known limits

### Liveview quality

The Sony Wi-Fi liveview endpoint still appears to be limited to Sony's proprietary framed JPEG preview stream. Current reverse-engineering work did not find a public JSON-RPC method like `startLiveviewWithSize`, `getAvailableLiveviewSize`, or similar on this camera/workflow.

Practical conclusion:

- The bridge can stabilize and relay the preview stream.
- Frigate can consume that preview stream.
- No confirmed higher-quality Wi-Fi liveview path has been found yet for the a6400 through the exposed JSON-RPC API.

Likely escalation paths, if better video is still required:

- USB/PTP or service-mode work similar to PMCA / OpenMemories research
- HDMI capture for true higher-quality live monitoring
- Camera-side firmware/service-mode investigation rather than ordinary Wi-Fi API calls

### SD-record trigger reliability

The automation framework is in place, but the Sony control path still has an intermittent issue: preview frames can continue flowing while `startMovieRec` occasionally fails or hangs. The bridge now treats recent frame activity as proof that the Wi-Fi Direct session is still alive, which avoids some false reconnect attempts, but this path still needs real-world validation under motion events.

## Running locally

Typical local startup:

- Put the camera in `Ctrl w/ Smartphone`
- Run [start_sony_frigate_wizard_bridge.ps1](start_sony_frigate_wizard_bridge.ps1)
- Open the bridge UI or point Frigate at the emitted RTSP URL

When you want to browse camera files instead:

- Put the camera in `Send to Smartphone / Sharing`
- Use the Transfer Mode controls in the bridge UI

## Related upstream work

- [ImagingEdge4Linux/README.md](ImagingEdge4Linux/README.md)
- `ma1co/Sony-PMCA-RE`
- `frank26080115/alpha-fairy`

Those projects are still the best references for Sony protocol internals beyond the exposed Wi-Fi JSON-RPC surface.
