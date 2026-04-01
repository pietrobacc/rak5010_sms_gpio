/*
 * main.c - Loop principale e dispatcher comandi SMS
 *
 * Punto di ingresso dell'applicazione. Gestisce:
 *   - Inizializzazione sequenziale di tutti i sottosistemi
 *   - Parsing e dispatch dei comandi SMS ricevuti
 *   - Loop di polling SMS con lampeggio LED di heartbeat
 *
 * Comandi SMS supportati:
 *   START           - Avvia la sequenza di attivazione GPIO
 *   CONFIG          - Risponde con i parametri T1/T2/T3 correnti
 *   SET T1 <sec>    - Imposta T1 (pausa OUT1->OUT2) e salva in NVS
 *   SET T2 <sec>    - Imposta T2 (pausa OUT2->OFF) e salva in NVS
 *   SET T3 <sec>    - Imposta T3 (timeout attesa IN4) e salva in NVS
 *
 * I comandi sono case-insensitive (es. "start" = "START").
 * Il confronto avviene dopo conversione in maiuscolo del testo SMS.
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>
#include <string.h>
#include <stdlib.h>
#include "gpio_ctrl.h"
#include "modem.h"
#include "sms.h"
#include "sequence.h"


LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *shtc3_dev;

/* ============================================================================
 * Configurazione utente
 * Modificare questi valori prima della compilazione.
 * ============================================================================ */

/**
 * Numero di telefono autorizzato a inviare comandi.
 * Formato internazionale con prefisso (es. "+41791234567").
 * Lasciare stringa vuota "" per accettare comandi da qualsiasi numero.
 */
#define AUTHORIZED_NUMBER  ""

/**
 * Abilita l'invio di SMS di conferma/esito al mittente.
 * Impostare a false per disabilitare le risposte SMS (es. in test).
 */
#define REPLY_ENABLED      true

/* ============================================================================
 * Definizioni hardware e parametri ADC
 * ============================================================================ */

#define VEXT_DIVIDER_FACTOR  9.333f   /* (100k + 12k) / 12k */

static const struct adc_dt_spec adc_vext =
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/* ============================================================================
 * Funzioni di utilita' interne
 * ============================================================================ */

/**
 * @brief Verifica se il mittente e' autorizzato a inviare comandi.
 *
 * Se AUTHORIZED_NUMBER e' una stringa vuota, tutti i mittenti
 * sono autorizzati (modalita' aperta, usare con cautela).
 *
 * @param sender  Numero mittente nel formato "+XXXXXXXXXXX".
 * @return true se autorizzato, false altrimenti.
 */
static bool is_authorized(const char *sender)
{
    if (strlen(AUTHORIZED_NUMBER) == 0) {
        return true;
    }
    return (strcmp(sender, AUTHORIZED_NUMBER) == 0);
}

/**
 * @brief Converte una stringa in maiuscolo in-place.
 *
 * Usato per rendere il parsing dei comandi SMS case-insensitive.
 * Opera solo sui caratteri ASCII a-z, lascia invariati gli altri.
 *
 * @param s  Stringa da convertire.
 * @param n  Numero massimo di caratteri da processare.
 */
static void str_upper(char *s, size_t n)
{
    for (size_t i = 0; i < n && s[i] != '\0'; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') {
            s[i] -= 32;
        }
    }
}

/**
 * @brief Legge i dati del sensore SHTC3.
 * 
 * @param temp  Puntatore al valore di temperatura.
 * @param hum   Puntatore al valore di umidità.
 */
static void read_sensor(float *temp, float *hum)
{
    if (!device_is_ready(shtc3_dev)) {
        LOG_WRN("SHTC3 non pronto");
        *temp = -999.0f;
        *hum  = -999.0f;
        return;
    }
    struct sensor_value t, h;
    sensor_sample_fetch(shtc3_dev);
    sensor_channel_get(shtc3_dev, SENSOR_CHAN_AMBIENT_TEMP, &t);
    sensor_channel_get(shtc3_dev, SENSOR_CHAN_HUMIDITY,     &h);
    *temp = sensor_value_to_float(&t);
    *hum  = sensor_value_to_float(&h);
}

