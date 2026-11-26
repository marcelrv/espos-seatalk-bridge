# espos_seatalk

Raymarine SeaTalk 1 protocol stack and optional [espOS](https://github.com/signalk-espOS/espOS)
Signal K bridge component for ESP-IDF 6 (`esp32` and `esp32s3`).

## Architecture

- **PHY (`st_phy`)**: 4800 baud 9-bit UART (parity-bit command recovery), IRAM ISR, byte-by-byte self-echo collision detection, and TX-low bus safety.
- **Link (`st_link`)**: Datagram framing, echo filtering, and quiet-gap accounting.
- **Codec (`st_codec`)**: Decode/encode for common SeaTalk 1 datagrams (depth, wind, speed, log, water temperature, SOG, COG, heading, rudder, lamp).
- **Scheduler (`st_sched`)**: Per-datagram mode (`off`, `to_sk`, `to_bus`, `both`, `auto`) and polite periodic transmit scheduling.
- **Core (`st_core`)**: Single bus task with collision backoff, bus-held-low fail-safe, and one-off TX queue.
- **Signal K adapter (`st_sk`, optional)**: Compiled automatically when `espos_sk`, `espos_config`, and `espos_net` are present in the build. Registers the `seatalk` settings namespace in the espOS web UI.

## Quick start

Choose free GPIOs for your chip (on ESP32-S3, avoid SPI flash / PSRAM pins
GPIO26–37):

```c
#include "espos.h"
#include "espos_seatalk.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#define SEATALK_RX_GPIO 13
#define SEATALK_TX_GPIO 12
#else
#define SEATALK_RX_GPIO 32
#define SEATALK_TX_GPIO 33
#endif

void app_main(void)
{
    st_phy_tx_hold_low(SEATALK_TX_GPIO); /* release bus immediately on boot */
    ESP_ERROR_CHECK(espos_start(NULL));

    st_core_config_t cfg = ST_CORE_CONFIG_DEFAULT();
    cfg.phy.uart_port = 1;
    cfg.phy.rx_gpio = SEATALK_RX_GPIO;
    cfg.phy.tx_gpio = SEATALK_TX_GPIO;
    cfg.phy.invert = true;
    ESP_ERROR_CHECK(espos_seatalk_start(&cfg));
}
```

## License

Apache-2.0.
