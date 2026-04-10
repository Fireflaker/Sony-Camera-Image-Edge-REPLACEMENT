# Encoder Hardening Notes (v4a)

## Summary
This update hardens the Nikon VP485-to-OLED workflow so the display remains stable during noisy or low-confidence samples.

## Firmware Changes
File: `src/main.cpp`

1. Added speed and plausibility gating in `updateVpAngleFromWord(...)`.
2. Added trusted/rejected sample counters:
   - `vpTrustedSamples`
   - `vpRejectedSamples`
3. Added quality helper:
   - `vpTrustedPercent()`
4. Added rejection criteria:
   - Reject jumps larger than 120 deg when `dtUs < 40000`.
   - Reject samples when instantaneous speed exceeds 1800 deg/s.
5. Updated OLED line 2 to show trust quality:
   - `Now:%6.2f Q:%3d%%`
6. Extended serial telemetry with quality accounting:
   - `quality=%d%% trusted=%lu rejected=%lu`
7. Reset trusted/rejected counters when mode `v` starts.

## Why This Helps
- Prevents implausible spikes from polluting the integrated angle.
- Keeps speed readout calm during invalid samples.
- Makes runtime confidence visible both on OLED and serial logs.

## Operational Notes
- For mode `v` verification, look for:
  - `Absolute Encoder Probe v4a`
  - `OLED fast update`
  - `OLED candidate updated`
- In PowerShell, redirected logs can be UTF-16LE. Use encoding-aware searches when grepping captures.
