/*
 * auth.c - Gestione numeri autorizzati e notifiche
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include "auth.h"

LOG_MODULE_REGISTER(auth, CONFIG_LOG_DEFAULT_LEVEL);

#define NUM_MAX_LEN  16  /* max lunghezza numero telefonico */
#define NUM_COUNT     3  /* numeri cliente configurabili */

static char numbers[NUM_COUNT][NUM_MAX_LEN] = {"", "", ""};
static uint8_t notify_index = 1;  /* default: notifiche a NUM1 */

/* ============================================================================
 * Settings subsystem
 * ============================================================================ */

static int settings_set_cb(const char *key, size_t len,
                            settings_read_cb read_cb, void *cb_arg)
{
    if (strcmp(key, "notify") == 0) {
        uint8_t idx;
        if (len != sizeof(idx)) return -EINVAL;
        if (read_cb(cb_arg, &idx, sizeof(idx)) != sizeof(idx)) return -EIO;
        if (idx >= 1 && idx <= NUM_COUNT) notify_index = idx;
        return 0;
    }

    /* num1, num2, num3 */
    int i = -1;
    if      (strcmp(key, "num1") == 0) i = 0;
    else if (strcmp(key, "num2") == 0) i = 1;
    else if (strcmp(key, "num3") == 0) i = 2;
    else {
        LOG_WRN("Auth settings: chiave sconosciuta '%s'", key);
        return 0;
    }

    if (len >= NUM_MAX_LEN) return -EINVAL;
    char buf[NUM_MAX_LEN] = {0};
    if (read_cb(cb_arg, buf, len) != (ssize_t)len) return -EIO;
    buf[len] = '\0';
    strncpy(numbers[i], buf, NUM_MAX_LEN - 1);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(auth, "auth", NULL, settings_set_cb, NULL, NULL);

/* ============================================================================
 * API pubblica
 * ============================================================================ */

int auth_init(void)
{
    int ret = settings_subsys_init();
    if (ret < 0) {
        LOG_WRN("Auth: settings non disponibile - uso valori default");
        return 0;
    }

    settings_load_subtree("auth");

    LOG_INF("Auth inizializzato:");
    LOG_INF("  NUM1: %s", strlen(numbers[0]) ? numbers[0] : "(vuoto)");
    LOG_INF("  NUM2: %s", strlen(numbers[1]) ? numbers[1] : "(vuoto)");
    LOG_INF("  NUM3: %s", strlen(numbers[2]) ? numbers[2] : "(vuoto)");
    LOG_INF("  TECH1: %s", TECH_NUMBER_1);
    LOG_INF("  TECH2: %s", TECH_NUMBER_2);
    LOG_INF("  NOTIFY: NUM%u", notify_index);
    return 0;
}

bool auth_is_authorized(const char *sender)
{
    if (sender == NULL || strlen(sender) == 0) return false;

    /* Numeri tecnici fissi - sempre autorizzati */
    if (strcmp(sender, TECH_NUMBER_1) == 0) return true;
    if (strcmp(sender, TECH_NUMBER_2) == 0) return true;

    /* Numeri cliente configurabili */
    for (int i = 0; i < NUM_COUNT; i++) {
        if (strlen(numbers[i]) > 0 &&
            strcmp(sender, numbers[i]) == 0) {
            return true;
        }
    }

    return false;
}

int auth_set_number(uint8_t index, const char *number)
{
    if (index < 1 || index > NUM_COUNT) return -EINVAL;
    if (number == NULL) return -EINVAL;
    if (strlen(number) >= NUM_MAX_LEN) return -EINVAL;

    int i = index - 1;
    strncpy(numbers[i], number, NUM_MAX_LEN - 1);
    numbers[i][NUM_MAX_LEN - 1] = '\0';

    char settings_key[16];
    snprintf(settings_key, sizeof(settings_key), "auth/num%u", index);

    int ret = settings_save_one(settings_key, number, strlen(number));
    if (ret == 0) {
        if (strlen(number) > 0) {
            LOG_INF("NUM%u impostato: %s", index, number);
        } else {
            LOG_INF("NUM%u cancellato", index);
        }
    }
    return ret;
}

const char *auth_get_number(uint8_t index)
{
    if (index < 1 || index > NUM_COUNT) return "";
    return numbers[index - 1];
}

int auth_set_notify(uint8_t index)
{
    if (index < 1 || index > NUM_COUNT) {
        LOG_ERR("auth_set_notify: indice non valido: %u", index);
        return -EINVAL;
    }

    notify_index = index;
    int ret = settings_save_one("auth/notify", &index, sizeof(index));
    if (ret == 0) {
        LOG_INF("NOTIFY impostato a NUM%u (%s)", index,
                strlen(numbers[index-1]) ? numbers[index-1] : "vuoto");
    }
    return ret;
}

const char *auth_get_notify_number(void)
{
    /* Se il numero di notifica è configurato usalo,
       altrimenti usa TECH_NUMBER_1 come fallback */
    const char *num = numbers[notify_index - 1];
    if (strlen(num) > 0) return num;

    LOG_WRN("Numero notifica NUM%u vuoto - uso TECH_NUMBER_1", notify_index);
    return TECH_NUMBER_1;
}

uint8_t auth_get_notify_index(void)
{
    return notify_index;
}