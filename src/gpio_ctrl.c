/*
 * gpio_ctrl.c - Implementazione gestione GPIO RAK5010-M
 *
 * Utilizza esclusivamente le API Device Tree di Zephyr (GPIO_DT_SPEC_GET,
 * gpio_pin_configure_dt, gpio_pin_set_dt) per garantire portabilita'
 * e indipendenza dai numeri di pin fisici nel codice C.
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "gpio_ctrl.h"

LOG_MODULE_REGISTER(gpio_ctrl, CONFIG_LOG_DEFAULT_LEVEL);

/* ============================================================================
 * Specifiche GPIO dal Device Tree
 *
 * GPIO_DT_SPEC_GET risolve a compile-time il device e il numero di pin
 * leggendoli direttamente dal Device Tree compilato. Se un nodo non esiste
 * nel DT, la compilazione fallisce con un errore chiaro.
 * ============================================================================ */

/** LED verde utente - P0.12 - definito nel DTS ufficiale come alias led0 */
static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/**
 * PWRKEY del BG95-M3 - P0.02
 * Definito nel DTS ufficiale come proprieta' mdm-power-gpios del nodo
 * "modem" (figlio di uart0). Non viene ridefinito nell'overlay per
 * evitare conflitti con il driver quectel,bg95.
 */
static const struct gpio_dt_spec bg95_pwrkey =
    GPIO_DT_SPEC_GET(DT_NODELABEL(modem), mdm_power_gpios);

/** Output utente - definiti in app.overlay sotto user_outputs */
static const struct gpio_dt_spec g_out1 =
    GPIO_DT_SPEC_GET(DT_NODELABEL(out1), gpios);
static const struct gpio_dt_spec g_out2 =
    GPIO_DT_SPEC_GET(DT_NODELABEL(out2), gpios);
static const struct gpio_dt_spec g_out3 =
    GPIO_DT_SPEC_GET(DT_NODELABEL(out3), gpios);

/**
 * Ingresso IN4 - P1.01 - J12 pin 4
 * Definito in app.overlay sotto user_inputs con compatible "gpio-keys".
 * Configurato come input con pull-down nel gpio_ctrl_init().
 */
static const struct gpio_dt_spec g_in4 =
    GPIO_DT_SPEC_GET(DT_NODELABEL(in4), gpios);

/**
 * Array di puntatori agli output per accesso tramite indice enum.
 * L'ordine deve corrispondere esattamente a gpio_out_id_t.
 */
static const struct gpio_dt_spec *out_specs[GPIO_OUT_COUNT] = {
    &g_out1,  /* GPIO_OUT_1 */
    &g_out2,  /* GPIO_OUT_2 */
    &g_out3,  /* GPIO_OUT_3 */
};

/* ============================================================================
 * Inizializzazione
 * ============================================================================ */

int gpio_ctrl_init(void)
{
    int ret;

    /* --- LED verde utente --- */
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED GPIO device non pronto");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Errore configurazione LED: %d", ret);
        return ret;
    }

    /* --- BG95 PWRKEY --- */
    if (!gpio_is_ready_dt(&bg95_pwrkey)) {
        LOG_ERR("PWRKEY GPIO device non pronto");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&bg95_pwrkey, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Errore configurazione PWRKEY: %d", ret);
        return ret;
    }

    /* --- Output utente OUT1, OUT2, OUT3 --- */
    for (int i = 0; i < GPIO_OUT_COUNT; i++) {
        if (!gpio_is_ready_dt(out_specs[i])) {
            LOG_ERR("OUT%d GPIO device non pronto", i + 1);
            return -ENODEV;
        }
        ret = gpio_pin_configure_dt(out_specs[i], GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_ERR("Errore configurazione OUT%d: %d", i + 1, ret);
            return ret;
        }
    }

    /*
     * --- Ingresso IN4 ---
     * GPIO_PULL_DOWN: resistenza di pull-down interna del nRF52840.
     * Garantisce che la lettura sia LOW (0) in assenza di segnale,
     * evitando stati indeterminati sul pin flottante.
     * Il pull-down interno e' circa 11-16 kOhm sul nRF52840.
     */
    if (!gpio_is_ready_dt(&g_in4)) {
        LOG_ERR("IN4 GPIO device non pronto");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&g_in4, GPIO_INPUT | GPIO_PULL_DOWN);
    if (ret < 0) {
        LOG_ERR("Errore configurazione IN4: %d", ret);
        return ret;
    }

    LOG_INF("GPIO inizializzati:");
    LOG_INF("  LED:P0.12  PWRKEY:P0.02");
    LOG_INF("  OUT1:P0.19  OUT2:P0.20  OUT3:P1.02");
    LOG_INF("  IN4:P1.01 (input, pull-down)");
    LOG_INF("  [!] Verificare EXT_VREF (J12 pin 1) collegato a 3.3V");
    return 0;
}

