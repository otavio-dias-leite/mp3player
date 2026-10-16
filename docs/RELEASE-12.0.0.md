# MP3 Player 12.0.0 — release notes

Target: M5Stack Cardputer ADV / ESP32-S3.

## Playback and DSP

- 44.1 kHz output with cubic resampling and separate speed controls: 0.75x, 1.25x, 1.75x and 1.00x.
- Anti-alias filtering during accelerated playback.
- DC blocker, five-band EQ, six presets, output-specific voicing, loudness contour, smooth compressor, virtual bass, soft peak limiter and final soft clip.
- SLOWED-only reverb allocated on demand and released when not in use.
- Speaker queue back-pressure, buffered file reads and short decoder slices to keep the interface responsive.
- Playback clock follows source-content time when speed changes.

## Storage and recovery

- SD mount attempts begin at 16 MHz and fall back through lower SPI rates.
- Failed reads trigger a remount attempt and resume from the last byte position when the card is recoverable.
- Directory scans are incremental; SD probing uses backoff while disconnected.
- SD-to-LittleFS copies check available capacity, display progress and remove partial destination files on failure.
- USB mass-storage callbacks handle byte offsets and partial-sector read/modify/write operations; the exported medium is limited to 512-byte SD sectors.

## Interface and power

- M5Stack Cardputer keyboard UI with browser, player, equalizer, settings and help screens.
- Album metadata/covers, four visualizer modes, five themes and RGB LED activity.
- Resource manager reduces optional allocations when heap headroom is low; CPU frequency is raised during active work and reduced while idle.

## Key speed controls

- 7: SLOWED 0.75x
- 8: SPED UP 1.25x
- 9: Nightcore 1.75x
- 0: normal speed.

## Format support

MP3 is built in. WAV, FLAC and AAC are enabled only when the corresponding ESP8266Audio decoder headers are available. M4A, OGG and OPUS are unsupported.

## Validation boundary

The repository CI pipeline compiles the PlatformIO target and assembles the full flash image before publishing the release and site installer. This is not a substitute for physical validation on a Cardputer ADV. Test speaker/headphone switching, SD read recovery, USB mass-storage enumeration/eject, copy integrity and long-duration playback before treating all hardware paths as field-verified.
