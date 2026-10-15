# JOHNNY — Cardputer ADV

**Version 1.0.0-alpha.7 · ESP32-S3 · 8 MB Flash · no PSRAM**

JOHNNY is a multi-tool firmware for the M5Stack Cardputer ADV. Its interface uses a black/crimson visual system, compact screens, a horizontal category launcher and managed audio workers.

## Music

- Synth: eight timbres, chromatic keyboard input, sharps/flats and seven sample pads.
- Tuner: guitar, bass and ukulele presets with nearest-note and cents guidance.
- Music player: local microSD library, metadata/cover support, playback controls, shuffle and repeat.
- Web radio: station presets and custom HTTP stream URL.
- Five-band DSP equalizer with presets and per-band adjustment.
- Microphone recorder and WAV sample/pad recording to microSD.
- Radio stream recording to WAV on microSD.

## System

- Files browser and microSD operations.
- USB transfer mode / local web interface.
- Wi-Fi scan, network connection and connection details.
- Firmware update over Wi-Fi from published release.
- Device status, battery and storage diagnostics.
- Bluetooth LE device scanner.
- Clock with NTP synchronization and configurable alarms; MP3 alarm source supported.
- Disk manager with Flash/SD capacity information.
- Process/runtime monitor for JOHNNY-managed tasks.
- Six themes including the new JOHNNY RED default.

## Games

- Conway's Game of Life with editable grid, pause, step, speed and SD persistence.

## HACK Lab

- Passive Wi-Fi reconnaissance and BLE scanning.
- Single-target TCP connectivity test, DNS lookup and HTTP status check.
- Network/radio information and local web status/screen view.
- Offline visual effects.
- HTTP security audit and device security audit.

## Stability design

- One foreground module at a time; audio and recorder work run in managed tasks.
- Cooperative stop and teardown for audio, radio recording, microphone and BLE scanning.
- Fixed-size decoder, DSP and recording buffers sized for the 8 MB / no-PSRAM target.
- Wi-Fi is not started at cold boot; it starts when a network feature is opened.
- The microphone and speaker are switched explicitly because they share audio resources.

## Build on Arch Linux

```bash
python -m pip install --user platformio
cd ~/johnny
pio run -e cardputer-adv
```

USB upload:

```bash
pio run -e cardputer-adv -t upload
```

## Important limitations

- Web radio depends on Wi-Fi quality and the selected station; buffering reduces short dropouts but cannot guarantee uninterrupted streaming.
- NTP time must be synchronized again after complete power loss unless a supported RTC is added.
- Preferences, including saved Wi-Fi credentials and alarms, use the current Preferences/NVS implementation. The planned encrypted AES-GCM SD vault and secure migration are not implemented; do not describe current credential storage as an encrypted vault.
- Disk manager and process monitor are informational/limited; arbitrary FreeRTOS tasks are not force-terminated.
- External SPI display and full GPIO manager are not part of this build.
- CI compilation and image assembly do not prove hardware stability. Test cold boot, SD, MP3, radio, synth, pads, mic, tuner, Wi-Fi, BLE, OTA, alarms and repeated app open/close cycles on the physical Cardputer ADV.