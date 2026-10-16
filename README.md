# MP3 Player — M5Stack Cardputer ADV

**Firmware 12.0.0** — a dedicated local-music player for the M5Stack Cardputer ADV (ESP32-S3).

## Version 12 features

- MP3 playback with optional WAV, FLAC and AAC decoders when the installed ESP8266Audio library provides them.
- microSD and LittleFS browsing, nested folders, playback queue, metadata, embedded/folder cover art and SD-to-flash copy.
- Play/pause/stop, next/previous, grouped skip requests, MP3 seeking, repeat, shuffle and sleep timer.
- Speed presets: 7 = SLOWED 0.75x, 8 = SPED UP 1.25x, 9 = Nightcore 1.75x, 0 = normal.
- Five-band equalizer, six presets, independent speaker/headphone volume, output profiles, DC blocking, smooth compression, virtual bass, peak limiting and soft clipping.
- Bars, mirror, waveform and cover-art visualizer modes; five colour themes and activity-reactive RGB LED.
- Buffered SD reads, automatic remount/recovery with slower SPI fallback, bounded directory scanning and cleanup of incomplete copies.
- USB mass-storage mode for the microSD card, enabled in the PlatformIO USB-OTG/TinyUSB configuration.
- Adaptive CPU and optional-memory management for the ESP32-S3.

## Controls

Browser: ; / . move, , / / page, ENTER open/play, DEL parent folder, M switch SD/Flash, C copy selected SD file to Flash, U USB mass storage, TAB player, S settings, E equalizer, R rescan, SPACE pause, N / P next/previous, X stop.

Player: SPACE pause, N / P next/previous, , / / seek by 10 seconds, ; / . volume, S shuffle, R repeat, V visualizer, T sleep timer, O output profile, 7 / 8 / 9 / 0 speed presets, TAB equalizer, DEL return to browser.

Equalizer: , / / select band, ; / . adjust gain, 0 flat preset, TAB / DEL return. Press H for the on-device help pages.

## Hardware and storage

- Target: M5Stack Cardputer ADV / ESP32-S3, 8 MB flash.
- microSD: CS GPIO12, SCK GPIO40, MISO GPIO39, MOSI GPIO14.
- RGB LED: GPIO21.
- Internal storage uses LittleFS. Card capacity and free space depend on the selected partition table and card.

USB mass storage uses USB-OTG/TinyUSB. The PlatformIO profile explicitly sets ARDUINO_USB_MODE=0 and ARDUINO_USB_CDC_ON_BOOT=1; do not change the USB mode to Hardware CDC/JTAG if the USB disk feature is required. Eject the card from the computer before leaving USB mode.

## Build

PlatformIO is the official build path: pio run -e cardputer-adv.

The project version is read from VERSION and injected into mp3_player_version.h by scripts/version.py. src/main.cpp is the PlatformIO entry point. MP3_Player.ino is kept in sync as the Arduino IDE sketch source.

The Build and publish MP3 PLAYER firmware GitHub Actions workflow compiles the firmware, assembles the complete ESP32-S3 flash image and publishes the versioned GitHub release. The Pages workflow then downloads that release asset and deploys the site installer with the version in the manifest. The installer is only updated after the build and release workflow succeeds.

## Install

Open https://otavio-dias-leite.github.io/mp3player/ in Chrome or Edge. Connect the Cardputer ADV by USB, hold G0 while connecting to enter download mode, select the serial port and start flashing. Do not disconnect during the process.

For manual recovery, download mp3-player-cardputer-adv-full.bin from https://github.com/otavio-dias-leite/mp3player/releases/latest and flash it at address 0x0 with esptool for ESP32-S3.

## Format limitations and validation

MP3 is the built-in decoder. WAV, FLAC and AAC are conditional on decoder headers supplied by the selected ESP8266Audio release. M4A, OGG and OPUS are not supported by this firmware. A successful CI build verifies compilation and image assembly; it does not replace a physical Cardputer ADV test of speaker/headphone output, SD recovery, USB host compatibility and long-duration playback.
