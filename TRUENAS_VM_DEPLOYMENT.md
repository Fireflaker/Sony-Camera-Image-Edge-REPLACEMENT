# TrueNAS SCALE Sony Bridge VM Deployment

This runbook is for the isolated Ubuntu VM named `sonybridge` running under Incus on TrueNAS SCALE.

## Why this layout

- Keeps TrueNAS host changes minimal
- Keeps Sony Wi-Fi Direct handling inside the guest
- Makes the bridge/debug page movable and recoverable

## Guest prerequisites

Inside the Ubuntu guest, the Realtek `0bda:8812` adapter needs both:

- the packaged `rtl8812au-dkms` driver
- the matching `linux-modules-extra-$(uname -r)` package so Linux wireless stack modules like `cfg80211` and `mac80211` are available

If `iw dev` reports `nl80211 not found`, install the kernel extra modules first.

## Deploy the bridge in the guest

Clone the repo into `/opt/sonybridge` and run:

```bash
cd /opt/sonybridge/ImagingEdge4Linux
sudo bash ./bootstrap_sonybridge_vm.sh
```

Then configure the service:

```bash
sudo cp imagingedge-liveview.service /etc/systemd/system/
sudo cp imagingedge-liveview.env.example /etc/default/imagingedge-liveview
sudoedit /etc/default/imagingedge-liveview
sudo systemctl daemon-reload
sudo systemctl enable --now imagingedge-liveview
```

### Recovery: missing `/opt/sonybridge/ImagingEdge4Linux`

If you get `No such file or directory` when trying to `cd /opt/sonybridge/ImagingEdge4Linux`, run this on the TrueNAS host shell:

```bash
incus exec sonybridge -- bash -lc '
set -euo pipefail
mkdir -p /opt/sonybridge
if [ ! -d /opt/sonybridge/ImagingEdge4Linux/.git ]; then
  rm -rf /opt/sonybridge/ImagingEdge4Linux
  git clone https://github.com/Fireflaker/Sony-Camera-Image-Edge-REPLACEMENT.git /opt/sonybridge/ImagingEdge4Linux
else
  git -C /opt/sonybridge/ImagingEdge4Linux pull --ff-only
fi
cd /opt/sonybridge/ImagingEdge4Linux/ImagingEdge4Linux
chmod +x bootstrap_sonybridge_vm.sh run_liveview.sh
./bootstrap_sonybridge_vm.sh
cp imagingedge-liveview.service /etc/systemd/system/
cp imagingedge-liveview.env.example /etc/default/imagingedge-liveview
systemctl daemon-reload
systemctl enable --now imagingedge-liveview
systemctl --no-pager --full status imagingedge-liveview
'
```

## Verify Wi-Fi support

```bash
lsusb | grep -i 0bda:8812
lsmod | egrep '8812au|cfg80211|mac80211'
iw dev
nmcli device status
```

A healthy setup should show a Wi-Fi interface in `iw dev` and `nmcli`.

## Service endpoint

The bridge service listens on `0.0.0.0:8765` by default.

Useful endpoints:

- `http://VM_IP:8765/`
- `http://VM_IP:8765/stream`
- `http://VM_IP:8765/frame.jpg`
- `http://VM_IP:8765/api/status`

## Notes

- `liveview_webui.py` now supports `--wifi-interface auto` and Linux `nmcli`-based Wi-Fi connection management.
- The VM still needs a reachable wireless interface before camera auto-connect can succeed.
- RTSP/Frigate exposure can be added after the HTTP bridge is confirmed working.