/* ============================================================================
 * LED
 * ============================================================================ */

void gpio_ctrl_led_set(bool on)
{
    gpio_pin_set_dt(&led, on ? 1 : 0);
}

/* ============================================================================
 * Output utente
 * ============================================================================ */

int gpio_ctrl_output_set(gpio_out_id_t id, bool value)
{
    if (id >= GPIO_OUT_COUNT) {
        LOG_ERR("ID output non valido: %d", id);
        return -EINVAL;
    }
    int ret = gpio_pin_set_dt(out_specs[id], value ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Errore set OUT%d: %d", id + 1, ret);
    } else {
        LOG_INF("OUT%d -> %s", id + 1, value ? "ON" : "OFF");
    }
    return ret;
}

int gpio_ctrl_output_get(gpio_out_id_t id)
{
    if (id >= GPIO_OUT_COUNT) {
        return -EINVAL;
    }
    return gpio_pin_get_dt(out_specs[id]);
}

/* ============================================================================
 * Ingresso IN4
 * ============================================================================ */

int gpio_ctrl_in4_get(void)
{
    return gpio_pin_get_dt(&g_in4);
}

/* ============================================================================
 * Gestione alimentazione BG95-M3
 *
 * Tempi da datasheet Quectel BG95-M3 Hardware Design:
 *   Power ON : PWRKEY HIGH >= 500 ms, poi LOW; boot completato in ~5 s
 *   Power OFF: PWRKEY HIGH >= 650 ms, poi LOW; shutdown in ~2-3 s
 * ============================================================================ */

void gpio_ctrl_bg95_power_on(void)
{
    LOG_INF("BG95: avvio sequenza accensione (PWRKEY P0.02)");
    gpio_pin_set_dt(&bg95_pwrkey, 1);
    k_msleep(600);   /* 600 ms > 500 ms minimo richiesto */
    gpio_pin_set_dt(&bg95_pwrkey, 0);
    k_msleep(5000);  /* Attesa boot: UART risponde dopo ~3-5 s */
    LOG_INF("BG95: acceso");
}

void gpio_ctrl_bg95_power_off(void)
{
    LOG_INF("BG95: avvio sequenza spegnimento");
    gpio_pin_set_dt(&bg95_pwrkey, 1);
    k_msleep(700);   /* 700 ms > 650 ms minimo per spegnimento */
    gpio_pin_set_dt(&bg95_pwrkey, 0);
    k_msleep(3000);  /* Attesa shutdown completo */
    LOG_INF("BG95: spento");
}

void gpio_ctrl_bg95_reset(void)
{
    /*
     * Il pin RESET del BG95-M3 non e' esposto nel DTS ufficiale RAK5010-M.
     * Si effettua un reset via power cycle: spegnimento + pausa + accensione.
     * Tempo totale: ~10 secondi.
     */
    LOG_INF("BG95: reset via power cycle");
    gpio_ctrl_bg95_power_off();
    k_msleep(1000);
    gpio_ctrl_bg95_power_on();
}
