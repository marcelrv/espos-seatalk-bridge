# SeaTalk core: design spec

The design for the firmware that connects a SeaTalk 1 bus to Signal K (and later, without Signal K, to NMEA 0183). It builds on:
- the hardware brief [SeaTalkRemote_FIRMWARE.md](SeaTalkRemote_FIRMWARE.md);
- the bench measurements made with the [st50-emulator](st50-emulator/) (test plan: [TESTPLAN.md](st50-emulator/TESTPLAN.md)).

Written 2026-11-13. Everything marked **measured** was measured on the bench: the SeaTalk ESP32 Remote board (variant A-6N137) with an ST50 Multi.

---

## 1. Decisions

| Topic | Decision | Why |
|---|---|---|
| Where SeaTalk is converted | On the ESP32 | Signal K has no general Signal K → SeaTalk encoder; keeps the device usable without Signal K |
| Framework | espOS (ESP-IDF 6) | SensESP's ESP-IDF successor. SensESP is explicitly not a target |
| SeaTalk code | Our own **SeaTalk core**: plain ESP-IDF components, independent of espOS | Reusable standalone (NMEA 0183 converter); the framework choice stays reversible |
| Receive PHY | Hardware UART (UART1), 9th bit from the parity flag | **Measured:** 0 errors in 302,823 bytes under WiFi and flash stress; software serial lost datagrams |
| Transmit PHY | Hardware UART, per-byte parity, echo check per byte | **Measured:** works; bit-bang has no arbitration advantage (§5.3); frees the CPU |
| Bus access | "Polite": ≥ 30 ms of quiet bus before sending, plus jitter | **Measured:** the ST50 waits ~26 ms. Shorter waits starve it or livelock (§5) |
| What the core outputs | Neutral values (quantity ID + SI number). Adapters map them to Signal K paths or NMEA 0183 sentences | One core for the Signal K firmware and a standalone NMEA 0183 converter |
| Direction per datagram | Configurable: `off` / `to_sk` / `to_bus` / `both` / `auto` (default) | No fixed "who wins"; `auto` lets a native bus instrument win |
| Data scope (v1) | Depth, wind, speed, water temperature, log; optionally heading/SOG/COG | Classic instruments; no AIS |
| `$STALK` raw stream | Later, not v1 | Nice to have (autopilot plugin, OpenCPN) |

---

## 2. Architecture

```
            +-----------------------------------------------------+
            |                  Adapters (per product)             |
            |  Signal K adapter (espOS)   |  NMEA 0183 / $STALK   |
            +--------------+--------------+-----------+-----------+
                           |  values (SI units, path ids) |
            +--------------v------------------------------v-------+
            |  st_codec   : datagram <-> value (pure functions)   |
            +--------------+--------------------------------------+
                           |  datagrams (bytes + metadata)
            +--------------v--------------------------------------+
            |  st_sched   : TX queue, rates, priorities, polite   |
            |               idle wait, retries, stale timeouts    |
            |  st_link    : RX framing, resync, echo suppression  |
            +--------------+--------------------------------------+
                           |  9-bit characters + timestamps
            +--------------v--------------------------------------+
            |  st_phy     : UART1 + IRAM ISR, parity 9th bit,     |
            |               per-byte TX with echo/collision check |
            +-----------------------------------------------------+
```

- **The core translates to neither Signal K nor NMEA 0183.** `st_codec` converts datagrams to and from **neutral values**: a quantity ID plus a number in SI units, e.g. `{ST_Q_DEPTH_BELOW_TRANSDUCER, 3.75}` in metres. The quantity list follows Signal K's meaning and units, because that's the richest model and NMEA 0183 can be derived from it. There's no dependency on Signal K.
  - The **Signal K adapter** maps quantity → path (nearly 1:1).
  - The **NMEA 0183 adapter** maps quantities → sentences and converts units (knots, feet, °C).
  - The **`$STALK` adapter** uses the raw datagram bytes, not the values.
- Every layer is an ESP-IDF component with a C API. C++ inside is fine.
- Only `st_phy` touches hardware. Everything above it can be unit-tested on a PC.
- The core doesn't depend on espOS. The Signal K adapter is the only part that does.
- One FreeRTOS task, `st_task`, runs the link layer and the scheduler. The ISR only moves characters.

### Proposed public API (sketch)

