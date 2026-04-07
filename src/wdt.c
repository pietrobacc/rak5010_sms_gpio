/*
 * wdt.c - Watchdog hardware nRF52840
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include "wdt.h"

LOG_MODULE_REGISTER(wdt, CONFIG_LOG_DEFAULT_LEVEL);

#define WDT_TIMEOUT_MS  30000U  /* 30 secondi */

static const struct device *wdt_dev;
static int wdt_channel_id = -1;

int wdt_init(void)
{
    wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt));
    if (!device_is_ready(wdt_dev)) {
        LOG_ERR("WDT device non pronto");
        return -ENODEV;
    }

    struct wdt_timeout_cfg cfg = {
        .window = {
            .min = 0,
            .max = WDT_TIMEOUT_MS,
        },
        .callback  = NULL,   /* NULL = reset immediato senza callback */
        .flags     = WDT_FLAG_RESET_SOC,
    };

    wdt_channel_id = wdt_install_timeout(wdt_dev, &cfg);
    if (wdt_channel_id < 0) {
        LOG_ERR("WDT install timeout fallito: %d", wdt_channel_id);
        return wdt_channel_id;
    }

    int ret = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret < 0) {
        LOG_ERR("WDT setup fallito: %d", ret);
        return ret;
    }

    LOG_INF("WDT inizializzato - timeout %u ms", WDT_TIMEOUT_MS);
    return 0;
}

void app_wdt_feed(void)
{
    if (wdt_channel_id >= 0) {
        wdt_feed(wdt_dev, wdt_channel_id);
    }
}