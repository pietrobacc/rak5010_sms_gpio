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
#include "auth.h"


LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *shtc3_dev;

/* ============================================================================
 * Configurazione utente
 * Modificare questi valori prima della compilazione.
 * ============================================================================ */

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
 * @brief Verifica se il mittente e' autorizzato.
 *
 * Confronta il numero del mittente con i numeri autorizzati configurati
 * in NVS. Se il numero è presente tra quelli autorizzati, restituisce true.
 * Altrimenti, restituisce false.
 *
 * @param sender  Numero mittente da verificare.
 * @return true se il mittente è autorizzato, false altrimenti.
 */
static bool is_authorized(const char *sender)
{
    return auth_is_authorized(sender);
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
    
    //float temp, hum;
    //read_sensor(&temp, &hum);

    //uint8_t bat_pct;
    //uint16_t bat_mv;
    //modem_get_battery(&bat_pct, &bat_mv);

    float vext = read_vext();

    snprintf(msg, sizeof(msg),
             "T1 = %u s\nT2 = %u s\nT3 = %u s\n"
             "T4 = %u min\nT5 = %u s\nT6 = %u min\n"
             "S1 = %.1f V\n"
             "Autost: %s\n"
             "VEXT = %.2f V",
             p->t1_ms / 1000,
             p->t2_ms / 1000,
             p->t3_ms / 1000,
             p->t4_min,
             p->t5_ms / 1000,
             p->t6_min,
             (double)p->s1_v,
             p->autostart ? "ON" : "OFF",
             (double)vext);

  
    LOG_INF("CONFIG richiesto: %s", msg);

    if (REPLY_ENABLED) {
        sms_send(sender, msg);
    }

    /* Aggiungi dopo VEXT nel messaggio STATUS - secondo SMS */
    char msg2[160];
    snprintf(msg2, sizeof(msg2),
            "NUM1:%s\nNUM2:%s\nNUM3:%s\nNOTIFY:NUM%u",
            strlen(auth_get_number(1)) ? auth_get_number(1) : "-",
            strlen(auth_get_number(2)) ? auth_get_number(2) : "-",
            strlen(auth_get_number(3)) ? auth_get_number(3) : "-",
            auth_get_notify_index());
    if (REPLY_ENABLED) {
        k_msleep(2000);
        sms_send(sender, msg2);
    }
}

