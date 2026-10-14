# JOHNNY v0.0.0-alpha.13

Target: M5Stack Cardputer ADV (ESP32-S3, 8 MB flash, no PSRAM).

## Changes
- Reduce the static synthesizer sample buffers from 8,192 to 4,096 samples per voice buffer, saving 24 KiB of static RAM.
- Reduce the maximum in-memory album-cover payload from 40 KiB to 16 KiB.
- Redraw the boot splash only once; apply small local glitch strokes instead of repeatedly clearing the entire display.
- Set the boot subtitle to `wake fuck up samurai`.
- Keep MP3 decoder workspace statically reserved and the shared audio input buffer at 4 KiB.
- Add the GitHub Pages configure step to the deployment workflow so the site and web flasher are prepared before deploy.

## Validation
GitHub Actions compilation and Pages deployment are required before this version can be called published. A successful CI build does not establish physical Cardputer ADV validation. Test boot, audio playback, SD, Wi-Fi, BLE, microphone and OTA on the device before treating the firmware as hardware-verified.