/**
 * @brief Legge la tensione della batteria esterna 12V tramite partitore.
 *
 * Partitore: R_high=100kΩ, R_low=12kΩ → fattore 9.333
 * Pin: P0.05 (AIN3) - J10 pin 3
 *
 * @return Tensione in Volt, -1.0 in caso di errore.
 */
static float read_vext(void)
{
    int16_t buf;
    struct adc_sequence seq = {
        .buffer      = &buf,
        .buffer_size = sizeof(buf),
    };
    adc_sequence_init_dt(&adc_vext, &seq);

    if (adc_read_dt(&adc_vext, &seq) != 0) {
        LOG_ERR("VEXT: adc_read fallito");
        return -1.0f;
    }

    int32_t val_mv = buf;
    adc_raw_to_millivolts_dt(&adc_vext, &val_mv);
    return (float)val_mv / 1000.0f * VEXT_DIVIDER_FACTOR;
}


/* ============================================================================
 * Handler comandi SMS
 * ============================================================================ */

/**  
 * @brief Gestisce il comando CONFIG.
 *
 * Legge i parametri correnti della sequenza (T1/T2/T3) e li invia
 * via SMS al mittente. Utile per verificare i valori salvati in NVS
 * senza dover consultare il log RTT.
 *
 * @param sender  Numero a cui inviare la risposta.
 */
static void handle_config(const char *sender)
{
    const sequence_params_t *p = sequence_get_params();
    char msg[256];
    
    float temp, hum;
    read_sensor(&temp, &hum);

    uint8_t bat_pct;
    uint16_t bat_mv;
    modem_get_battery(&bat_pct, &bat_mv);

    float vext = read_vext();

    snprintf(msg, sizeof(msg),
             "Parametri correnti:\n"
             "T1 = %u s\nT2 = %u s\nT3 = %u s\n"
             "Temp = %.1f C\nUmidita = %.1f %%\n"
             "VBAT = %u mV (%u %%)\n"
             "VEXT = %.2f V",
             p->t1_ms / 1000,
             p->t2_ms / 1000,
             p->t3_ms / 1000,
             (double)temp, 
             (double)hum,
             bat_mv,
             bat_pct,
             (double)vext);

    LOG_INF("CONFIG richiesto: %s", msg);

    if (REPLY_ENABLED) {
        sms_send(sender, msg);
    }
}

/**
 * @brief Gestisce il comando SET T1/T2/T3 <valore>.
 *
 * Formato atteso (dopo conversione maiuscolo): "SET T1 10"
 * Il valore viene validato nel range 1-300 secondi prima
 * di essere passato a sequence_set_param() per il salvataggio NVS.
 *
 * @param sender  Numero a cui inviare la conferma.
 * @param text    Testo SMS gia' convertito in maiuscolo.
 */
static void handle_set(const char *sender, const char *text)
{
    char     key[4] = {0};
    uint32_t val    = 0;

    /*
     * sscanf con formato "SET %3s %u":
     *   %3s  - legge max 3 caratteri (es. "T1", "T2", "T3")
     *   %u   - legge un intero senza segno
     * Restituisce 2 se entrambi i campi sono stati letti correttamente.
     */
    if (sscanf(text, "SET %3s %u", key, &val) != 2) {
        LOG_WRN("SET: formato non valido: '%s'", text);
        if (REPLY_ENABLED) {
            sms_send(sender,
                     "Formato: SET T1 <sec>\n"
                     "         SET T2 <sec>\n"
                     "         SET T3 <sec>\n"
                     "(valori: 1-300 secondi)");
        }
        return;
    }

    /* Validazione range lato main per feedback immediato */
    if (val == 0 || val > 300) {
        LOG_WRN("SET: valore fuori range: %u", val);
        if (REPLY_ENABLED) {
            sms_send(sender, "Valore non valido. Range: 1-300 secondi.");
        }
        return;
    }

    int ret = sequence_set_param(key, val);
    if (ret == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s impostato a %u s (salvato)", key, val);
        LOG_INF("%s", msg);
        if (REPLY_ENABLED) {
            sms_send(sender, msg);
        }
    } else {
        LOG_ERR("SET fallito per '%s': %d", key, ret);
        if (REPLY_ENABLED) {
            sms_send(sender, "Parametro non valido. Usa T1, T2 o T3.");
        }
    }
}

