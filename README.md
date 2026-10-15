# JOHNNY — Cardputer ADV

**Alpha 6 · ESP32-S3 · 8 MB Flash · sem PSRAM**

JOHNNY is a multi-tool firmware for the M5Stack Cardputer ADV. UI: graphite black, crimson neon, compact screens and one foreground activity at a time.

## Implemented in alpha.6

- MP3 player from microSD with fixed 4 KB input buffer, play/pause/stop and volume.
- SD browser with bounded directory depth and file listing.
- Monophonic keyboard synth with nine notes.
- Microphone tuner with approximate pitch estimation and note naming.
- Microphone recorder: two-second mono 16 kHz WAV capture to microSD.
- Wi-Fi passive network scan showing SSID and RSSI.
- Snake game with WASD controls.
- Settings for brightness, volume and crimson/amber accent, persisted to microSD as JOHNNY.CFG.
- Device diagnostics: firmware version, SD availability and free heap.
- Five-second JOHNNY / WAKE UP SAMURAI boot splash.

## Controls

- Home: `;` / `.` navigate; Enter opens.
- Lists: `;` / `.` select; Enter opens folder or plays MP3.
- MP3: Space pause/resume, P stop, `-` / `+` volume, Esc returns.
- Synth: A S D F G H J K L play notes.
- Tuner: play a note into the microphone; Esc exits.
- Recorder: R records a short WAV file; Esc exits.
- Snake: W A S D; Enter restarts after game over; Esc exits.
- Settings: `;` / `.` brightness, `-` / `+` volume, T accent, Enter saves.

## Cardputer ADV hardware

Target is M5Stack Cardputer ADV. SD SPI pins: CS GPIO12, SCK GPIO40, MISO GPIO39, MOSI GPIO14. Do not use reserved board pins as arbitrary GPIO. Mic and speaker share audio resources and are switched between modes.

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

## Planned / not yet implemented

Web radio, Wi-Fi credential onboarding, Bluetooth/BLE scan, equalizer, synth pads/recording, alarm/clock/NTP, OTA from device, USB file server, external display, process manager, encrypted vault, advanced disk manager, more games and GPIO lab remain planned. They are not represented as functional features in this release.

CI compilation does not prove physical hardware operation. Test boot, keyboard, SD, mic, speaker, tuner calibration and sustained MP3 playback on the actual Cardputer ADV before treating this build as stable.