```c
// st_core.h
typedef struct { uint8_t len; uint8_t b[18]; uint32_t t_us; } st_datagram_t;

esp_err_t st_core_start(const st_config_t *cfg);           // pins, UART port, idle wait, ...
typedef void (*st_rx_cb_t)(const st_datagram_t *dg, void *arg);
void      st_core_on_rx(st_rx_cb_t cb, void *arg);        // foreign datagrams only (never our echo)

// Periodic datagram: the scheduler pulls fresh bytes at 'period_ms'.
// encode() returns 0 when its value is stale, and the slot then stays silent.
typedef uint8_t (*st_encode_fn)(uint8_t *out, void *arg);
st_slot_t st_core_add_periodic(uint8_t cmd, uint32_t period_ms, st_prio_t prio,
                               st_encode_fn encode, void *arg);
esp_err_t st_core_send_once(const uint8_t *b, uint8_t len, st_prio_t prio); // e.g. autopilot keys
void      st_core_stats(st_stats_t *out);
```

---

## 3. PHY (`st_phy`)

### 3.1 Pins and polarity
- UART1, RX = GPIO32, TX = GPIO33.
- **Both lines inverted** (`RXD_INV | TXD_INV`). The board inverts both directions (brief §3).
- Port and pins come from the config. The emulator also proved that a non-inverted setup on a 3.3 V wired-AND line works, which is useful for tests.

### 3.2 Startup safety (brief §3)
1. The **very first thing** in `app_main` (before espOS starts): GPIO33 as a GPIO output, LOW.
2. Configure UART1, inversion included, **before** routing TX to GPIO33. An inverted TX idles LOW, so the bus stays released.
3. Only then connect `U1TXD_OUT` to GPIO33 through the GPIO matrix.
4. Never send a break condition.
5. Fail-safe: if RX sees the bus held low for > 10 s, stop sending and report it (no 12 V, or a short).

### 3.3 Character format and the 9th bit
- 4800 baud, 1 start bit, 8 data bits, parity, 1 stop bit. The parity bit sits exactly where SeaTalk's command bit is.
- **TX:** per byte, choose EVEN or ODD parity so the parity bit equals the wanted command bit:
  `odd = parity8(data) != cmdBit`.
- **RX:** `cmdBit = expectedParityBit(data, currentMode) XOR parityErrorFlag`.
  When idle the mode is EVEN. While we transmit, it's whatever was set for the byte being sent, so echo bytes decode correctly too.
- `conf0.err_wr_mask = 0`, so bytes with a parity "error" (= command bit 1) are kept.

### 3.4 RX interrupt
- RX FIFO full threshold **1**: the ISR runs for every byte. ISR in IRAM (`ESP_INTR_FLAG_IRAM`). No flash calls, no `__builtin_parity`.
- The classic ESP32 FIFO keeps no per-byte error flags. The rules:
  - read FIFO count → raw interrupt flags → count again;
  - one byte waiting: the flags belong to it;
  - several bytes and a parity flag set: mark all as **AMBIGUOUS**; clear flags are certain for all;
  - after clearing the flags, anything still in the FIFO is treated as ambiguous if it shows a parity flag.
- Each character goes into a lock-free ring (1024 entries) as `{t_us (esp_timer), data, flags}`. Flags: CMD, AMBIGUOUS, FRAME_ERR, ECHO, ECHO_BAD.
- **Measured:** never more than one byte per ISR, worst echo latency 155 µs under WiFi and flash stress, 0 ambiguous in > 300,000 bytes.
- The emulator installs the ISR on core 0. In espOS, leave it wherever espOS puts the SeaTalk task; latency is far inside the 2.29 ms character time.

### 3.5 TX state machine (ISR-driven)
1. The task (after the idle wait, §5) loads the datagram, sets parity for byte 0, enables TX_DONE, writes byte 0.
2. The ISR, on the RX byte: if a datagram of ours is active, compare it with the byte sent (data, command bit, no framing error, not ambiguous) and flag it ECHO or ECHO_BAD.
3. The ISR, on TX_DONE:
   - echo bad → stop: **collision**;
   - no echo for the byte just sent → stop: **no echo** (hardware fault, no 12 V);
   - more bytes left → set parity for the next byte, write it;
   - otherwise → done.
4. `tx_idle_num = 0`. Gap between bytes is a few µs (ISR latency). **Measured:** the ST50 accepts it.
5. A collision is detected at the end of the character, not per bit. That's acceptable because bit-bang showed no arbitration advantage (§5.3).

