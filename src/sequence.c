/*
 * sequence.c - Macchina a stati sequenza di avvio (implementazione k_work)
 *
 * Sequenze disponibili:
 *
 *   START - Accensione generatore:
 *     EXP_OUT0 ON -> attesa T1 -> EXP_OUT1 ON -> attesa T2 ->
 *     EXP_OUT1 OFF -> polling EXP_IN_0 (max T3) ->
 *     OK: SMS + attesa T4 min + EXP_OUT0 OFF + SMS spegnimento
 *     FAIL: SMS + EXP_OUT0 OFF
 *     STOP: EXP_OUT0 OFF + SMS
 *
 *   START POMPA - Accensione generatore + pompa:
 *     (stessa accensione generatore di START) ->
 *     OK: attesa T5 sec -> EXP_OUT2 ON ->
 *     attesa T6 min | EXP_IN_1 HIGH | STOP ->
 *     EXP_OUT2 OFF + EXP_OUT0 OFF + SMS
 *
 *   TEST - Ciclo output 0-7 ogni 0.5s finche' EXP_IN_0 HIGH
 *
 *   Avvio automatico: se VEXT < S1 e SEQ_IDLE -> START automatico
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdlib.h>
#include "sequence.h"
#include "gpio_ctrl.h"
#include "sms.h"

LOG_MODULE_REGISTER(sequence, CONFIG_LOG_DEFAULT_LEVEL);

/* ============================================================================
 * Stato interno del modulo
 * ============================================================================ */

/** Parametri con valori di default */
static sequence_params_t params = {
    .t1_ms  = 5000,   /* 5 secondi */
    .t2_ms  = 5000,   /* 5 secondi */
    .t3_ms  = 5000,   /* 5 secondi */
    .t4_min = 30,     /* 30 minuti */
    .t5_ms  = 10000,  /* 10 secondi */
    .t6_min = 0,      /* infinito */
    .s1_v   = 12.5f,  /* 12.5 Volt */
    .autostart = false,  /* disabilitato per default */
};

/** Stato corrente della macchina a stati */
static sequence_state_t state = SEQ_IDLE;

/** Numero del mittente a cui inviare gli SMS di esito */
static char reply_to[24];

/** Flag per interrompere la sequenza via SMS STOP */
static volatile bool stop_requested = false;

/* ============================================================================
 * Settings subsystem - Persistenza parametri in NVS
 * ============================================================================ */