/* ============================================================================
 * Callback ricezione SMS
 *
 * Invocata da sms_poll() ogni volta che viene trovato un SMS non letto.
 * Esegue: autorizzazione -> normalizzazione testo -> dispatch comando.
 * ============================================================================ */

/**
 * @brief Callback invocata alla ricezione di ogni SMS.
 *
 * @param msg  Struttura contenente mittente, testo e indice SIM.
 *             Valida solo per la durata della callback.
 */
static void on_sms_received(const sms_message_t *msg)
{
    LOG_INF("SMS ricevuto da [%s]: [%s]", msg->sender, msg->text);

    /* --- Controllo autorizzazione --- */
    if (!is_authorized(msg->sender)) {
        LOG_WRN("Mittente non autorizzato: %s - SMS ignorato", msg->sender);
        return;
    }

    /* --- Copia e normalizzazione testo --- */
    char text[SMS_TEXT_MAX_LEN];
    strncpy(text, msg->text, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';

    /* Trim degli spazi iniziali (es. " START" -> "START") */
    char *t = text;
    while (*t == ' ' || *t == '\t') {
        t++;
    }

    /* Conversione maiuscolo per confronto case-insensitive */
    str_upper(t, strlen(t));

    /* --- Dispatch comandi --- */

    if (strcmp(t, "START") == 0) {
        /*
         * Avvio sequenza: controlla stato prima di procedere.
         * Se gia' in corso, informa il mittente e ignora.
         * Se IDLE, invia conferma e avvia il thread della sequenza.
         */
        if (sequence_get_state() == SEQ_RUNNING) {
            LOG_WRN("START ricevuto ma sequenza gia' in corso - ignorato");
            if (REPLY_ENABLED) {
                sms_send(msg->sender,
                         "Sequenza gia' in corso. Attendere il completamento.");
            }
            return;
        }
        LOG_INF("Comando START da %s - avvio sequenza", msg->sender);
        if (REPLY_ENABLED) {
            sms_send(msg->sender, "Sequenza avviata. Attendi SMS di esito.");
        }
        sequence_start(msg->sender);
        return;
    }

    if (strcmp(t, "TEST") == 0) {
        if (sequence_get_state() == SEQ_RUNNING) {
            LOG_WRN("TEST ricevuto ma sequenza gia' in corso - ignorato");
            if (REPLY_ENABLED) {
                sms_send(msg->sender, "Sequenza gia' in corso. Attendere.");
            }
            return;
        }    
        LOG_INF("Comando TEST da %s", msg->sender);
        if (REPLY_ENABLED) {
            sms_send(msg->sender,
                     "TEST avviato.\n"
                    "Ciclo EXP_OUT0-7 ogni 0.5s.\n"
                    "Si ferma quando EXP_IN_0 va HIGH.");
        }
        sequence_test_start(msg->sender);
        return;
}

    if (strcmp(t, "CONFIG") == 0) {
        handle_config(msg->sender);
        return;
    }

    if (strcmp(t, "STATUS") == 0) {
        handle_config(msg->sender);
        return;
    }

    if (strncmp(t, "SET ", 4) == 0) {
        handle_set(msg->sender, t);
        return;
    }

    /* Comando non riconosciuto */
    LOG_WRN("Comando non riconosciuto: [%s]", t);
    if (REPLY_ENABLED) {
        sms_send(msg->sender,
                "Comandi disponibili:\n"
                "  START\n"
                "  TEST\n"
                "  CONFIG\n"
                "  STATUS\n"
                "  SET T1/T2/T3 <sec>");
    }
}

/* ============================================================================
 * Entry point
 * ============================================================================ */

int main(void)
{
    LOG_INF("=== RAK5010-M + BG95-M3 | SMS->GPIO Controller ===");

 
    /* ------------------------------------------------------------------
     * 1. Inizializzazione GPIO
     *    Deve avvenire prima di qualsiasi altra operazione hardware.
     * ------------------------------------------------------------------ */
    int ret = gpio_ctrl_init();
    if (ret != 0) {
        LOG_ERR("gpio_ctrl_init fallito: %d", ret);
        return ret;
    }

    /* LED acceso durante l'inizializzazione come indicatore visivo */
    gpio_ctrl_led_set(true);

    /* ------------------------------------------------------------------
     * 2. Accensione BG95-M3
     *    Sequenza PWRKEY bloccante (~5.6 s).
     * ------------------------------------------------------------------ */
    gpio_ctrl_bg95_power_on();

    /* ------------------------------------------------------------------
     * 3. Inizializzazione UART del modem
     * ------------------------------------------------------------------ */
    ret = modem_init();
    if (ret != 0) {
        LOG_ERR("modem_init fallito: %d", ret);
        return ret;
    }

    /* ------------------------------------------------------------------
     * 4. Verifica comunicazione AT con il modem
     *    In caso di mancata risposta, tenta un reset hardware.
     * ------------------------------------------------------------------ */
    ret = modem_check();
    if (ret != 0) {
        LOG_ERR("Modem non risponde - tentativo reset hardware");
        gpio_ctrl_bg95_reset();
        k_msleep(3000);
        ret = modem_check();
        if (ret != 0) {
            LOG_ERR("Modem irraggiungibile dopo il reset - arresto");
            return ret;
        }
    }

    /* ------------------------------------------------------------------
     * 5. Configurazione rete LTE-M / NB-IoT (Swisscom APN)
     *    Non fatale: anche senza rete i SMS potrebbero funzionare
     *    se la SIM e' gia' registrata.
     * ------------------------------------------------------------------ */
    modem_configure_network();

    /* ------------------------------------------------------------------
     * 6. Inizializzazione sottosistema SMS
     *    Registra la callback on_sms_received.
     * ------------------------------------------------------------------ */
    ret = sms_init(on_sms_received);
    if (ret != 0) {
        LOG_ERR("sms_init fallito: %d", ret);
        return ret;
    }

    /* ------------------------------------------------------------------
     * 7. Caricamento parametri sequenza da NVS
     *    Usa default (5s/5s/5s) se nessun valore e' stato salvato.
     * ------------------------------------------------------------------ */
    sequence_init();

    /* ------------------------------------------------------------------
     * 8. Stato iniziale GPIO: tutti gli output a LOW
     * ------------------------------------------------------------------ */
    gpio_ctrl_exp_out_set(EXP_OUT_0, false);
    gpio_ctrl_exp_out_set(EXP_OUT_1, false);
    gpio_ctrl_exp_out_set(EXP_OUT_2, false);
    gpio_ctrl_exp_out_set(EXP_OUT_3, false);
    gpio_ctrl_exp_out_set(EXP_OUT_4, false);
    gpio_ctrl_exp_out_set(EXP_OUT_5, false);
    gpio_ctrl_exp_out_set(EXP_OUT_6, false);
    gpio_ctrl_exp_out_set(EXP_OUT_7, false);

    /* LED spento: sistema pronto */
    gpio_ctrl_led_set(false);
    LOG_INF("Sistema pronto. Comandi: START | CONFIG | SET T1/T2/T3 <sec>");

    /* ------------------------------------------------------------------
     * 8b. Inizializzazione sensore SHTC3
     * ------------------------------------------------------------------ */
    shtc3_dev = DEVICE_DT_GET_ANY(sensirion_shtcx);
    if (shtc3_dev == NULL) {
        LOG_ERR("SHTC3: device non trovato nel DT");
    } else if (!device_is_ready(shtc3_dev)) {
        LOG_ERR("SHTC3: device trovato ma non pronto (init fallita)");
        LOG_ERR("SHTC3: nome device = %s", shtc3_dev->name);
    } else {
        LOG_INF("SHTC3: pronto");
    }

    /* ------------------------------------------------------------------
     * 8c. Inizializzazione ADC per lettura VEXT
     * ------------------------------------------------------------------ */
    if (!adc_is_ready_dt(&adc_vext)) {
        LOG_WRN("ADC VEXT non pronto");
    } else {
        adc_channel_setup_dt(&adc_vext);
        LOG_INF("ADC pronto - VEXT su P0.05");
    }

    /* ------------------------------------------------------------------
     * 9. Loop principale
     *
     * Ogni ciclo da 5 secondi:
     *   - sms_poll(): interroga il modem per SMS non letti via
     *     AT+CMGL="REC UNREAD". Per ogni SMS trovato invoca
     *     on_sms_received() e cancella il messaggio dalla SIM.
     *   - Lampeggio LED 100 ms: heartbeat visivo che indica che
     *     il sistema e' operativo (utile per debug senza RTT).
     *
     * Il thread della sequenza (seq_tid) gira in parallelo quando
     * attivo, grazie allo scheduler preemptivo di Zephyr.
     * ------------------------------------------------------------------ */
    while (1) {
        
        float temp, hum;
        uint8_t bat_pct;
        uint16_t bat_mv;

        LOG_INF("---- Ciclo principale: polling SMS e lettura sensori ----");

        // Legge lo stato della batteria del modem (VBAT) e lo logga.
        if (modem_get_battery(&bat_pct, &bat_mv) == 0) {
            LOG_INF("VBAT: %u mV  %u %%", bat_mv, bat_pct);
        } else {
            LOG_WRN("VBAT: lettura fallita");
        }

        // Legge la tensione esterna (VEXT) tramite ADC e la logga.
        float vext = read_vext();
        LOG_INF("VEXT: %.2f V", (double)vext);

        // Legge temperatura e umidità dal sensore SHTC3 e li logga.
        read_sensor(&temp, &hum);
        LOG_INF("Temperatura: %.1f C  Umidita: %.1f %%", (double)temp, (double)hum);
        
        // Legge lo stato degli ingressi EXP_IN_0..7 e li logga.
        if (gpio_ctrl_mcp_is_ready()) {
            LOG_INF("EXP_IN: %d %d %d %d %d %d %d %d",
            gpio_ctrl_exp_in_get(EXP_IN_0),
            gpio_ctrl_exp_in_get(EXP_IN_1),
            gpio_ctrl_exp_in_get(EXP_IN_2),
            gpio_ctrl_exp_in_get(EXP_IN_3),
            gpio_ctrl_exp_in_get(EXP_IN_4),
            gpio_ctrl_exp_in_get(EXP_IN_5),
            gpio_ctrl_exp_in_get(EXP_IN_6),
            gpio_ctrl_exp_in_get(EXP_IN_7));
        }
        
        // Polling SMS: controlla se ci sono nuovi SMS non letti e invoca on_sms_received per ciascuno.
        sms_poll();

        /* Heartbeat LED: impulso breve ogni 5 secondi */
        gpio_ctrl_led_set(true);
        k_msleep(100);
        gpio_ctrl_led_set(false);
        k_msleep(4900);
    }

    return 0;
}
