# SeaTalk ESP32 Remote: hardware brief for the firmware

This is the hand-over from the hardware work to the firmware work. It holds everything the software needs to know about the board. It does not prescribe the software design.

Written 2026-11-13 from the KiCad netlists in this workspace. The board has **not been built or tested yet**, so every behaviour below is the designed behaviour.

---

## 0. Prompt to start the coding session

> I'm writing ESP32 firmware (Arduino core or ESP-IDF, your advice) for a home-built SeaTalk 1 interface board. It must receive and transmit on the Raymarine SeaTalk 1 bus. First goal: a wireless autopilot remote that sends keystroke datagrams and shows the bus traffic. The hardware is fixed and described in `SeaTalkRemote_FIRMWARE.md`. Read it first. Pay special attention to the **inverted** RX and TX logic, the self-echo used for collision detection, the 9-bit SeaTalk character format, and the rule that GPIO33 must never stay high. Start with a bench self-test (section 7) before anything touches the real bus.

---

## 1. Which board

Three board variants exist. All three use **the same ESP32 pins and the same logic polarity**, so one firmware fits all of them. The netlists confirm this: U1 uses only VIN, GND, 3V3, D32, D33, TX0 and RX0 on every variant.

| Variant | Folder | RX path | TX path |
|---|---|---|---|
| A-6N137 (being built first) | `STWirelessRemote - basic - 6n137/` | DATA → R4/R6 → Q1 BC547 → 6N137 LED; 6N137 output → GPIO32, 2K2 pull-up to 3V3 | GPIO33 → R2 1K → Q2 BC547 base (R7 10K to GND); collector on DATA |
| A basic | `STWirelessRemote - basic/` | Same, with a PC817 and a 10K pull-up | GPIO33 → R2 270R → PC817 LED; opto output on DATA |
| B protected | `SeaTalkESP32Remote/` | Like A, but behind diode D4 and R8 pull-up | Like A, with R7 100R and diode D3 in series |

---

## 2. ESP32 pin map (DevKit V1, 30-pin)

| ESP32 pin | Use | Direction | Notes |
|---|---|---|---|
| **GPIO32** (D32) | SeaTalk RX | Input | External pull-up on the board. No internal pull is needed |
| **GPIO33** (D33) | SeaTalk TX | Output | External 10K base pull-down (A-6N137). Drive it LOW at startup |
| GPIO1 / GPIO3 (TX0 / RX0) | UART0: programming and log | – | Also on header J2 (1 = TX0, 2 = RX0, 3 = GND) and on the module's USB |
| VIN | 5 V from the on-board 7805 | Power | Never 12 V |
| 3V3 | Feeds the RX pull-up | Power | – |

**Free pins** (not connected on the board, available on the module headers): D2, D4, D5, D12, D13, D14, D15, D18, D19, D21, D22, D23, D25, D26, D27, D34, D35, VP (GPIO36), VN (GPIO39), RX2 (GPIO16), TX2 (GPIO17), EN.

If you add buttons, LEDs or a display later:
- GPIO34, 35, 36 and 39 are input-only and have no internal pull-ups;
- GPIO0, 2, 5, 12 and 15 are strapping pins; avoid them for anything that is pulled at boot (GPIO12 high at boot breaks flash voltage);
- GPIO2 is the blue on-board LED on most DevKit V1 modules.

---

## 3. Signal logic (the important part)

The SeaTalk DATA line is a single-wire, open-collector bus with a pull-up to about 12 V. **Idle = high.** A device sends a 0 bit by pulling the line low.

**Both directions are inverted on this board.**

| SeaTalk DATA | Bus meaning | GPIO32 (RX) reads | GPIO33 (TX) must be |
|---|---|---|---|
| High, ~12 V | Idle, stop bit, 1 bit | **LOW** | **LOW** (released) |
| Low, < ~2.4 V | Start bit, 0 bit | **HIGH** | **HIGH** (pulls DATA low) |

How it works on A-6N137:
- **RX:** DATA high → Q1 conducts → 6N137 LED on → open-collector output pulls GPIO32 low. DATA low → LED off → R1 pulls GPIO32 to 3V3.
- **TX:** GPIO33 high → about 2.6 mA into Q2's base → Q2 pulls DATA low. GPIO33 low or floating → R7 keeps Q2 off → DATA released.
- **Receive threshold:** Q1 switches at about 2.4 V on DATA (A-6N137), about 2.1 V (A basic) or about 2.8 V (B).

For a UART this means: **invert both RX and TX.** On the Arduino core, for example: `Serial1.begin(4800, config, 32, 33, true)`. The last argument inverts RX and TX together, which matches this board. In ESP-IDF, use `uart_set_line_inverse(port, UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV)`.

### TX safety rules
1. **GPIO33 high for longer than one character jams the whole SeaTalk bus.** Every instrument and the autopilot go deaf. Make sure no code path, crash or debugger halt can leave it high.
2. **Set GPIO33 to output LOW as early as possible in startup.** During reset and boot, GPIO33 floats and R7 keeps the bus released. GPIO33 is not a strapping pin, so it doesn't glitch during boot.
3. If you use the UART with TX inversion: an uninverted UART TX idles **high** until inversion is configured. Always do **both** steps in order (matching `docs/protocol.md` §3.2): initialize GPIO33 as output LOW first, then configure UART TX inversion **before** routing UART TX to the pin.
4. Consider a watchdog that forces GPIO33 low if the line has been driven for more than about 3 ms (one 11-bit character at 4800 baud is 2.29 ms).

