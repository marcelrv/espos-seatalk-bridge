/*
 * seatalk-bridge: SeaTalk 1 <-> Signal K bridge on espOS.
 *
 * M0 skeleton: bus-safe startup + espOS. The SeaTalk core arrives in M1+.
 */
#include "driver/gpio.h"
#include "esp_log.h"
#include "espos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "seatalk-bridge";

/* SeaTalk TX. HIGH pulls the bus low: it must never stay high (spec 3.2). */
#define ST_TX_GPIO GPIO_NUM_33

void app_main(void)
{
    /* Very first thing, before espOS: release the bus. */
    gpio_set_level(ST_TX_GPIO, 0);
    gpio_set_direction(ST_TX_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ST_TX_GPIO, 0);

    ESP_ERROR_CHECK(espos_start(NULL));
    ESP_LOGI(TAG, "started; SeaTalk TX (GPIO%d) held LOW", ST_TX_GPIO);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
