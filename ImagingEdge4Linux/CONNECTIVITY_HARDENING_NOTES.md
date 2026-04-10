# Connectivity Hardening Notes

## Scope
This note documents the latest reliability updates in `liveview_webui.py` for Sony Wi-Fi Direct control continuity.

## Changes

1. SSID normalization for Linux `nmcli -t` parsing.
   - Added `_normalize_ssid()` to unescape escaped separators (for example `\:`).
   - Applied normalization while extracting `DIRECT-*` SSIDs.

2. Camera control port reachability check.
   - Added `_camera_control_port_reachable()` using a short TCP connect probe.
   - Used as an additional signal that the camera control path is alive even when OS SSID reporting is stale.

3. Connectivity gate refinement in `ensure_connected()`.
   - Clears `last_camera_error` and returns success when any of the following are true:
     - Adapter reports connected SSID.
     - Camera control port is reachable.
     - Recent frame activity indicates active session.

## Why
Some Windows/Linux adapter states can briefly misreport SSID even while Sony control/liveview is healthy. The updated logic reduces false reconnect behavior and protects in-progress control operations.
