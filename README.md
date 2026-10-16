# MP3 Player — M5Stack Cardputer ADV

**Firmware for playing MP3 music on the M5Stack Cardputer ADV (ESP32-S3).**

MP3 Player is a dedicated music-player firmware. It reads MP3 files from a microSD card and provides a compact library browser and playback interface designed for the Cardputer ADV hardware.

## Features

- Browse microSD folders containing MP3 files, including nested folders.
- Build a playback queue from the selected folder.
- Play, pause, stop, next/previous track and automatic track advance.
- Shuffle and repeat modes.
- Seek backward and forward, track duration and progress.
- ID3 title/artist metadata and embedded JPEG/PNG album art.
- Five-band equalizer with six presets and per-band adjustment.
- Audio visualizers: bars, mirror, waveform and album cover.
- Sleep timer, screen timeout, brightness and optional shake-to-skip.
- Settings saved to device preferences.
- Five selectable themes: Dark Red, Matrix, Cyberpunk, Ice Blue and Amber.
- Cardputer ADV RGB LED follows the selected theme and pulses with music.

## Hardware

- M5Stack Cardputer ADV / ESP32-S3
- microSD card containing .mp3 files
- Built-in speaker or headphones

microSD SPI pins: CS GPIO12, SCK GPIO40, MISO GPIO39, MOSI GPIO14. The RGB LED is driven on GPIO21.

## Build

Install PlatformIO with `python -m pip install --user platformio`, then run `pio run -e cardputer-adv` from the repository directory.

USB upload: `pio run -e cardputer-adv -t upload`.

Dependencies are managed by PlatformIO: M5Stack M5Cardputer and ESP8266Audio.

## Keyboard

- `;` / `.`: move through the library or adjust volume
- `,` / `/`: page through the library or seek
- `ENTER`: open folder / play selected track
- `SPACE`: pause or resume
- `N` / `P`: next / previous track
- `TAB`: open player or switch player/equalizer
- `S`: settings
- `E`: equalizer
- `X`: stop playback
- `1`–`6`: equalizer presets

## Stability notes

The MP3 decoder is advanced in short cooperative slices so the UI can continue processing input. Audio output uses fixed-size buffers and a bounded queue. Playback should still be validated on the physical Cardputer ADV with the target microSD card, speaker and headphones; a successful CI build alone does not prove hardware stability.