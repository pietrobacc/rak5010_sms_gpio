/*
 * sequence.c - Macchina a stati sequenza di avvio (implementazione k_work)
 *
 * Output:
 *   EXP_OUT_0 - Generatore (rimane ON fino allo spegnimento)
 *   EXP_OUT_1 - Avviamento generatore (ON per T2, poi OFF)
 *   EXP_OUT_2 - Pompa (solo START POMPA, rimane ON fino allo spegnimento)
 *
 * Input:
 *   EXP_IN_0  - Feedback accensione generatore
 *   EXP_IN_1  - Trigger spegnimento pompa (solo START POMPA)
 *
 * Flusso START:
 *   OUT0 ON -> attesa T1 -> OUT1 ON -> attesa T2 -> OUT1 OFF
 *   -> polling IN0 (max T3)
 *   -> FAIL: OUT0 OFF + SMS
 *   -> OK: SMS + attesa T4 (inf se 0) | STOP -> OUT0 OFF + SMS
 *
 * Flusso START POMPA:
 *   OUT0 ON -> attesa T1 -> OUT1 ON -> attesa T2 -> OUT1 OFF
 *   -> polling IN0 (max T3)
 *   -> FAIL: OUT0 OFF + SMS
 *   -> OK: SMS + attesa T5 -> OUT2 ON + SMS
 *   -> attesa T6 (inf se 0) | IN1 HIGH | STOP
 *   -> OUT2 OFF + OUT0 OFF + SMS
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

static sequence_params_t params = {
    .t1_ms     = 5000,
    .t2_ms     = 5000,
    .t3_ms     = 5000,
    .t4_ms     = 10000,  /* ex t5_ms */
    .t5_min    = 0,      /* ex t4_min, infinito */
    .t6_min    = 0,
    .s1_v      = 12.5f,
    .autostart = false,
};

static sequence_state_t state = SEQ_IDLE;
static char reply_to[24];
static volatile bool stop_requested = false;

static bool generatore_on = false;
static bool pompa_on      = false;
static int64_t generatore_on_time = 0;  /* uptime ms quando acceso */
static int64_t pompa_on_time      = 0;

/* ============================================================================
 * Settings subsystem
 * ============================================================================ */