/**
 * @brief Gestisce il comando SET T1/T2/T3/T4/T5/T6/S1 <valore>.
 *
 * Formato atteso (dopo conversione maiuscolo):
 *   "SET T1 10"  -> T1 = 10 secondi
 *   "SET T4 30"  -> T4 = 30 minuti
 *   "SET S1 125" -> S1 = 12.5 Volt
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
     *   %3s  - legge max 3 caratteri (es. "T1", "T2", "S1")
     *   %u   - legge un intero senza segno
     * Restituisce 2 se entrambi i campi sono stati letti correttamente.
     */
    if (sscanf(text, "SET %3s %u", key, &val) != 2) {
        LOG_WRN("SET: formato non valido: '%s'", text);
        if (REPLY_ENABLED) {
            sms_send(sender,
                     "Formato:\n"
                     "  SET T1/T2/T3/T5 <sec>\n"
                     "  SET T4/T6 <min>\n"
                     "  SET S1 <volt*10>\n"
                     "  (es. SET S1 125 = 12.5V)");
        }
        return;
    }

    /* Validazione range per tipo di parametro */
    if (strcmp(key, "T4") == 0 || strcmp(key, "T6") == 0) {
        if (val > 1440) {
            LOG_WRN("SET: valore fuori range per %s: %u (max 1440 min)", key, val);
            if (REPLY_ENABLED) {
                sms_send(sender, "Valore non valido. T4/T6 max 1440 min (24h).");
            }
            return;
        }
    } else if (strcmp(key, "S1") == 0) {
        /* S1: valore in decimi di volt, range 100-140 (10.0V-14.0V) */
        if (val < 100 || val > 140) {
            LOG_WRN("SET: valore S1 fuori range: %u (valido 100-140)", val);
            if (REPLY_ENABLED) {
                sms_send(sender, "Valore non valido. SET S1 range: 100-140 (es. 125 = 12.5V).");
            }
            return;
        }
    } else {
        /* T1/T2/T3/T5: valore in secondi, range 1-300 */
        if (val == 0 || val > 300) {
            LOG_WRN("SET: valore fuori range per %s: %u (valido 1-300 s)", key, val);
            if (REPLY_ENABLED) {
                sms_send(sender, "Valore non valido. Range: 1-300 secondi.");
            }
            return;
        }
    }

    /* Gestione S1 separata (float) */
    if (strcmp(key, "S1") == 0) {
        float volt = val / 10.0f;
        int ret = sequence_set_s1(volt);
        if (ret == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "S1 impostato a %.1f V (salvato)", (double)volt);
            LOG_INF("%s", msg);
            if (REPLY_ENABLED) sms_send(sender, msg);
        } else {
            LOG_ERR("SET S1 fallito: %d", ret);
            if (REPLY_ENABLED) sms_send(sender, "Errore salvataggio S1.");
        }
        return;
    }

    /* Gestione T1-T6 */
    int ret = sequence_set_param(key, val);
    if (ret == 0) {
        char msg[64];
        if (strcmp(key, "T4") == 0 || strcmp(key, "T6") == 0) {
            snprintf(msg, sizeof(msg), "%s impostato a %u min (salvato)", key, val);
        } else {
            snprintf(msg, sizeof(msg), "%s impostato a %u s (salvato)", key, val);
        }
        LOG_INF("%s", msg);
        if (REPLY_ENABLED) sms_send(sender, msg);
    } else {
        LOG_ERR("SET fallito per '%s': %d", key, ret);
        if (REPLY_ENABLED) sms_send(sender, "Parametro non valido. Usa T1-T6 o S1.");
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

    /* STOP */
    if (strcmp(t, "STOP") == 0) {
        sequence_stop(msg->sender);
        return;
    }

    /* START POMPA */
    if (strcmp(t, "START POMPA") == 0) {
        if (sequence_get_state() == SEQ_RUNNING) {
            if (REPLY_ENABLED) sms_send(msg->sender, "Sequenza gia' in corso.");
            return;
        }
        if (REPLY_ENABLED) sms_send(msg->sender, "Avvio generatore + pompa...");
        sequence_start_pompa(msg->sender);
        return;
    }

    /* START */
    if (strcmp(t, "START") == 0) {
        if (sequence_get_state() == SEQ_RUNNING) {
            if (REPLY_ENABLED) sms_send(msg->sender, "Sequenza gia' in corso.");
            return;
        }
        if (REPLY_ENABLED) sms_send(msg->sender, "Avvio generatore...");
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

    /* SET NUM1/2/3 */
    if (strncmp(t, "SET NUM", 7) == 0) {
        uint8_t idx = t[7] - '0';
        if (idx >= 1 && idx <= 3) {
            /* Trova il numero dopo "SET NUMx " */
            const char *num = msg->text + 9;  /* usa testo originale non uppercase */
            while (*num == ' ') num++;
            int ret = auth_set_number(idx, num);
            if (ret == 0) {
                char reply[64];
                if (strlen(num) > 0) {
                    snprintf(reply, sizeof(reply), "NUM%u impostato: %s", idx, num);
                } else {
                    snprintf(reply, sizeof(reply), "NUM%u cancellato", idx);
                }
                if (REPLY_ENABLED) sms_send(msg->sender, reply);
            }
        }
        return;
    }

    /* SET NOTIFY */
    if (strncmp(t, "SET NOTIFY ", 11) == 0) {
        uint8_t idx = t[11] - '0';
        int ret = auth_set_notify(idx);
        if (ret == 0) {
            char reply[64];
            snprintf(reply, sizeof(reply),
                    "Notifiche -> NUM%u (%s)",
                    idx, auth_get_notify_number());
            if (REPLY_ENABLED) sms_send(msg->sender, reply);
        } else {
            if (REPLY_ENABLED) sms_send(msg->sender, "Indice non valido (1-3)");
        }
        return;
    }

    if (strncmp(t, "SET ", 4) == 0) {
        handle_set(msg->sender, t);
        return;
    }

    if (strcmp(t, "AUTOSTART ON") == 0) {
       sequence_set_autostart(true);
        if (REPLY_ENABLED) sms_send(msg->sender, "Autostart: ON (salvato)");
        return;
    }

    if (strcmp(t, "AUTOSTART OFF") == 0) {
        sequence_set_autostart(false);
        if (REPLY_ENABLED) sms_send(msg->sender, "Autostart: OFF (salvato)");
        return;
    }

    /* Comando non riconosciuto */
    LOG_WRN("Comando non riconosciuto: [%s]", t);
    if (REPLY_ENABLED) {
        sms_send(msg->sender,
         "START\n"
         "START POMPA\n"
         "STOP\n"
         "CONFIG\n"
         "AUTOSTART ON/OFF\n"
         "SET T1/T2/T3/T5 <sec>\n"
         "SET T4/T6 <min>\n"
         "SET S1 <V*10>");
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
     * 7a. Caricamento parametri numeri autorizzati da NVS
     * ------------------------------------------------------------------ */
    auth_init();

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
    
     float vext_sms = 0.0f; 
     bool sms_sended = false;

     while (1) {
        
        float temp, hum;
        uint8_t bat_pct;
        uint16_t bat_mv;
        float delta_vext = 0.2f;

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

        /* Controllo VEXT per avvio automatico */
        if (vext > 0.0f) 
        {
            if(vext < sequence_get_params()->s1_v)
            {
                if (sequence_get_state() == SEQ_IDLE && sequence_get_params()->autostart) {
                    LOG_WRN("VEXT bassa (%.2f V) - AUTOSTART ON - avvio automatico generatore",
                            (double)vext);
                    sms_send(auth_get_notify_number(),
                            "ATTENZIONE: VEXT bassa - avvio automatico generatore!");
                    sequence_start(auth_get_notify_number());
                }
                else {
                    LOG_WRN("VEXT bassa (%.2f V) - AUTOSTART OFF o Sequenza già in corso - avvio automatico disabilitato - NON INVIATO A: %s",
                            (double)vext, auth_get_notify_number());
                    if (sms_sended == false || vext < vext_sms - delta_vext) {
                        sms_send(auth_get_notify_number(),
                                "ATTENZIONE: VEXT bassa - avvio automatico disabilitato!");
                        LOG_WRN("VEXT bassa (%.2f V - sms %.2f V) - avvio automatico disabilitato! SMS INVIATO A: %s", (double)vext, (double)vext_sms, auth_get_notify_number());
                        sms_sended = true;
                        vext_sms = vext;
                    }                       
                }
            }
            else {
                sms_sended = false;
                vext_sms = vext;
            }
        }

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