static int settings_set_cb(const char *key, size_t len,
                            settings_read_cb read_cb, void *cb_arg)
{
    /* S1 e' un float, gli altri sono uint32_t */
    if (strcmp(key, "s1") == 0) {
        float val_f;
        if (len != sizeof(val_f)) return -EINVAL;
        if (read_cb(cb_arg, &val_f, sizeof(val_f)) != sizeof(val_f)) return -EIO;
        params.s1_v = val_f;
        return 0;
    }

    uint32_t val_u;
    if (len != sizeof(val_u)) {
        LOG_WRN("Settings: dimensione non valida per '%s'", key);
        return -EINVAL;
    }
    if (read_cb(cb_arg, &val_u, sizeof(val_u)) != sizeof(val_u)) {
        LOG_ERR("Settings: errore lettura '%s'", key);
        return -EIO;
    }

    if      (strcmp(key, "t1") == 0) params.t1_ms  = val_u;
    else if (strcmp(key, "t2") == 0) params.t2_ms  = val_u;
    else if (strcmp(key, "t3") == 0) params.t3_ms  = val_u;
    else if (strcmp(key, "t4") == 0) params.t4_min = val_u;
    else if (strcmp(key, "t5") == 0) params.t5_ms  = val_u;
    else if (strcmp(key, "t6") == 0) params.t6_min = val_u;
    else if (strcmp(key, "auto") == 0) params.autostart = (bool)val_u;
    else LOG_WRN("Settings: chiave sconosciuta '%s'", key);

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(seq, "seq", NULL, settings_set_cb, NULL, NULL);

/* ============================================================================
 * Helper - Accensione generatore
 * Comune a START e START POMPA.
 * ============================================================================ */

/**
 * @brief Esegue la sequenza di accensione del generatore.
 *
 * Step 1: EXP_OUT0 ON + attesa T1
 * Step 2: EXP_OUT1 ON + attesa T2
 * Step 3: EXP_OUT1 OFF
 * Step 4: polling EXP_IN_0 ogni 50ms fino a T3
 *
 * In ogni step controlla stop_requested ogni 100ms.
 *
 * @return true se EXP_IN_0 HIGH rilevato, false se timeout o STOP.
 */
static bool generatore_accendi(void)
{
    stop_requested = false;

    /* Step 1: EXP_OUT0 ON + attesa T1 */
    gpio_ctrl_exp_out_set(EXP_OUT_0, true);
    LOG_INF("SEQ: EXP_OUT0 ON - attesa T1 (%u s)", params.t1_ms / 1000);
    for (uint32_t i = 0; i < params.t1_ms; i += 100) {
        if (stop_requested) goto gen_stop;
        k_msleep(100);
    }

    /* Step 2: EXP_OUT1 ON + attesa T2 */
    gpio_ctrl_exp_out_set(EXP_OUT_1, true);
    LOG_INF("SEQ: EXP_OUT1 ON - attesa T2 (%u s)", params.t2_ms / 1000);
    for (uint32_t i = 0; i < params.t2_ms; i += 100) {
        if (stop_requested) goto gen_stop;
        k_msleep(100);
    }

    /* Step 3: EXP_OUT1 OFF */
    gpio_ctrl_exp_out_set(EXP_OUT_1, false);
    LOG_INF("SEQ: EXP_OUT1 OFF - polling EXP_IN_0 (max %u s)",
            params.t3_ms / 1000);

    /* Step 4: polling EXP_IN_0 ogni 50ms */
    for (uint32_t elapsed = 0; elapsed < params.t3_ms; elapsed += 50) {
        if (stop_requested) goto gen_stop;
        if (gpio_ctrl_exp_in_get(EXP_IN_0) == 1) {
            LOG_INF("SEQ: EXP_IN_0 HIGH rilevato dopo %u ms", elapsed);
            return true;
        }
        k_msleep(50);
    }

    LOG_WRN("SEQ: timeout EXP_IN_0 - accensione fallita");
    return false;

gen_stop:
    LOG_INF("SEQ: STOP ricevuto durante accensione generatore");
    gpio_ctrl_exp_out_set(EXP_OUT_1, false);
    gpio_ctrl_exp_out_set(EXP_OUT_0, false);
    return false;
}

/**
 * @brief Spegne il generatore (EXP_OUT0 OFF).
 */
static void generatore_spegni(void)
{
    gpio_ctrl_exp_out_set(EXP_OUT_0, false);
    LOG_INF("SEQ: EXP_OUT0 OFF - generatore spento");
}

/* ============================================================================
 * Handler sequenza START (generatore)
 * ============================================================================ */

static void seq_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    LOG_INF("SEQ START: avvio - T1=%us T2=%us T3=%us T4=%umin S1=%.1fV",
            params.t1_ms / 1000,
            params.t2_ms / 1000,
            params.t3_ms / 1000,
            params.t4_min,
            (double)params.s1_v);

    /* Accensione generatore */
    bool ok = generatore_accendi();

    if (!ok) {
        if (stop_requested) {
            sms_send(reply_to, "Generatore spento su richiesta STOP");
        } else {
            sms_send(reply_to,
                     "Accensione FALLITA - nessun segnale su EXP_IN_0");
            generatore_spegni();
        }
        state = SEQ_IDLE;
        LOG_INF("SEQ START: terminata - IDLE");
        return;
    }

    /* Accensione OK */
    sms_send(reply_to, "Accensione OK - generatore operativo");

    /* Attesa T4 (minuti) controllando STOP ogni secondo */
    uint32_t t4_ms = params.t4_min * 60000U;
    LOG_INF("SEQ START: attesa T4 (%u min)", params.t4_min);
    for (uint32_t i = 0; i < t4_ms; i += 1000) {
        if (stop_requested) {
            LOG_INF("SEQ START: STOP ricevuto durante T4");
            break;
        }
        k_msleep(1000);
    }

    /* Spegnimento generatore */
    generatore_spegni();

    if (stop_requested) {
        sms_send(reply_to, "Generatore spento su richiesta STOP");
    } else {
        sms_send(reply_to, "Spegnimento generatore completato");
    }

    state = SEQ_IDLE;
    LOG_INF("SEQ START: terminata - IDLE");
}

