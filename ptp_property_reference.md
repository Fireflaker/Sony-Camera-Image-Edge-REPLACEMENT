# Sony PTP Device Property Reference — a6400 / Alpha Series

Cross-reference of all Sony PTP property codes (`0xD200`–`0xD2D9`) decoded from:

- **[libgphoto2 ptp.h](https://raw.githubusercontent.com/gphoto/libgphoto2/master/camlibs/ptp2/ptp.h)** — authoritative Sony property names
- **[Parrot sequoia-ptpy](https://github.com/Parrot-Developers/sequoia-ptpy/blob/master/ptpy/extensions/sony.py)** — alternative Sony extension reference
- **Live `gphoto2 --list-all-config` dump from ILCE-6400 connected via USB PTP**

Generated: 2026-03-14 | Camera: ILCE-6400, firmware 2.0

---

## PTP Opcodes (Sony Proprietary — USB transport, opcode range 0x9200–0x9209)

| Opcode | libgphoto2 Name | Description |
| --- | --- | --- |
| `0x9201` | `SONY_SDIO_Connect` | Auth handshake. Call with params `(1,0,0)`, then `(2,0,0)`, then `(3,0,0)` |
| `0x9202` | `SONY_SDIO_GetExtDeviceInfo` | Returns list of supported property codes |
| `0x9203` | `SONY_GetDevicePropdesc` | Get full descriptor for one property |
| `0x9204` | `SONY_GetDevicePropertyValue` | Get current value for one property |
| `0x9205` | `SONY_SDIO_SetExtDevicePropValue` | **SetControlDeviceA** — immediate set (exposure, ISO, shutter speed, aperture) |
| `0x9206` | `SONY_GetControlDeviceDesc` | Get control descriptor |
| `0x9207` | `SONY_SDIO_ControlDevice` | **SetControlDeviceB** — queued/latching set (shutter press, MF drive, movie) |
| `0x9209` | `SONY_SDIO_GetAllExtDevicePropInfo` | Bulk read all properties at once (~4126 byte blob) |
| `0x9210` | `SONY_SDIO_OpenSession` | Extended session open |
| `0x9211` | `SONY_SDIO_GetPartialLargeObject` | Partial large object transfer |
| `0x9215` | `SONY_SDIO_GetDisplayStringList` | Get display string list |
| `0x9223` | `SONY_SDIO_GetLensInformation` | Lens info |

---

## SetControlDeviceA vs SetControlDeviceB

**SetControlDeviceA (0x9205)** — Immediate value set, used for:

- Exposure settings (ISO, shutter speed, aperture, EV compensation)
- Image quality settings
- White balance
- Drive mode

**SetControlDeviceB (0x9207)** — Queued/latching, used for:

- Shutter button simulation (half-press, full-press, release)
- Movie record start/stop
- Manual focus drive steps
- Remote control buttons (D-pad, AEL, FEL, AWB lock)

---

## Device Properties — Status / Read-Only

| Code | libgphoto2 Name | gphoto2 Path | Current (a6400) | Notes |
| --- | --- | --- | --- | --- |
| `0xD213` | `FocusFound` | `/main/other/d213` | 1 | 1=searching, 2=locked, 3=failed |
| `0xD214` | `Zoom` | `/main/other/d214` | 3.44975e+07 | Focal length × 1,000,000 (34.4mm) |
| `0xD215` | `ObjectInMemory` | `/main/other/d215` | 0 | Non-zero = new image ready to download |
| `0xD217` | `AELockIndication` | `/main/other/d217` | 1 | 1=unlocked, 2=locked |
| `0xD218` | `BatteryLevel` | `/main/other/d218` | 100 | 0–100 percent |
| `0xD219` | `SensorCrop` | `/main/other/d219` | 2 | 1=full-frame, 2=APS-C crop |
| `0xD21D` | `MovieRecordingState` | `/main/other/d21d` | 0 | 0=stopped, 1=recording, 2=paused |
| `0xD21F` | `FELockIndication` | `/main/other/d21f` | 1 | Flash Exposure Lock: 1=off, 2=locked |
| `0xD221` | `LiveViewStatus` | `/main/other/d221` | 0/1/2 | 0=off, 1=active |
| `0xD20E` | `BatteryLevelIndicator` | `/main/other/d20e` | (enum 0–14) | Display-oriented battery indicator |
| `0xD216` | `ExposeIndex` | `/main/other/d216` | — | Exposure index counter |
| `0xD24E` | `AWBLockIndication` | `/main/other/d24e` | 1 | AWB Lock: 1=off, 2=locked |
| `0xD250` | `IntervalRECStatus` | `/main/other/d250` | 0 | 0=stopped, 1=interval recording running |

---

## Device Properties — Read/Write (SetControlDeviceA)

| Code | libgphoto2 Name | gphoto2 Path | a6400 Value | Notes |
| --- | --- | --- | --- | --- |
| `0xD200` | `DPCCompensation` | `/main/other/d200` | 0 | Exposure compensation (DPC) |
| `0xD201` | `DRangeOptimize` | `/main/other/d201` | 31 | D-Range Optimizer / Auto HDR; 1=off, 16=auto, 17–22=Lv1–Lv5, 31=Auto HDR |
| `0xD203` | `ImageSize` | `/main/other/d203` | — | JPEG image size |
| `0xD20D` | `ShutterSpeed` | `/main/capturesettings/shutterspeed` | 1/30 | Encoded Sony shutter speed value |
| `0xD20F` | `ColorTemp` | — | — | White balance color temperature |
| `0xD210` | `CCFilter` | — | — | CC filter |
| `0xD211` | `AspectRatio` | — | — | Image aspect ratio |
| `0xD21B` | `PictureEffect` | — | — | Picture Effect / Creative Style |
| `0xD21C` | `ABFilter` | — | — | AB filter (white balance fine-tune) |
| `0xD21E` | `ISO` | `/main/other/d21e` | 1250 | ISO value; 0=AUTO |
| `0xD222` | `StillImageStoreDestination` | `/main/other/d222` | 1 | 1=card+PC, 17=PC only, 16=card only |
| `0xD224` | `ExposureCompensation` | — | — | EV compensation |
| `0xD231` | `LiveViewSettingEffect` | `/main/other/d231` | 1 | 1=setting effect on, 2=off |
| `0xD24F` | `IntervalRECModel` | `/main/other/d24f` | 1 | 1=off, 2=interval recording mode |

---

## Device Properties — Control (SetControlDeviceB)

These are "button" properties written as 1=press or 2=release (some cameras use 1=activate only).

| Code | libgphoto2 Name | gphoto2 Path | Semantics |
| --- | --- | --- | --- |
| `0xD2C1` | `ShutterHalfRelease` | `/main/actions/autofocus` | **S1 half-press** — 2=press, 1=release |
| `0xD2C2` | `ShutterRelease` | `/main/actions/capture` | **S2 full-press** — 2=press, 1=release |
| `0xD2C3` | `AELButton` | `/main/other/d2c3` | AE Lock button — 2=press, 1=release |
| `0xD2C4` | `AFLButton` | — | AF Lock button |
| `0xD2C5` | `ReleaseLock` | `/main/other/d2c5` | Release lock toggle |
| `0xD2C7` | `RequestOneShooting` | `/main/actions/stillimage` | **Trigger one still shot** — write 1 |
| `0xD2C8` | `MovieRecButtonHold` | `/main/actions/movie` | **Movie record** — 1=start, 2=stop |
| `0xD2C9` | `FELButton` | `/main/other/d2c9` | Flash Exposure Lock button |
| `0xD2CA` | `FormatMedia` | — | Format memory card |
| `0xD2CB` | `FocusMagnifier` | `/main/other/d2cb` | Enter focus magnifier — write 1 |
| `0xD2CC` | `FocusMagnifierCancel` | `/main/other/d2cc` | Exit focus magnifier — write 1 |
| `0xD2CD` | `RemoteKeyUp` | `/main/other/d2cd` | D-pad Up — 2=press, 1=release |
| `0xD2CE` | `RemoteKeyDown` | `/main/other/d2ce` | D-pad Down |
| `0xD2CF` | `RemoteKeyLeft` | `/main/other/d2cf` | D-pad Left |
| `0xD2D0` | `RemoteKeyRight` | `/main/other/d2d0` | D-pad Right |
| `0xD2D1` | `ManualFocusAdjust` | `/main/other/d2d1` | **MF drive steps** — range **-7 to +7** (negative=near, positive=far) |
| `0xD2D2` | `AFMFHold` | `/main/other/d2d2` | AF/MF toggle hold |
| `0xD2D3` | `CancelPixelShiftShooting` | `/main/other/d2d3` | Cancel pixel shift — write 1 |
| `0xD2D4` | `PixelShiftShootingMode` | `/main/other/d2d4` | Pixel shift on/off |
| `0xD2D9` | `AWBLButton` | `/main/other/d2d9` | AWB Lock button — 2=press, 1=release |

---

## Device Properties — Focus Magnifier Group

| Code | libgphoto2 Name | Notes |
| --- | --- | --- |
| `0xD22D` | `FocusMagnifierStatus` | 0=off, 1=active, 2=magnified view |
| `0xD22F` | `CurrentFocusMagnifierRatio` | Magnification ratio (e.g. 59 ≈ 5.9×) |
| `0xD230` | `FocusMagnifierPosition` | XY position of magnifier box (packed uint32) |

---

## Device Properties — Still Unknown

These codes appeared in the live a6400 `gphoto2 --list-all-config` dump but are NOT yet in libgphoto2's `ptp.h`. Best guesses from value ranges:

| Code | Value Range on a6400 | Best Guess |
| --- | --- | --- |
| `0xD212` | enum 0–15, current ~3 | **DriveMode** (single/burst/bracket/self-timer) |
| `0xD22E` | 0–255, current 59 | **CurrentFocusMagnifierPhase** or step position |
| `0xD232` | large uint32 (≈0x013FFFFF) | **FocusMagnifierPosition2** or display bounding box |
| `0xD233` | 0/1 | **FocusMagnifierLockState** |
| `0xD235` | 0/1 | `ManualFocusAdjustEnableStatus` (confirmed in newer libgphoto2 versions) |
| `0xD236` | 0/1 | **SteadyShotStatus** or NR mode indicator |

---

## Capture Sequences

### Still photo (PTP)

```python
# Step 1: Half-press (AF engage)
SetControlDeviceB(0xD2C1, 2)   # press S1
time.sleep(0.5)

# Step 2: Full-press (capture)
SetControlDeviceB(0xD2C2, 2)   # press S2
time.sleep(0.3)

# Step 3: Release
SetControlDeviceB(0xD2C2, 1)   # release S2
SetControlDeviceB(0xD2C1, 1)   # release S1

# Poll 0xD215 (ObjectInMemory) until non-zero, then download
```

### One-shot still (simpler)

```python
SetControlDeviceA(0xD2C7, 1)   # RequestOneShooting
```

### Movie record

```python
SetControlDeviceB(0xD2C8, 1)   # start recording
# ... wait ...
SetControlDeviceB(0xD2C8, 2)   # stop recording
```

### Manual focus drive

```python
# Focus near 3 steps
SetControlDeviceB(0xD2D1, -3)

# Focus far 7 steps (maximum)
SetControlDeviceB(0xD2D1, 7)
```

---

## Parrot sequoia-ptpy vs libgphoto2 — Name Mapping

The Parrot library used drone-oriented labels. libgphoto2 has the canonical camera names:

| Code | Parrot Label | libgphoto2 Canonical Name |
| --- | --- | --- |
| `0xD2C1` | `AutoFocus` | `ShutterHalfRelease` (S1 half-press) |
| `0xD2C2` | `Capture` | `ShutterRelease` (S2 full-press) |
| `0xD2C7` | `StillImage` | `RequestOneShooting` |
| `0xD2C8` | `Movie` | `MovieRecButtonHold` |

---

## References

- [libgphoto2 ptp.h](https://github.com/gphoto/libgphoto2/blob/master/camlibs/ptp2/ptp.h) — Sony PTP property defines (`PTP_DPC_SONY_*`)
- [Parrot sequoia-ptpy sony.py](https://github.com/Parrot-Developers/sequoia-ptpy/blob/master/ptpy/extensions/sony.py)
- [alpha-fairy Camera Reverse Engineering](https://github.com/frank26080115/alpha-fairy/blob/main/doc/Camera-Reverse-Engineering.md)
- [OpenMemories: Tweak](https://github.com/ma1co/OpenMemories-Tweak) — Sony camera app framework
