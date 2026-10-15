# JOHNNY v0.0.0-alpha.12

Target: M5Stack Cardputer ADV (ESP32-S3, 8 MB flash, no PSRAM).

## Changes
- Reserve the MP3 decoder workspace statically to avoid playback-time heap fragmentation.
- Reduce the shared audio input buffer to 4 KiB.
- Add diagnostics for decoder initialization, audio format, file size, free heap and largest allocatable block.
- Keep firmware version sourced from the repository VERSION file.

## Validation status
The GitHub Actions build is the release gate. This version must not be considered hardware-verified until it has compiled successfully and has been tested on a physical Cardputer ADV.