K_WORK_DEFINE(seq_work, seq_work_handler);

/* ============================================================================
 * Handler sequenza START POMPA (generatore + pompa)
 * ============================================================================ */

static void seq_pompa_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    LOG_INF("SEQ POMPA: avvio - T5=%us T6=%umin",
            params.t5_ms / 1000,
            params.t6_min);

    /* Accensione generatore */
    bool ok = generatore_accendi();

    if (!ok) {
        if (stop_requested) {
            sms_send(reply_to, "Generatore spento su richiesta STOP");
        } else {
            sms_send(reply_to,
                     "Accensione FALLITA - nessun segnale su EXP_IN_0");
            generatore_spegni();
        }
        state = SEQ_IDLE;
        LOG_INF("SEQ POMPA: terminata - IDLE");
        return;
    }

    /* Accensione OK */
    sms_send(reply_to, "Accensione OK - generatore operativo");

    /* Attesa T5 prima di accendere la pompa */
    LOG_INF("SEQ POMPA: attesa T5 (%u s)", params.t5_ms / 1000);
    for (uint32_t i = 0; i < params.t5_ms; i += 100) {
        if (stop_requested) goto pompa_stop;
        k_msleep(100);
    }

    /* Accensione pompa */
    gpio_ctrl_exp_out_set(EXP_OUT_2, true);
    LOG_INF("SEQ POMPA: EXP_OUT2 ON - pompa avviata");
    sms_send(reply_to, "Pompa avviata");

    /* Attesa T6 | EXP_IN_1 HIGH | STOP */
    {
        uint32_t t6_ms  = params.t6_min * 60000U;
        uint32_t elapsed = 0;

        LOG_INF("SEQ POMPA: attesa T6=%umin | EXP_IN_1 | STOP",
                params.t6_min);

        while (1) {
            if (stop_requested) {
                LOG_INF("SEQ POMPA: STOP ricevuto");
                break;
            }
            if (gpio_ctrl_exp_in_get(EXP_IN_1) == 1) {
                LOG_INF("SEQ POMPA: EXP_IN_1 HIGH - stop pompa");
                break;
            }
            if (t6_ms > 0 && elapsed >= t6_ms) {
                LOG_INF("SEQ POMPA: timeout T6 (%u min)", params.t6_min);
                break;
            }
            k_msleep(1000);
            elapsed += 1000;
        }
    }

pompa_stop:
    /* Spegnimento pompa e generatore */
    gpio_ctrl_exp_out_set(EXP_OUT_2, false);
    LOG_INF("SEQ POMPA: EXP_OUT2 OFF - pompa spenta");
    generatore_spegni();

    if (stop_requested) {
        sms_send(reply_to, "Generatore e pompa spenti su richiesta STOP");
    } else {
        sms_send(reply_to, "Spegnimento generatore e pompa completato");
    }

    state = SEQ_IDLE;
    LOG_INF("SEQ POMPA: terminata - IDLE");
}

K_WORK_DEFINE(seq_pompa_work, seq_pompa_handler);

/* ============================================================================
 * Handler sequenza TEST
 * Attiva EXP_OUT0-7 in sequenza a 0.5s, si ferma quando EXP_IN_0=1
 * ============================================================================ */