static int settings_set_cb(const char *key, size_t len,
                            settings_read_cb read_cb, void *cb_arg)
{
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
    else if (strcmp(key, "t4") == 0) params.t4_ms  = val_u;
    else if (strcmp(key, "t5") == 0) params.t5_min = val_u;
    else if (strcmp(key, "t6") == 0) params.t6_min = val_u;
    else if (strcmp(key, "auto") == 0) params.autostart = (bool)val_u;
    else LOG_WRN("Settings: chiave sconosciuta '%s'", key);

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(seq, "seq", NULL, settings_set_cb, NULL, NULL);

/* ============================================================================
 * Helper - spegnimento di emergenza immediato
 * ============================================================================ */

static void spegni_tutto(void)
{
    gpio_ctrl_exp_out_set(EXP_OUT_0, false);
    gpio_ctrl_exp_out_set(EXP_OUT_1, false);
    gpio_ctrl_exp_out_set(EXP_OUT_2, false);
    generatore_on = false;
    pompa_on      = false;
    LOG_INF("SEQ: tutti gli output spenti");
}

/* ============================================================================
 * Helper - accensione generatore
 * Comune a START e START POMPA.
 *
 * OUT0 ON -> attesa T1 -> OUT1 ON -> attesa T2 -> OUT1 OFF
 * -> polling IN0 ogni 50ms fino a T3
 *
 * @return true se IN0 HIGH rilevato, false se timeout o STOP.
 * ============================================================================ */

static bool generatore_accendi(void)
{
    /* Step 1: OUT0 ON + attesa T1 */
    gpio_ctrl_exp_out_set(EXP_OUT_0, true);
    LOG_INF("SEQ: OUT0 ON - attesa T1 (%u s)", params.t1_ms / 1000);
    for (uint32_t i = 0; i < params.t1_ms; i += 100) {
        if (stop_requested) goto gen_stop;
        k_msleep(100);
    }

    /* Step 2: OUT1 ON + attesa T2 */
    gpio_ctrl_exp_out_set(EXP_OUT_1, true);
    LOG_INF("SEQ: OUT1 ON - attesa T2 (%u s)", params.t2_ms / 1000);
    for (uint32_t i = 0; i < params.t2_ms; i += 100) {
        if (stop_requested) goto gen_stop;
        k_msleep(100);
    }

    /* Step 3: OUT1 OFF */
    gpio_ctrl_exp_out_set(EXP_OUT_1, false);
    LOG_INF("SEQ: OUT1 OFF - polling IN0 (max %u s)", params.t3_ms / 1000);

    /* Step 4: polling IN0 ogni 50ms fino a T3 */
    for (uint32_t elapsed = 0; elapsed < params.t3_ms; elapsed += 50) {
        if (stop_requested) goto gen_stop;
        if (gpio_ctrl_exp_in_get(EXP_IN_0) == 1) {
            LOG_INF("SEQ: IN0 HIGH dopo %u ms", elapsed);
            generatore_on = true;
            generatore_on_time = k_uptime_get();
            return true;
        }
        k_msleep(50);
    }

    LOG_WRN("SEQ: timeout IN0 - accensione fallita");
    return false;

gen_stop:
    LOG_INF("SEQ: STOP durante accensione generatore");
    return false;
}

/* ============================================================================
 * Helper - attesa spegnimento con 3 trigger
 * Usato sia da START (solo T5+STOP) che da START POMPA (T6+IN1+STOP)
 *
 * @param timeout_min  Timeout in minuti (0 = infinito)
 * @param check_in1    true = controlla anche EXP_IN_1
 * @return motivo dello stop: 0=timeout, 1=IN1, 2=STOP
 * ============================================================================ */

static int attendi_spegnimento(uint32_t timeout_min, bool check_in1)
{
    uint32_t elapsed = 0;
    uint32_t timeout_ms = timeout_min * 60000U;

    LOG_INF("SEQ: attesa spegnimento - timeout=%umin IN1=%s",
            timeout_min, check_in1 ? "SI" : "NO");

    while (1) {
        if (stop_requested) {
            LOG_INF("SEQ: trigger STOP");
            return 2;
        }
        if (check_in1 && gpio_ctrl_exp_in_get(EXP_IN_1) == 1) {
            LOG_INF("SEQ: trigger IN1 HIGH dopo %u s", elapsed / 1000);
            return 1;
        }
        if (timeout_ms > 0 && elapsed >= timeout_ms) {
            LOG_INF("SEQ: trigger timeout T=%umin", timeout_min);
            return 0;
        }
        k_msleep(1000);
        elapsed += 1000;
    }
}

/* ============================================================================
 * Handler sequenza START (solo generatore)
 * ============================================================================ */

static void seq_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    LOG_INF("SEQ START: T1=%us T2=%us T3=%us T5=%umin",
            params.t1_ms / 1000, params.t2_ms / 1000,
            params.t3_ms / 1000, params.t5_min);

    /* Accensione generatore */
    bool ok = generatore_accendi();

    if (!ok) {
        spegni_tutto();
        if (stop_requested) {
            sms_send(reply_to, "Generatore spento - STOP ricevuto");
        } else {
            sms_send(reply_to, "Accensione FALLITA - nessun feedback su IN0");
        }
        state = SEQ_IDLE;
        return;
    }

    /* Accensione OK */
    LOG_INF("SEQ START: generatore acceso - attesa spegnimento T5=%umin",
            params.t5_min);
    sms_send(reply_to, "Generatore acceso");

    /* Attesa T5 | STOP (no IN1) */
    int trigger = attendi_spegnimento(params.t5_min, false);

    /* Spegnimento */
    spegni_tutto();

    switch (trigger) {
    case 0:
        sms_send(reply_to, "Generatore spento - timeout T5");
        break;
    case 2:
        sms_send(reply_to, "Generatore spento - STOP ricevuto");
        break;
    default:
        sms_send(reply_to, "Generatore spento");
        break;
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

    LOG_INF("SEQ POMPA: T1=%us T2=%us T3=%us T4=%us T6=%umin",
            params.t1_ms / 1000, params.t2_ms / 1000,
            params.t3_ms / 1000, params.t4_ms / 1000,
            params.t6_min);

    /* Accensione generatore */
    bool ok = generatore_accendi();

    if (!ok) {
        spegni_tutto();
        if (stop_requested) {
            sms_send(reply_to, "Generatore spento - STOP ricevuto");
        } else {
            sms_send(reply_to, "Accensione FALLITA - nessun feedback su IN0");
        }
        state = SEQ_IDLE;
        return;
    }

    /* Accensione generatore OK */
    sms_send(reply_to, "Generatore acceso");
    LOG_INF("SEQ POMPA: generatore acceso - attesa T4 (%u s)",
            params.t4_ms / 1000);

    /* Attesa T4 prima di accendere la pompa */
    for (uint32_t i = 0; i < params.t4_ms; i += 100) {
        if (stop_requested) {
            spegni_tutto();
            sms_send(reply_to, "Generatore spento - STOP ricevuto");
            state = SEQ_IDLE;
            return;
        }
        k_msleep(100);
    }

    /* Accensione pompa */
    gpio_ctrl_exp_out_set(EXP_OUT_2, true);
    pompa_on      = true;
    pompa_on_time = k_uptime_get();
    LOG_INF("SEQ POMPA: OUT2 ON - pompa accesa");
    sms_send(reply_to, "Pompa accesa");

    /* Attesa T6 | IN1 | STOP */
    int trigger = attendi_spegnimento(params.t6_min, true);

    /* Spegnimento pompa e generatore */
    spegni_tutto();

    switch (trigger) {
    case 0:
        sms_send(reply_to, "Generatore e pompa spenti - timeout T6");
        break;
    case 1:
        sms_send(reply_to, "Generatore e pompa spenti - segnale IN1");
        break;
    case 2:
        sms_send(reply_to, "Generatore e pompa spenti - STOP ricevuto");
        break;
    default:
        sms_send(reply_to, "Generatore e pompa spenti");
        break;
    }

    state = SEQ_IDLE;
    LOG_INF("SEQ POMPA: terminata - IDLE");
}

K_WORK_DEFINE(seq_pompa_work, seq_pompa_handler);

/* ============================================================================
 * Handler sequenza TEST
 * Attiva EXP_OUT0-7 in sequenza a 0.5s, si ferma quando IN0=1 o STOP
 * ============================================================================ */

/*
static void seq_test_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    LOG_INF("SEQ TEST: avvio - ciclo OUT0-7, attesa IN0 HIGH o STOP");

    for (int i = 0; i < EXP_OUT_COUNT; i++) {
        gpio_ctrl_exp_out_set(i, false);
    }

    while (gpio_ctrl_exp_in_get(EXP_IN_0) == 0) {
        for (int i = 0; i < EXP_OUT_COUNT; i++) {
            if (stop_requested) {
                LOG_INF("SEQ TEST: STOP ricevuto");
                goto test_done;
            }
            if (gpio_ctrl_exp_in_get(EXP_IN_0) == 1) {
                LOG_INF("SEQ TEST: IN0 HIGH");
                goto test_done;
            }
            gpio_ctrl_exp_out_set(i, true);
            LOG_INF("SEQ TEST: OUT%d ON", i);
            k_msleep(500);
            gpio_ctrl_exp_out_set(i, false);
        }
    }

test_done:
    spegni_tutto();
    LOG_INF("SEQ TEST: terminata");

    if (stop_requested) {
        sms_send(reply_to, "TEST interrotto - STOP ricevuto");
    } else {
        sms_send(reply_to, "TEST completato - IN0 HIGH rilevato");
    }

    state = SEQ_IDLE;
    LOG_INF("SEQ TEST: IDLE");
} 

K_WORK_DEFINE(seq_test_work, seq_test_handler); 
*/

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

    LOG_INF("Parametri: T1=%us T2=%us T3=%us T4=%us T5=%umin T6=%umin "
        "S1=%.1fV Auto=%s",
        params.t1_ms  / 1000,
        params.t2_ms  / 1000,
        params.t3_ms  / 1000,
        params.t4_ms  / 1000,
        params.t5_min,
        params.t6_min,
        (double)params.s1_v,
        params.autostart ? "ON" : "OFF");
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

/*
int sequence_test_start(const char *sender)
{
    if (state == SEQ_RUNNING) {
        LOG_WRN("sequence_test_start: sequenza gia' in corso");
        return -EBUSY;
    }

    strncpy(reply_to, sender, sizeof(reply_to) - 1);
    reply_to[sizeof(reply_to) - 1] = '\0';

    state = SEQ_RUNNING;
    stop_requested = false;

    int ret = k_work_submit(&seq_test_work);
    if (ret < 0) {
        LOG_ERR("k_work_submit test fallito: %d", ret);
        state = SEQ_IDLE;
        return ret;
    }

    LOG_INF("SEQ TEST: accodata, risposta a: %s", reply_to);
    return 0;
}
*/

void sequence_stop(const char *sender)
{
    LOG_INF("STOP richiesto da %s", sender);

    /* Spegnimento immediato di tutti gli output */
    spegni_tutto();

    if (state != SEQ_RUNNING) {
        LOG_WRN("STOP: nessuna sequenza in corso");
        sms_send(sender, "STOP ignorato! (nessuna sequenza in corso)");
        return;
    }

    /* Aggiorna il destinatario della risposta */
    strncpy(reply_to, sender, sizeof(reply_to) - 1);
    reply_to[sizeof(reply_to) - 1] = '\0';

    /* Segnala al work handler di terminare */
    stop_requested = true;

    //sms_send(sender, "STOP: generatore e pompa spenti");
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
        if (value_s == 0 || value_s > 300) return -EINVAL;
        params.t4_ms = value_s * 1000;
        val_to_save  = params.t4_ms;
        snprintf(settings_key, sizeof(settings_key), "seq/t4");
    } else if (strcmp(key, "T5") == 0) {
        if (value_s > 1440) return -EINVAL;
        params.t5_min = value_s;
        val_to_save   = params.t5_min;
        snprintf(settings_key, sizeof(settings_key), "seq/t5");
    } else if (strcmp(key, "T6") == 0) {
        if (value_s > 1440) return -EINVAL;
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

bool sequence_generatore_is_on(uint32_t *uptime_s)
{
    if (uptime_s != NULL) {
        if (generatore_on) {
            *uptime_s = (uint32_t)((k_uptime_get() - generatore_on_time) / 1000);
        } else {
            *uptime_s = 0;
        }
    }
    return generatore_on;
}

bool sequence_pompa_is_on(uint32_t *uptime_s)
{
    if (uptime_s != NULL) {
        if (pompa_on) {
            *uptime_s = (uint32_t)((k_uptime_get() - pompa_on_time) / 1000);
        } else {
            *uptime_s = 0;
        }
    }
    return pompa_on;
}