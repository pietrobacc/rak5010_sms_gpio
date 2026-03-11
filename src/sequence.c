/*
 * sequence.c - Macchina a stati sequenza di avvio (implementazione k_work)
 *
 * La sequenza viene eseguita tramite un k_work sottomesso al system work
 * queue di Zephyr. A differenza di K_THREAD_DEFINE + k_thread_start(),
 * un k_work puo' essere sottomesso quante volte si vuole: ogni chiamata
 * a sequence_start() pianifica una nuova esecuzione completa dall'inizio,
 * indipendentemente da quante volte sia gia' stata eseguita in precedenza.
 *
 * Questo risolve la limitazione della versione precedente dove START
 * funzionava correttamente solo la prima volta dopo l'accensione.
 *
 * Flusso di esecuzione:
 *
 *   [SMS "START"]
 *        │
 *        ▼
 *   sequence_start()
 *        │
 *        ▼
 *   k_work_submit(&seq_work)  ← ritorna subito, non blocca il main
 *        │
 *        ▼ (system work queue thread)
 *   seq_work_handler()
 *        ├─ Step 1: OUT1 ON  + attesa T1
 *        ├─ Step 2: OUT2 ON  + attesa T2
 *        ├─ Step 3: OUT1+OUT2 OFF
 *        ├─ Step 4: polling IN4 ogni 50ms (max T3)
 *        └─ Step 5: SMS esito + state = IDLE
 *                                  │
 *                                  ▼
 *                          [SMS "START"] successivo
 *                          funziona correttamente ✓
 *
 * NOTA: k_msleep() dentro il work handler blocca il system work queue
 * thread per la durata delle attese. Il thread main (polling SMS) non
 * e' influenzato e continua a girare normalmente in parallelo.
 * Assicurarsi che CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE in prj.conf sia
 * sufficiente (2048 byte sono adeguati per questa sequenza).
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

/** Parametri temporali con valori di default (modificabili via SMS) */
static sequence_params_t params = {
    .t1_ms = 5000,
    .t2_ms = 5000,
    .t3_ms = 5000,
};

/** Stato corrente della macchina a stati */
static sequence_state_t state = SEQ_IDLE;

/**
 * Numero del mittente a cui inviare il SMS di esito.
 * Scritto da sequence_start() prima della sottomissione del work,
 * letto da seq_work_handler() durante l'esecuzione.
 */
static char reply_to[24];

/* ============================================================================
 * Settings subsystem - Persistenza parametri in NVS
 * ============================================================================ */

/**
 * @brief Callback invocata dal Settings subsystem al caricamento.
 *
 * Il parametro key contiene la parte dopo il prefisso "seq/"
 * (es. "t1", "t2", "t3"). Aggiorna i parametri in RAM con i
 * valori letti dal backend NVS flash.
 */
static int settings_set_cb(const char *key, size_t len,
                           settings_read_cb read_cb, void *cb_arg)
{
    uint32_t val;

    if (len != sizeof(val)) {
        LOG_WRN("Settings: dimensione non valida per '%s'", key);
        return -EINVAL;
    }
    if (read_cb(cb_arg, &val, sizeof(val)) != sizeof(val)) {
        LOG_ERR("Settings: errore lettura '%s'", key);
        return -EIO;
    }

    if      (strcmp(key, "t1") == 0) { params.t1_ms = val; }
    else if (strcmp(key, "t2") == 0) { params.t2_ms = val; }
    else if (strcmp(key, "t3") == 0) { params.t3_ms = val; }
    else { LOG_WRN("Settings: chiave sconosciuta '%s'", key); }

    return 0;
}

/*
 * Registrazione statica dell'handler Settings.
 * Il prefisso "seq" identifica tutte le chiavi di questo modulo
 * nel backend NVS ("seq/t1", "seq/t2", "seq/t3").
 */
SETTINGS_STATIC_HANDLER_DEFINE(seq, "seq", NULL, settings_set_cb, NULL, NULL);