static void seq_test_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    LOG_INF("SEQ TEST: avvio - ciclo EXP_OUT0-7, attesa EXP_IN_0 HIGH");

    /* Spegni tutte le uscite prima di iniziare */
    for (int i = 0; i < EXP_OUT_COUNT; i++) {
        gpio_ctrl_exp_out_set(i, false);
    }

    while (gpio_ctrl_exp_in_get(EXP_IN_0) == 0) {
        for (int i = 0; i < EXP_OUT_COUNT; i++) {
            if (gpio_ctrl_exp_in_get(EXP_IN_0) == 1) {
                LOG_INF("SEQ TEST: EXP_IN_0 HIGH - interruzione");
                goto test_done;
            }
            gpio_ctrl_exp_out_set(i, true);
            LOG_INF("SEQ TEST: EXP_OUT%d ON", i);
            k_msleep(500);
            gpio_ctrl_exp_out_set(i, false);
        }
    }

test_done:
    for (int i = 0; i < EXP_OUT_COUNT; i++) {
        gpio_ctrl_exp_out_set(i, false);
    }

    LOG_INF("SEQ TEST: terminata");
    sms_send(reply_to, "TEST completato - EXP_IN_0 HIGH rilevato");

    state = SEQ_IDLE;
    LOG_INF("SEQ TEST: IDLE, pronto per nuovo comando");
}

K_WORK_DEFINE(seq_test_work, seq_test_handler);

/* ============================================================================
 * API pubblica
 * ============================================================================ */

int sequence_init(void)
{
    int ret = settings_subsys_init();
    if (ret < 0) {
        LOG_WRN("Settings non disponibile (%d) - uso valori default", ret);
        return 0;
    }

    settings_load_subtree("seq");

    LOG_INF("Parametri: T1=%us T2=%us T3=%us T4=%umin T5=%us T6=%umin S1=%.1fV",
            params.t1_ms  / 1000,
            params.t2_ms  / 1000,
            params.t3_ms  / 1000,
            params.t4_min,
            params.t5_ms  / 1000,
            params.t6_min,
            (double)params.s1_v);
    return 0;
}

int sequence_start(const char *sender)
{
    if (state == SEQ_RUNNING) {
        LOG_WRN("sequence_start: sequenza gia' in corso");
        return -EBUSY;
    }

    strncpy(reply_to, sender, sizeof(reply_to) - 1);
    reply_to[sizeof(reply_to) - 1] = '\0';

    state = SEQ_RUNNING;
    stop_requested = false;

    int ret = k_work_submit(&seq_work);
    if (ret < 0) {
        LOG_ERR("k_work_submit fallito: %d", ret);
        state = SEQ_IDLE;
        return ret;
    }

    LOG_INF("SEQ START: accodata, risposta a: %s", reply_to);
    return 0;
}

int sequence_start_pompa(const char *sender)
{
    if (state == SEQ_RUNNING) {
        LOG_WRN("sequence_start_pompa: sequenza gia' in corso");
        return -EBUSY;
    }

    strncpy(reply_to, sender, sizeof(reply_to) - 1);
    reply_to[sizeof(reply_to) - 1] = '\0';

    state = SEQ_RUNNING;
    stop_requested = false;

    int ret = k_work_submit(&seq_pompa_work);
    if (ret < 0) {
        LOG_ERR("k_work_submit pompa fallito: %d", ret);
        state = SEQ_IDLE;
        return ret;
    }

    LOG_INF("SEQ POMPA: accodata, risposta a: %s", reply_to);
    return 0;
}

int sequence_test_start(const char *sender)
{
    if (state == SEQ_RUNNING) {
        LOG_WRN("sequence_test_start: sequenza gia' in corso");
        return -EBUSY;
    }

    strncpy(reply_to, sender, sizeof(reply_to) - 1);
    reply_to[sizeof(reply_to) - 1] = '\0';

    state = SEQ_RUNNING;

    int ret = k_work_submit(&seq_test_work);
    if (ret < 0) {
        LOG_ERR("k_work_submit test fallito: %d", ret);
        state = SEQ_IDLE;
        return ret;
    }

    LOG_INF("SEQ TEST: accodata, risposta a: %s", reply_to);
    return 0;
}

