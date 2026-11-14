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

`h` lists all keys. ST50 Multi notes (measured): heading is read from 89
only; it has no COG page (53 is still sent for other devices); CMG/DMG
are computed by the display, not sent by us; when data stops it keeps the
last values for ~40 s before it shows dashes.

## Signal K

The device publishes and subscribes through espOS (approve it once in
signalk-server under Security → Access Requests). Settings are in the espOS
web UI, namespace `seatalk`: per datagram type a mode (`off`, `to_sk`,
`to_bus`, `both`, `auto`) and transmit period, per value an optional
Signal K `$source` filter, plus idle wait, jitter and stale timeout.

- Values this device publishes appear as `$source` `<hostname>.XX`; the
  adapter ignores those on the way back (no SeaTalk → Signal K → SeaTalk loop).
- Without `headingMagnetic` / `courseOverGroundMagnetic` on the server, the
  magnetic value is computed from the true one minus `magneticVariation`.
- Diagnostics: `espos.<hostname>.seatalk.*` every 10 s.
- Substitutes (group `Substitutes`): `stw_from_sog` sends SOG as boat speed
  (datagram 20), `hdg_from_cog` sends COG as heading (89/9C), for displays
  without SOG/COG pages such as the ST50 Multi. `fallback` uses them only
  while the real value is stale, `replace` always.

Memory (classic ESP32, no PSRAM): about 42 KB free at rest, ~8 KB less
while a telnet session is open. `sdkconfig.defaults` trims espOS's Signal K
batch table to 16 slots for this.

## espOS patches

`patches/espos-*.patch` are local fixes to the `espos/` submodule, applied
automatically when CMake configures (skipped when already applied; the build
stops if one no longer applies after a submodule update). The submodule is
marked `ignore = dirty`, so the patched files don't show in `git status`.

- `espos-0001-schema-from-flash.patch`: the config page failed with
  `ESP_ERR_NO_MEM` because the 33 KB schema was copied into RAM. Upstream:
  signalk-espOS/espOS#309; drop the patch once that is merged and the
  submodule is updated.

## Host tests

```powershell
.\tools\host-test.ps1        # CMake + MSVC, runs ctest (build output in D:\Programming\esp\builds\host-test)
```

## Back to the emulator

Flash `../st50-emulator/firmware/st50-emulator-merged.bin` at 0x0, or
`pio run -t upload` in `../st50-emulator`.

## License

Apache-2.0 ([LICENSE](LICENSE)), the same as espOS. Every source file carries
SPDX headers; [REUSE.toml](REUSE.toml) covers the files that cannot.
