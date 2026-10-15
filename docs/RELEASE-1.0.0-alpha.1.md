# JOHNNY 1.0.0-alpha.1

This is the first 1.0 alpha firmware for M5Stack Cardputer ADV / ESP32-S3. It is a functional development build, not a final stability-certified release.

## Included in this alpha

- Horizontal, one-module-at-a-time launcher and one-large-card category navigation.
- Large music now-playing screen with album art area, title, artist, album, progress, playback state and controls.
- MP3-only local decoder (the configured ESP8266Audio dependency instantiates MP3); unsupported files are filtered instead of being advertised as playable.
- Audio decoder isolated on a FreeRTOS task away from the UI core. The previous forced speaker stop was removed. Decoder shutdown is cooperative with a 650 ms UI-side timeout; a timeout prevents new SD work from starting concurrently.
- 12 KiB compressed-stream buffer for local playback and web radio. This adds buffering at the cost of RAM and start latency; it cannot compensate for every network dropout.
- Synth with eight generated timbres, chromatic note input, sharps on number keys, flats on S/A/L, and seven Z-X-C-V-B-N-M WAV pads. A pad can be recorded from the microphone to /JOHNNY/PADS/; pad playback reads at most 8,192 mono PCM samples into a fixed buffer.
- Original, locally synthesized startup jingle. It does not reproduce the melody or recording from Cyberpunk 2077 / Samurai.
- NTP clock and repeating alarm with weekday presets, snooze, built-in tone and optional MP3 selected from the music player. The time zone uses BRT3 (UTC-3), the current Sao Paulo offset.
- Six color themes, basic Flash/SD capacity display, runtime heap/task view and charging screensaver.

## Known limitations — not implemented yet

- No RTC-backed timekeeping after complete power loss. A fresh NTP sync is required after reboot unless the platform time remains valid.
- Wi-Fi credentials, alarms and preferences still use the existing NVS Preferences store; the planned encrypted AES-256-GCM SD vault, unlock flow and safe credential migration are not included.
- Disk Manager is a capacity/status view. Transactional copy/move/rename/delete UI is not included yet; the existing Files and USB transfer screens remain separate.
- Process view is observational and lists the two JOHNNY tasks created explicitly by this firmware. It does not safely stop arbitrary FreeRTOS tasks, and the new AppManager/ResourceManager foundation is not yet fully integrated into every legacy app.
- GPIO manager, RGB LED settings, external SPI display, external I2C arbitration and full per-app lifecycle migration are not included in this image.
- Radio depends on the station/server and Wi-Fi. A 12 KiB buffer reduces short jitter but does not guarantee gapless streaming. Only HTTP station URLs are configured by default.
- Local music playback is MP3 only. WAV is used for mic/pad recording and pad playback, not general music-library playback. FLAC/AAC/Vorbis are not enabled.

## Validation

CI firmware compilation and image assembly are required for this release. CI success does not equal physical testing. Hardware validation is still required for cold boot, the startup jingle, SD mount, repeated track changes, long MP3 playback, radio dropouts, pad recording/playback, NTP, alarm preemption, charging detection and repeated app open/close cycles.

A claim of zero freezes or 100% operation cannot be established by static analysis or one successful build. The firmware uses fixed buffers, bounded loops/timeouts and isolated audio work to reduce risk, but a long stress test on the actual Cardputer ADV remains mandatory.