void sequence_stop(const char *sender)
{
    if (state != SEQ_RUNNING) {
        LOG_WRN("STOP ricevuto ma nessuna sequenza in corso");
        sms_send(sender, "Nessuna sequenza in corso");
        return;
    }
    LOG_INF("STOP richiesto da %s", sender);
    strncpy(reply_to, sender, sizeof(reply_to) - 1);
    reply_to[sizeof(reply_to) - 1] = '\0';
    stop_requested = true;
}

sequence_state_t sequence_get_state(void)
{
    return state;
}

const sequence_params_t *sequence_get_params(void)
{
    return &params;
}

int sequence_set_param(const char *key, uint32_t value_s)
{
    char settings_key[16];
    uint32_t val_to_save;

    if (strcmp(key, "T1") == 0) {
        if (value_s == 0 || value_s > 300) return -EINVAL;
        params.t1_ms = value_s * 1000;
        val_to_save  = params.t1_ms;
        snprintf(settings_key, sizeof(settings_key), "seq/t1");
    } else if (strcmp(key, "T2") == 0) {
        if (value_s == 0 || value_s > 300) return -EINVAL;
        params.t2_ms = value_s * 1000;
        val_to_save  = params.t2_ms;
        snprintf(settings_key, sizeof(settings_key), "seq/t2");
    } else if (strcmp(key, "T3") == 0) {
        if (value_s == 0 || value_s > 300) return -EINVAL;
        params.t3_ms = value_s * 1000;
        val_to_save  = params.t3_ms;
        snprintf(settings_key, sizeof(settings_key), "seq/t3");
    } else if (strcmp(key, "T4") == 0) {
        if (value_s > 1440) return -EINVAL;  /* max 24 ore */
        params.t4_min = value_s;
        val_to_save   = params.t4_min;
        snprintf(settings_key, sizeof(settings_key), "seq/t4");
    } else if (strcmp(key, "T5") == 0) {
        if (value_s == 0 || value_s > 300) return -EINVAL;
        params.t5_ms = value_s * 1000;
        val_to_save  = params.t5_ms;
        snprintf(settings_key, sizeof(settings_key), "seq/t5");
    } else if (strcmp(key, "T6") == 0) {
        if (value_s > 1440) return -EINVAL;  /* max 24 ore */
        params.t6_min = value_s;
        val_to_save   = params.t6_min;
        snprintf(settings_key, sizeof(settings_key), "seq/t6");
    } else {
        LOG_ERR("Chiave non valida: '%s' (usa T1-T6)", key);
        return -EINVAL;
    }

    int ret = settings_save_one(settings_key, &val_to_save, sizeof(val_to_save));
    if (ret < 0) {
        LOG_ERR("Errore salvataggio NVS '%s': %d", settings_key, ret);
    } else {
        LOG_INF("Salvato in NVS: %s = %u", key, value_s);
    }
    return ret;
}

int sequence_set_s1(float volt)
{
    if (volt < 10.0f || volt > 14.0f) {
        LOG_ERR("S1 fuori range: %.2f (valido 10.0-14.0 V)", (double)volt);
        return -EINVAL;
    }
    params.s1_v = volt;
    int ret = settings_save_one("seq/s1", &volt, sizeof(volt));
    if (ret < 0) {
        LOG_ERR("Errore salvataggio NVS S1: %d", ret);
    } else {
        LOG_INF("Salvato in NVS: S1 = %.2f V", (double)volt);
    }
    return ret;
}

int sequence_set_autostart(bool enabled)
{
    params.autostart = enabled;
    uint32_t val = (uint32_t)enabled;
    int ret = settings_save_one("seq/auto", &val, sizeof(val));
    if (ret == 0) {
        LOG_INF("Salvato: AUTOSTART = %s", enabled ? "ON" : "OFF");
    }
    return ret;
}