### 3.6 Porting notes (emulator → espOS / IDF 6)
The emulator runs on Arduino core 2.0.17 (ESP-IDF 4.4). Check these when porting:
- `uart_ll_*` signatures changed between IDF 4.4 and 5/6 (baud rate / clock source in particular). Prefer `uart_param_config()` for setup and keep `uart_ll` for the ISR fast path.
- The UART pin signal tables changed (`uart_periph_signal[...]`). Use `U1RXD_IN_IDX` / `U1TXD_OUT_IDX` from `soc/gpio_sig_map.h`, which works on the classic ESP32.
- `esp_intr_alloc()`, `esp_rom_gpio_connect_*_signal()` and `esp_timer_get_time()` are unchanged in use.
- **Other chips (S3, C3, C6...):** the parity method works in principle, but error-flag behaviour and FIFO details differ. v1 targets the **classic ESP32** (the board). Other chips need their own bench validation with the emulator self-test.

---

## 4. Link layer (`st_link`)

### 4.1 Framing
- A character with the command bit set starts a datagram.
- Length = `(attr & 0x0F) + 3`, taken from byte 1. **Only the low nibble:** the ST50 sends `30 70 0X` (high nibble 7).
- Maximum datagram length is 18 bytes.
- A new command byte before the datagram is complete → count it as **incomplete** and start over (this is how the other device's abort-and-retry shows up).
- A data byte with no datagram open → **orphan**, dropped.
- A datagram containing an AMBIGUOUS or FRAME_ERR byte → dropped and counted (**flagged**). It must never reach the codec as a valid value.

### 4.2 Echo
- Bytes flagged ECHO never go to the parser or to Signal K. This is the first loop-prevention layer.
- After a collision, the rest of the other device's datagram is normal foreign data. Its start was lost, so it becomes an orphan or incomplete.

### 4.3 Timestamps
- Each datagram keeps the timestamp of its first byte. `idle-before` = start time − end of the previous byte on the bus.
- Keep these as diagnostics. They're how we measured the ST50's behaviour, and we'll need them for other devices (autopilot!).

---

## 5. Bus access and scheduler (`st_sched`)

### 5.1 Measured behaviour of the ST50 Multi
| Our quiet gap / idle wait | Result |
|---|---|
| ST50's own wait before talking | **25.7–26.0 ms** of quiet bus, every time |
| Gap 15 ms (we start after ~20 ms) | ST50 **starved**: never talks, 0 collisions |
| Gap 20 ms (we start after ~25 ms) | **Livelock**: starts coincide, 80–124 collisions/20–30 s, 0 ST50 datagrams. Happened with **both** UART and bit-bang |
| Gap 24–60 ms | ST50 gets through; a few collisions |
| **Idle wait 30 ms, gap 0** (polite) | UART: 74 ST50 datagrams/30 s, 4 collisions, **0 dropped**. Our load 24% |

### 5.2 Rules
1. **Idle wait:** before each datagram the bus must have been quiet for `IDLE_US = 30 ms + random(0..4 ms)`.
   - It must be longer than every Raymarine device's own wait, so they always go first. We're the guest on this bus.
   - The jitter stops us from starting in sync with another device's fixed timer, which is what caused the livelock.
   - Make it a config value. Other devices, the autopilot especially, may need a different value once measured.
2. **Retries:** after a collision, random back-off `2..15 ms`, then the full idle wait again. At most 5 retries, then drop and count it. **Measured:** 0 drops in polite mode.
3. **No echo / bus held low:** don't retry. Report it and pause sending.
4. **Capacity:** with a 30 ms idle wait our maximum is ≈ 20 datagrams/s (24% load). Plan for **≤ 50% of that** (~10 datagrams/s), leaving room for other devices.
5. **Priorities:**
   - `HIGH`: autopilot commands (0x86), sent at once (still politely);
   - `NORMAL`: periodic instrument data;
   - `LOW`: slow values (temperature, log).
6. **Per-datagram rate** (defaults, configurable):

   | Datagram | Rate |
   |---|---|
   | 10/11 apparent wind | 2 Hz |
   | 00 depth, 20 STW, 52 SOG, 53 COG, 9C heading | 1 Hz |
   | 27 water temperature, 25 log/trip | 0.2 Hz |

   This adds up to ≈ 9.4 datagrams/s.
7. **Stale data:** a slot whose source value is older than its timeout (default 5 s, 3× the source rate for fast values) **stops sending**. A display must go blank rather than show a frozen value.
8. **Direction is configured per datagram type** (see §7.1). In the default `auto` mode our slot pauses while that type has been received from another bus device in the last 10 s, and resumes 10 s after the native source goes quiet. This is the second loop-prevention layer, and it stops two sources fighting on the display.

### 5.3 Why not bit-bang
The idea was that releasing the bus at the first wrong bit lets the other device win cleanly, like CAN arbitration. **Measured:** it didn't help (124 collisions, 114 aborted ST50 datagrams at the bad gap). SeaTalk devices don't start on the same bit time, so both sides see a wrong bit and both abort. The real fix is timing (§5.2), and the UART does as well as bit-bang there, without busy-waiting with interrupts off.

---

## 6. Codec (`st_codec`)

Pure functions, SI units in and out (Signal K conventions: m, m/s, rad, K). Unit-tested on a PC with the byte examples below.

### 6.1 v1 datagram set
"Display ✓" means our encoder output is shown correctly by the ST50 Multi (st50-emulator).

| Cmd | Layout | Meaning | Signal K path | Dir. | Display ✓ |
|---|---|---|---|---|---|
| 00 | `00 02 YZ XX XX` | Depth below transducer, XXXX/10 ft | `environment.depth.belowTransducer` | ↔ | ✓ |
| 10 | `10 01 XX YY` | Apparent wind angle, XXYY/2 °, big-endian | `environment.wind.angleApparent` | ↔ | ✓ |
| 11 | `11 01 XX 0Y` | Apparent wind speed, (XX&0x7F) + Y/10 kn; 0x80 = m/s | `environment.wind.speedApparent` | ↔ | ✓ |
| 20 | `20 01 XX XX` | Speed through water, XXXX/10 kn | `navigation.speedThroughWater` | ↔ | ✓ |
| 21 | `21 02 XX XX 0X` | Trip, XXXXX/100 nm | `navigation.trip.log` | ↔ | ✓ |
| 22 | `22 02 XX XX 00` | Total log, XXXX/10 nm | `navigation.log` | ↔ | ✓ |
| 23 | `23 Z1 XX YY` | Water temp, whole °C / °F | `environment.water.temperature` | → SK | (off: rounds) |
| 25 | `25 Z4 XX YY UU VV AW` | Total (/10 nm) + trip (/100 nm) | `navigation.log`, `navigation.trip.log` | ↔ | ✓ |
| 26 | `26 04 XX XX YY YY DE` | STW /100 kn (current, average) | `navigation.speedThroughWater` | ↔ | ✓ |
| 27 | `27 01 XX XX` | Water temp, (XXXX−100)/10 °C | `environment.water.temperature` | ↔ | ✓ |
| 52 | `52 01 XX XX` | SOG, XXXX/10 kn | `navigation.speedOverGround` | ↔ | ✓ |
| 53 | `53 U0 VW` | COG, magnetic | `navigation.courseOverGroundMagnetic` | ↔ | ✓ |
| 89 | `89 U2 VW XY 2Z` | Compass heading, magnetic | `navigation.headingMagnetic` | ↔ | ✓ |
| 9C | `9C U1 VW RR` | Heading + rudder | `navigation.headingMagnetic`, `steering.rudderAngle` | ↔ | ✓ |
| 30 | `30 X0 0Y` | Lamp level (ST50 sends `30 70 0X`) | – (internal / optional) | ← ST | **measured** RX |

Heading encoding (53/89/9C): `deg = (U & 3)*90 + (VW & 0x3F)*2 + odd`. The odd-degree flag sits in the high bits of U (0x8 for 53, 0x4 for 89/9C); see the emulator's `encodeHeading()`.

**Decoder rules:**
- Respect the unit/flag bits that Signal K's own parser ignores: metric flag in 00 (Y & 4), m/s flag in 11 (0x80), validity/MPH flags in 26.
- Unknown datagram types → raw callback only (for `$STALK` later), never dropped silently in the stats.

### 6.2 Later
- 84 (autopilot status) and 86 (keystrokes) for the wireless remote and autopilot control. Key codes come from Knauf, not from memory (brief §8).
- 50/51/54/56/57 position, time and GPS data, if a GPS-less ST system needs them.
- 85 cross-track/bearing, 82 waypoint name.

---

## 7. Signal K adapter (espOS)

### 7.1 Per-datagram configuration
Each datagram type (00, 10, 11, 20, ...) has its own entry in the config. The same table is used by the NMEA 0183 adapter.

| Setting | Values | Default |
|---|---|---|
| `mode` | `off` · `to_sk` (bus → Signal K only) · `to_bus` (Signal K → bus only) · `both` · `auto` | `auto` |
| `rate_ms` | Transmit period on the bus (only for `to_bus`, `both`, `auto`) | per §5.2 rule 6 |
| `stale_ms` | Stop sending once the source value is older than this | 5000 |
| `source` | Optional: only take this Signal K `$source` (e.g. the NMEA 2000 wind sensor) | any, except our own |

- **`auto`:**
  - the type is always forwarded bus → Signal K;
  - Signal K → bus only while no other bus device sends that type (§5.2 rule 8);
  - an instrument on the bus therefore wins automatically, and our datagram fills the gap when there's none.
- **`both`:** both directions without the native-source check. Loop prevention (echo and `$source`) still applies.
- Shown and edited in the espOS web UI (JSON descriptor); the defaults work without any configuration.
- **Example: the owner's boat.**
  - The only native SeaTalk instrument is depth; everything else comes from Signal K. All types are set to `both`.
  - Depth goes bus → Signal K. Its Signal K value carries our own `$source`, so loop prevention stops it coming back to the bus.
  - Wind, speed, log, temperature, SOG/COG and heading go Signal K → bus for the ST50.

### 7.2 Data flow

- **Bus → Signal K:** every valid foreign datagram → codec → `espos_sk_publish_number(path, value)`.
  - The source label is espOS's own (`espos.<hostname>`), which is how loop prevention recognises our values.
  - Throttle at most per path, with espOS batching (`sk.batch_ms`).
- **Signal K → bus:**
  - `espos_sk_subscribe(path, period)` for each **configured** path. The subscription period throttles the input, and a value only lands in its slot when it's fresher than the slot's rate.
  - **Loop prevention, layer 3:** ignore updates whose `$source` is our own device. Never send a value back to SeaTalk that came from SeaTalk.
  - What goes to the bus follows the per-datagram `mode` (§7.1).
- **Later: autopilot.** `espos_sk_put_handler_register("steering.autopilot.state", …)` and similar PUT handlers → 0x86 keystrokes at HIGH priority.
- Config through espOS's JSON descriptors (`config/seatalk.json`): idle wait, rates, path allow-list, stale timeouts.

---

## 8. Standalone NMEA 0183 / `$STALK` (later)
- A TCP server (port 10110) and/or UART2 (wired).
- Out: `$STALK,<cmd>,<attr>,<data...>*CS` for every foreign datagram, in hex, standard XOR checksum. Optionally NMEA sentences (DPT, MWV, VHW, MTW, VLW...) from the same codec values.
- In: `$STALK` lines → `st_core_send_once()` with NORMAL priority (HIGH for 0x86). This lets the existing signalk-autopilot plugin and OpenCPN work unchanged.

---

## 9. Diagnostics (keep from the emulator)
- Counters: TX ok / retried / collisions / no-echo / bus-busy / dropped; RX bytes / echo ok-bad / ambiguous / frame errors / overflow / max bytes per ISR; datagrams / incomplete / orphan / flagged; bus load (own / foreign); the last echo mismatch in detail.
- `idle-before` histogram per foreign datagram type, to learn each device's timing on a real boat.
- Expose them in Signal K under the device (e.g. `electrical.seatalk.*` or a plugin path) and in the espOS web UI.
- Boot count and reset reason at startup.

---

## 10. Acceptance tests
Run with the st50-emulator board as the test partner, or with the new firmware in place of the emulator.

| # | Test | Pass |
|---|---|---|
| 1 | Self-test pattern (all 256 values) through the new `st_phy` | 0 echo errors, 0 ambiguous |
| 2 | 20 min soak at the maximum polite load, WiFi + espOS Signal K running | 0 RX errors, max bytes/ISR 1, no reboot |
| 3 | ST50 light key every 2 s, our maximum load | Every lamp datagram received, 0 dropped of ours |
| 4 | Stale test: stop the Signal K source | Our datagram stops within the timeout; ST50 blanks |
| 5 | Loop test: same path from SeaTalk and Signal K | No echo back to the bus, no ping-pong |
| 6 | Startup and reset | GPIO33 never high outside a character (scope or logic analyser) |
| 7 | Forced collisions (jammer ESP, TESTPLAN §6) | Collisions recovered, no invalid values shown |

---

## 11. Open questions
1. **Other devices' timing:** autopilot, wind and depth instruments may wait less or more than 26 ms. Measure them on the boat with the `idle-before` diagnostics before freezing `IDLE_US`.
2. **Settled 2026-11-13:** direction is configured per datagram type (§7.1), with `auto` as the default, where a native bus instrument wins.
3. **Your boat's setup:** which datagram types to set to `to_bus` or `both`, for example wind from an NMEA 2000 sensor, or GPS SOG/COG. With `auto` everything works without configuring anything.
4. **The `30 70 0X` high nibble:** is it a device ID? Log it per device; it may identify senders.
5. **Chip:** v1 is the classic ESP32. Decide whether S3/C3 boards are wanted later (that needs PHY re-validation, §3.6).
