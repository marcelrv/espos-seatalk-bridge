# seatalk-bridge

SeaTalk 1 <-> Signal K bridge firmware for the SeaTalk ESP32 Remote board
(classic ESP32). Built on [espOS](https://github.com/signalk-espOS/espOS)
(ESP-IDF 6.0), included as the `espos/` submodule.

- Design: [../SeaTalkCore_SPEC.md](../SeaTalkCore_SPEC.md)
- Hardware: [../SeaTalkRemote_FIRMWARE.md](../SeaTalkRemote_FIRMWARE.md)
- Bench reference: [../st50-emulator/](../st50-emulator/)

## Build (Windows, PowerShell)

ESP-IDF v6.0.3 is installed in `D:\Programming\esp` (tools in
`D:\Programming\esp\.espressif`). Build output goes outside the synced
source tree.

```powershell
git submodule update --init
. .\tools\idf-env.ps1
idf.py -B D:\Programming\esp\builds\seatalk-bridge set-target esp32   # once
idf.py -B D:\Programming\esp\builds\seatalk-bridge build
idf.py -B D:\Programming\esp\builds\seatalk-bridge -p COMx flash monitor
```

The first configure generates a development signing key,
`secure_boot_signing_key.pem` (git-ignored). A board flashed with it only
accepts OTA images signed with the same key: back it up.

## Back to the emulator

Flash `../st50-emulator/firmware/st50-emulator-merged.bin` at 0x0, or
`pio run -t upload` in `../st50-emulator`.
