# Third-party notices

## ESP8266Audio

- Project: https://github.com/earlephilhower/ESP8266Audio
- Version: 1.9.7
- License: GNU General Public License v3.0 (GPL-3.0)
- Used for MP3, WAV, FLAC, AAC and Ogg/Opus decoding, plus HTTP/ICY stream input.

The firmware links against ESP8266Audio. The upstream license text and source are available from the project above. Redistribution of binaries that include this library must comply with GPL-3.0, including the applicable corresponding-source and license-notice requirements.

## M5Cardputer

- Project: https://github.com/m5stack/M5Cardputer
- Version requirement in PlatformIO: ^1.1.1
- License: see the upstream repository's license files.

The JOHNNY audio output adapter is written for this firmware and passes decoded PCM samples to M5Unified's Cardputer speaker output. No ESP32-audioI2S or PSRAM-only audio stack is included.
## AAC decoder notice

ESP8266Audio bundles an AAC decoder derived from the Helix project. Its upstream README identifies the AAC decoder code as subject to RealNetworks' RSPL license and notes that commercial AAC use may require separate patent licensing through Via Licensing. Review the upstream ESP8266Audio license and the Helix/RSPL terms before redistributing builds with AAC enabled, especially for commercial distribution.


## DOOM WebAssembly demo (external embed)

- Project: https://github.com/jacobenget/doom.wasm
- License: GNU General Public License v2.0 (GPL-2.0).
- The Games page embeds the upstream browser demo from https://jacobenget.github.io/doom.wasm/examples/browser/doom.html in an iframe. The engine/WASM binary is served by the upstream project; this repository does not store or redistribute its engine binary.
- The upstream demo uses its own Doom shareware game data. This repository does not bundle or redistribute commercial DOOM game data.
