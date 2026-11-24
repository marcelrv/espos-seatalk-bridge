# seatalk-bridge

SeaTalk 1 <-> Signal K bridge firmware for the SeaTalk ESP32 Remote board
(classic ESP32) and the ESP32-S3 SuperMini. Built on [espOS](https://github.com/signalk-espOS/espOS)
(ESP-IDF 6.0), included as the `espos/` submodule.

The design notes live next to this folder in the seatalk-remote project:
`SeaTalkCore_SPEC.md` (design), `SeaTalkRemote_FIRMWARE.md` (hardware) and
`st50-emulator/` (bench reference).

## Status

Tested on the bench with an ST50 Multi display and a signalk-server:
- PHY self-test and soak runs on a real bus;
- Signal K → SeaTalk: depth, wind, speed, SOG, COG and heading shown on the
  ST50 and matching the server;
- the SOG/COG substitutes;
- memory stable over time.

Not yet proven on a boat over a longer period. SeaTalk → Signal K is
implemented but untested, because the bench has no device that sends data
(the ST50 only sends lamp datagrams). Treat it as experimental until it has
sailed a while.

The SeaTalk layers (`components/st_*`) don't depend on the board firmware
and only `st_sk` depends on espOS. Once proven on the boat, they may move into
espOS as a regular component (like `espos_nmea0183`), with only the board
firmware staying here.

## Build (Windows, PowerShell)

Needs ESP-IDF v6.0.x (espOS's pin is in `espos/.idf-version`). Load it with
your IDF's `export.ps1`, or with `tools/idf-env.ps1` after setting
`$env:ST_IDF_PATH` (the IDF folder) and `$env:ST_IDF_TOOLS_PATH` (its tools
folder). `-B <build-dir>` is optional; a build directory outside a synced
source tree avoids sync conflicts.

```powershell
git submodule update --init
. .\tools\idf-env.ps1
idf.py -B <build-dir> set-target esp32   # once
idf.py -B <build-dir> build
idf.py -B <build-dir> -p COMx flash monitor
```

### ESP32-S3 SuperMini

```powershell
idf.py -B <s3-build-dir> -D SDKCONFIG=sdkconfig.s3 -D IDF_TARGET=esp32s3 build
idf.py -B <s3-build-dir> menuconfig          # S3 settings: always with -B <s3-build-dir>
idf.py -B <s3-build-dir> -p COMx flash monitor
```

A separate build directory and sdkconfig keep it apart from the ESP32 build.
`sdkconfig.s3` lives in the project folder (git-ignored), so `fullclean` of
the build directory keeps your menuconfig changes. Every later `idf.py` call
for the S3 needs `-B <s3-build-dir>`: without it, idf.py uses the default
build directory and the ESP32 configuration. After `fullclean`, or in a new
build directory, `-B` alone is not enough: the build directory no longer
remembers `sdkconfig.s3` or the target, and idf.py falls back to the ESP32
and the project's `sdkconfig` (and rewrites it). Run the first command again,
with both `-D` options, before `menuconfig` or `flash`.

For this target the SeaTalk pins default to RX GPIO13 and TX GPIO12 and the
console (bench keys included) is on the board's USB-C, the chip's own USB
Serial/JTAG (`sdkconfig.defaults.esp32s3`). The pins, the UART and whether
the bus interface inverts are in `idf.py -B <s3-build-dir> menuconfig` ->
SeaTalk bridge. GPIO11 enables the RX/TX lights (HIGH = on); switch them in
the web UI, SeaTalk bridge -> General -> "RX/TX lights" (`seatalk.leds`). The
interface must hold the bus released while TX is LOW: the firmware drives TX
LOW first thing at boot, but during reset and the bootloader the pin floats,
so it needs a pull-down.

The first configure generates a development signing key,
`secure_boot_signing_key.pem` (git-ignored). A board flashed with it only
accepts OTA images signed with the same key: back it up.

## WiFi and OTA

```powershell
python tools/provision_wifi.py --port COMx --secrets <secrets.ini>   # WiFi into NVS (resets NVS)
.\tools\ota.ps1 -Device <hostname>.local -Image <build-dir>\seatalk_bridge.bin   # serve the build from this PC and install it over WiFi
```

Pass `-Image` with the build directory of the board's chip (`<s3-build-dir>`
for the S3): without it, `ota.ps1` serves a fixed default path. The device
refuses an image built for the other chip.

`secrets.ini` has a `[secrets]` section with `wifi_ssid` and `wifi_password`
(default: the st50-emulator's, next to this folder). The device's hostname is
`espos-` plus the last four hex digits of its MAC.

The device pulls the image over HTTP from this PC (port 8070; allow Python
through the Windows firewall on private networks). A new image that doesn't
reach the network within 10 minutes rolls back to the previous one.

On the CH340 DevKit the automatic reset into download mode is unreliable:
hold BOOT, tap EN, release BOOT. Flash at 115200 baud; 460800 gave serial
errors.

## Bench console

Key commands over USB serial and, optionally, WiFi (telnet). What is built in
is chosen in `idf.py menuconfig` -> SeaTalk bridge:

| Option | Default | What |
|---|---|---|
| `ST_BRIDGE_BENCH` | on | Bench console on USB serial (key commands, statistics) |
| `ST_BRIDGE_TELNET` | off | Telnet console, port 23, unauthenticated: bench use only |
| `ST_BRIDGE_SIM` | off | Simulated boat (demo values to the bus, keys `v`/`V`) |

espOS has no run-time module loading, so optional parts are build-time
choices. Telnet is the exception in one respect: when compiled in, it is
switched on and off at run time in the espOS web UI (tab "SeaTalk1 console",
namespace `seatalk_console`, setting `telnet`, default off). While off it costs no RAM (measured); while on it
takes about 5 KB plus about 8 KB per open session. Without `ST_BRIDGE_SIM`, keys `v`/`V`
only report that the simulator is not in this build.

With telnet on, `stconsole.py` connects to `--host <hostname>.local`:

```powershell
python tools/stconsole.py --host <hostname>.local -k "t" -s 3   # statistics
python tools/stconsole.py --host <hostname>.local -k "v" -s 2   # simulated boat on/off (needs ST_BRIDGE_SIM)
python tools/stconsole.py --host <hostname>.local -k "d" -s 3   # per-datagram table (mode, rate, sent, stale, native)
python tools/stconsole.py --host <hostname>.local -k "k" -s 8   # PHY self-test
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

![espOS config page with the SeaTalk bridge settings](docs/config-page.jpg)

- Values this device publishes appear as `$source` `<hostname>.XX`; the
  adapter ignores those on the way back (no SeaTalk → Signal K → SeaTalk loop).
- Without `headingMagnetic` / `courseOverGroundMagnetic` on the server, the
  magnetic value is computed from the true one minus `magneticVariation`.
- Diagnostics: `espos.<hostname>.seatalk.*` every 10 s.
- Substitutes (group `Substitutes`): `stw_from_sog` sends SOG as boat speed
  (datagram 20), `hdg_from_cog` sends COG as heading (89/9C), for displays
  without SOG/COG pages such as the ST50 Multi. `fallback` uses them only
  while the real value is stale, `replace` always.

Memory (classic ESP32, no PSRAM): about 52 KB free at rest in the default
build (bench console on, telnet and simulator off). `sdkconfig.defaults`
trims espOS's Signal K batch table to 16 slots to help.

## Host tests

```powershell
.\tools\host-test.ps1        # CMake + MSVC Build Tools 2022, runs ctest (build directory set in the script)
```

## Back to the emulator

Flash `st50-emulator/firmware/st50-emulator-merged.bin` (next to this
folder) at 0x0, or run `pio run -t upload` in `st50-emulator/`.

## License

Apache-2.0 ([LICENSE](LICENSE)), the same as espOS. Every source file carries
SPDX headers; [REUSE.toml](REUSE.toml) covers the files that cannot.
