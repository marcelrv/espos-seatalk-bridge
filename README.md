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

## WiFi and OTA

```powershell
python tools/provision_wifi.py --port COMx     # WiFi from ../st50-emulator/secrets.ini into NVS (resets NVS)
.\tools\ota.ps1 -Device espos-f768.local       # serve the build from this PC and install it over WiFi
```

The device pulls the image over HTTP from this PC (port 8070; allow Python
through the Windows firewall on private networks). A new image that doesn't
reach the network within 10 minutes rolls back to the previous one.

On the CH340 DevKit the automatic reset into download mode is unreliable:
hold BOOT, tap EN, release BOOT. Flash at 115200 baud; 460800 gave serial
errors.

## Bench console

Over WiFi (telnet, port 23, unauthenticated: bench use only) or USB serial:

```powershell
python tools/stconsole.py -k "t" -s 3        # statistics
python tools/stconsole.py -k "v" -s 2        # simulated boat on/off (values to the ST50)
python tools/stconsole.py -k "d" -s 3        # per-datagram table (mode, rate, sent, stale, native)
python tools/stconsole.py -k "k" -s 8        # PHY self-test
```

`h` lists all keys. ST50 notes (measured): heading is read from 89 only,
and when data stops the ST50 keeps the last values for ~40 s before it
shows dashes.

## Host tests

```powershell
.\tools\host-test.ps1        # CMake + MSVC, runs ctest (build output in D:\Programming\esp\builds\host-test)
```

## Back to the emulator

Flash `../st50-emulator/firmware/st50-emulator-merged.bin` at 0x0, or
`pio run -t upload` in `../st50-emulator`.