/* ============================================================================
 * k_work handler - Corpo della sequenza
 *
 * Questa funzione viene eseguita nel contesto del system work queue
 * di Zephyr ogni volta che k_work_submit(&seq_work) viene chiamato.
 *
 * Differenza chiave rispetto al vecchio approccio con K_THREAD_DEFINE:
 *
 *   VECCHIO (K_THREAD_DEFINE + k_thread_start):
 *     - Il thread viene creato una sola volta
 *     - k_thread_start() funziona solo se il thread non e' mai partito
 *     - Dopo la prima esecuzione, k_thread_start() non fa nulla
 *     - Risultato: START funziona solo la prima volta
 *
 *   NUOVO (K_WORK_DEFINE + k_work_submit):
 *     - Il work item e' riutilizzabile
 *     - k_work_submit() puo' essere chiamato infinite volte
 *     - Ogni submit dopo il completamento avvia una nuova esecuzione
 *     - Risultato: START funziona ogni volta ✓
 * ============================================================================ */

/**
 * @brief Handler del work item: esegue la sequenza completa.
 *
 * @param work  Puntatore al k_work sottomesso (non usato direttamente).
 */
static void seq_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    LOG_INF("SEQ: avvio - T1=%u s  T2=%u s  T3=%u s",
            params.t1_ms / 1000,
            params.t2_ms / 1000,
            params.t3_ms / 1000);

    /* ------------------------------------------------------------------
     * Step 1: Attiva OUT1, attendi T1
     * OUT1 si attiva e rimane attivo per tutta la durata di T1.
     * ------------------------------------------------------------------ */
    gpio_ctrl_output_set(GPIO_OUT_1, true);
    LOG_INF("SEQ: OUT1 ON - attesa T1 (%u s)", params.t1_ms / 1000);
    k_msleep(params.t1_ms);

    /* ------------------------------------------------------------------
     * Step 2: Attiva OUT2 (con OUT1 ancora attivo), attendi T2
     * In questo intervallo entrambi OUT1 e OUT2 sono attivi.
     * ------------------------------------------------------------------ */
    gpio_ctrl_output_set(GPIO_OUT_2, true);
    LOG_INF("SEQ: OUT2 ON - attesa T2 (%u s)", params.t2_ms / 1000);
    k_msleep(params.t2_ms);

    /* ------------------------------------------------------------------
     * Step 3: Disattiva entrambi gli output
     * Il dispositivo controllato dovrebbe ora essere autonomo.
     * ------------------------------------------------------------------ */
    gpio_ctrl_output_set(GPIO_OUT_1, false);
    gpio_ctrl_output_set(GPIO_OUT_2, false);
    LOG_INF("SEQ: OUT1+OUT2 OFF - polling IN4 (max %u s)",
            params.t3_ms / 1000);

    /* ------------------------------------------------------------------
     * Step 4: Polling IN4 ogni 50 ms fino a timeout T3
     *
     * Campionamento a 50 ms: buon compromesso tra reattivita' (max
     * 50 ms di ritardo nel rilevamento) e carico sul work queue.
     * ------------------------------------------------------------------ */
    bool     in4_ok  = false;
    uint32_t elapsed = 0;

    while (elapsed < params.t3_ms) {
        if (gpio_ctrl_in4_get() == 1) {
            in4_ok  = true;
            LOG_INF("SEQ: IN4 HIGH rilevato dopo %u ms", elapsed);
            break;
        }
        k_msleep(50);
        elapsed += 50;
    }

    /* ------------------------------------------------------------------
     * Step 5: SMS di esito al mittente originale del comando START
     * ------------------------------------------------------------------ */
    if (in4_ok) {
        LOG_INF("SEQ: completata con SUCCESSO");
        sms_send(reply_to, "Accensione OK - dispositivo operativo");
    } else {
        LOG_WRN("SEQ: FALLITA - IN4 non HIGH entro %u s",
                params.t3_ms / 1000);
        sms_send(reply_to, "Accensione FALLITA - nessun segnale su IN4");
    }

    /* Sicurezza: assicura tutti gli output spenti al termine */
    gpio_ctrl_output_set(GPIO_OUT_1, false);
    gpio_ctrl_output_set(GPIO_OUT_2, false);
    gpio_ctrl_output_set(GPIO_OUT_3, false);

    /*
     * Transizione di stato RUNNING -> IDLE.
     * Avviene DOPO l'invio dell'SMS: un nuovo START ricevuto durante
     * la sequenza viene correttamente rifiutato (state == SEQ_RUNNING).
     * Da questo momento sequence_start() accettera' un nuovo START.
     */
    state = SEQ_IDLE;
    LOG_INF("SEQ: terminata - IDLE, pronto per nuovo START");
}

