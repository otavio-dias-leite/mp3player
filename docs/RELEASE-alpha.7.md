# JOHNNY 1.0.0-alpha.7

Restores the full multi-tool firmware implementation from the repository's prior comprehensive development line and applies the new black/crimson identity.

## Included modules

- Music: synth with timbres and pads, tuner, local MP3 player, web radio, five-band equalizer, microphone recorder and radio WAV recording.
- System: files, USB transfer, Wi-Fi, OTA update, device status, BLE devices, clock/alarms, disk manager, process monitor and six themes.
- HACK: Wi-Fi recon, BLE scanner, TCP connection test, DNS lookup, HTTP status, network/radio info, offline visual FX, HTTP security audit and device security audit.
- Games: Conway's Game of Life with interactive controls and persistence.
- Boot: five-second JOHNNY splash with WAKE UP SAMURAI and locally synthesized intro cue.

## Visual changes

- Default theme is now JOHNNY RED: black base, deep crimson panels, restrained neon red accents and warm high-contrast text.
- Other selectable themes remain available.
- Website and manifest are aligned to alpha.7.

## Hardware and stability notes

- Target: M5Stack Cardputer ADV, ESP32-S3, 8 MB flash, no PSRAM.
- Audio decoder and radio recorder use managed workers and bounded queues; app transitions request cooperative shutdown.
- Wi-Fi remains off at cold boot and starts only when a network feature is opened.
- Current saved Wi-Fi credentials/preferences use Preferences/NVS; the planned encrypted SD vault is not implemented.
- CI build success is not hardware validation. Physical testing is required for audio, microphone, SD, Wi-Fi, BLE, OTA, alarms, USB transfer and repeated app transitions.