---

## 4. Self-echo and collision detection

The board hears itself: when GPIO33 pulls DATA low, GPIO32 sees it too. This is intended. SeaTalk has no bus arbiter, and collision detection relies on this echo.

What the firmware must do:
- **Before sending:** wait until the bus has been idle (GPIO32 LOW the whole time) for a while. Thomas Knauf's description (below) covers this; a few character times is a common choice. Make it a constant.
- **While sending:** compare each character read back on RX with the character just sent, including the 9th bit. On a mismatch, stop sending at once, release the bus, and retry the whole datagram later after a random back-off.
- **Received data includes your own transmissions.** If you use a UART, its RX buffer will contain the echo. Remove the echo before the datagrams go to the parser, or let the parser see them on purpose.

---

## 5. SeaTalk 1 character format

- **4800 baud**, 1 start bit, **9 data bits**, 1 stop bit, LSB first: 11 bits per character, 208.3 µs per bit.
- The 9th bit is the **command bit**. It is 1 on the first character of a datagram and 0 on the rest.
- A datagram is: command byte (9th bit = 1), attribute byte (low nibble + 3 = total length in bytes), then the data bytes.

The ESP32 UART has no 9-bit mode. Ways to get the 9th bit (choose one in the software session):
- **Parity trick:** use 8 data bits + parity, and set even or odd parity per character so the parity bit equals the wanted command bit. On receive, recover the bit from the parity error flag. This is easy on TX; check how the chosen framework reports parity errors on RX.
- **Software serial** with mark/space parity or 9-bit support (for example the EspSoftwareSerial library), with inversion on.
- **RMT peripheral** or timer-driven bit-banging on GPIO32/33.

The original Nauti-control firmware (github.com/nauti-control/STWirelessRemote, `SeaTalk.h`, GPL-3.0) works on this exact pin and polarity setup. It is a good reference for one working approach.

### Timing margins from the hardware
- The 6N137 is fast (< 100 ns). Q1 and Q2 (BC547) switch in a few µs. Edge delay is negligible next to a 208 µs bit.
- Falling edges on DATA are driven (sharp). **Rising edges are passive**, through the 10K pull-up R3 and the other devices' pull-ups, so they are slower on a long bus. Sampling in the middle of the bit, as a UART does, handles this.

---

## 6. Power and current

- The ESP32 runs from a **linear 7805** fed from the boat's 12–14.8 V. All ESP32 current turns into heat in the 7805: at 14.4 V and 150 mA that is about 1.4 W, so it gets hot.
- Less current = cooler board. Options for the firmware:
  - run the CPU at 80 MHz instead of 240 MHz;
  - use WiFi modem sleep, or Bluetooth LE only, if that's enough;
  - lower the WiFi TX power if the range allows it.
- WiFi transmit peaks (300+ mA) are buffered by C2 220 µF on VIN. Leave the brownout detector **on**. If it trips during WiFi start, that is a hardware finding worth reporting, not something to turn off.
- The board takes 12 V from the SeaTalk cable. A firmware hang therefore keeps drawing current from the boat's instrument supply; keep a task watchdog enabled.

### USB and bus power together
On most DevKit V1 modules, USB 5 V reaches VIN through a diode.
- **12 V on, USB connected:** fine, and the normal way to develop.
- **USB only, no 12 V:** the ESP32 runs, but the 7805 is back-fed from its output. There is no 12 V for the RX LED or the DATA pull-up, so RX reads HIGH (= bus "low") all the time. Don't trust bus data in this state, and avoid it for long periods.

Check your module before relying on this.

---

## 7. Bench test plan (no instruments needed)

With only a current-limited 12 V supply on J1 (pin 1 +12V, pin 3 GND, pin 2 DATA open):
1. **Idle:** R3 pulls DATA to about 12 V; GPIO32 reads **LOW**.
2. **TX/RX loopback:** set GPIO33 HIGH briefly → DATA drops to about 0.1 V and GPIO32 reads **HIGH**. Set it LOW → both return.
3. **Character loopback:** send datagrams through the chosen 9-bit method and check that every character, including the 9th bit, comes back identical on RX. This tests the whole TX/RX chain and the collision-detection code path without a real bus.
4. **Forced collision:** short DATA to GND through 1K (simulates another talker) while sending, and check that the firmware aborts and retries.
5. Only then connect it to a real SeaTalk bus, first in receive-only mode, and check that known datagrams are decoded (depth, wind, autopilot heading).

---

## 8. Protocol reference

- **Thomas Knauf, "SeaTalk Technical Reference"** (thomasknauf.de/seatalk.htm): the standard description of SeaTalk 1 datagrams, collision handling and timing.
- For an autopilot remote, the relevant message is the **keystroke datagram `0x86`** (format `86 X1 YY yy`, where `yy` is the bitwise inverse of `YY`). Standard key codes include Auto, Standby, −1, +1, −10 and +10. Take the exact codes and the `X` value from Knauf's reference, not from memory.
- `ST50_Multi/` in this workspace is a redrawn Raymarine ST50 schematic. It shows how Raymarine's own instruments connect to the bus, in case an electrical question comes up.