/*
 * Definizione e inizializzazione statica del work item.
 *
 * K_WORK_DEFINE(nome, handler) e' equivalente a:
 *   struct k_work nome;
 *   k_work_init(&nome, handler);
 *
 * Non alloca uno stack separato: usa lo stack del system work queue
 * thread, la cui dimensione e' configurata in prj.conf con:
 *   CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048
 */
K_WORK_DEFINE(seq_work, seq_work_handler);

/* ============================================================================
 * API pubblica
 * ============================================================================ */

int sequence_init(void)
{
    int ret = settings_subsys_init();
    if (ret < 0) {
        LOG_WRN("Settings non disponibile (%d) - uso valori default", ret);
        return 0;  /* Non fatale */
    }

    /*
     * Carica tutte le chiavi "seq" dal backend NVS.
     * Invoca settings_set_cb() per ognuna trovata.
     */
    settings_load_subtree("seq");

    LOG_INF("Parametri sequenza caricati: T1=%u s  T2=%u s  T3=%u s",
            params.t1_ms / 1000,
            params.t2_ms / 1000,
            params.t3_ms / 1000);
    return 0;
}

int sequence_start(const char *sender)
{
    if (state == SEQ_RUNNING) {
        LOG_WRN("sequence_start: sequenza gia' in corso, ignorato");
        return -EBUSY;
    }

    /* Copia il numero del mittente prima del submit */
    strncpy(reply_to, sender, sizeof(reply_to) - 1);
    reply_to[sizeof(reply_to) - 1] = '\0';

    state = SEQ_RUNNING;

    /*
     * k_work_submit() accoda seq_work nel system work queue e ritorna
     * immediatamente (non bloccante). Il work handler viene eseguito
     * appena il work queue thread e' disponibile.
     *
     * Valori di ritorno:
     *   0  = work accodato con successo
     *   1  = work gia' in coda (non ancora eseguito) - non accade qui
     *        perche' controlliamo state == SEQ_RUNNING prima
     *  <0  = errore
     */
    int ret = k_work_submit(&seq_work);
    if (ret < 0) {
        LOG_ERR("k_work_submit fallito: %d", ret);
        state = SEQ_IDLE;
        return ret;
    }

    LOG_INF("Sequenza accodata nel work queue, risposta a: %s", reply_to);
    return 0;
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
    if (value_s == 0 || value_s > 300) {
        LOG_ERR("Valore fuori range per %s: %u (valido 1-300 s)", key, value_s);
        return -EINVAL;
    }

    uint32_t val_ms = value_s * 1000;
    char settings_key[16];

    if (strcmp(key, "T1") == 0) {
        params.t1_ms = val_ms;
        snprintf(settings_key, sizeof(settings_key), "seq/t1");
    } else if (strcmp(key, "T2") == 0) {
        params.t2_ms = val_ms;
        snprintf(settings_key, sizeof(settings_key), "seq/t2");
    } else if (strcmp(key, "T3") == 0) {
        params.t3_ms = val_ms;
        snprintf(settings_key, sizeof(settings_key), "seq/t3");
    } else {
        LOG_ERR("Chiave non valida: '%s' (usa T1, T2 o T3)", key);
        return -EINVAL;
    }

    /*
     * settings_save_one() scrive immediatamente in NVS.
     * Il valore persiste ai riavvii e viene ricaricato da
     * settings_load_subtree("seq") nel prossimo sequence_init().
     */
    int ret = settings_save_one(settings_key, &val_ms, sizeof(val_ms));
    if (ret < 0) {
        LOG_ERR("Errore salvataggio NVS '%s': %d", settings_key, ret);
    } else {
        LOG_INF("Salvato in NVS: %s = %u s", key, value_s);
    }

    return ret;
}
