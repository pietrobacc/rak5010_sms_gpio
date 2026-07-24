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
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "gpio_ctrl.h"
#include "modem.h"
#include "sms.h"
#include "sequence.h"
#include "auth.h"
#include "sms_replies.h"
#include "plant_status.h"
#include "wdt.h"


LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static uint32_t g_reset_reason = 0;      // Variabile globale per memorizzare la causa del reset
static const struct device *shtc3_dev;   // Device SHTC3 per lettura temperatura e umidità

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

#define VEXT_DIVIDER_FACTOR     9.333f  /* (100k + 12k) / 12k */
#define VEXT_SAMPLES            5       /* campioni per la media mobile */

#define VEXT_LOW_CYCLES         3       /* cicli consecutivi sotto soglia prima di agire */
#define IN3_ACTIVE_CYCLES       3       /* cicli consecutivi IN3 HIGH prima di agire (debounce) */

static const struct adc_dt_spec adc_vext =
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/* ============================================================================
 * Funzioni di utilita' interne
 * ============================================================================ */

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

/* Lettura VEXT con media mobile su VEXT_SAMPLES campioni */
static float read_vext_avg(void)
{
    float   sum   = 0.0f;
    uint8_t valid = 0;

    for (uint8_t i = 0; i < VEXT_SAMPLES; i++) {
        float v = read_vext();
        if (v > 0.0f) {
            sum += v;
            valid++;
        }
        //LOG_INF("VEXT : %.2f V", (double)v);
        k_msleep(10);  /* pausa tra campioni per stabilizzare il partitore */
    }

    if (valid == 0) return -1.0f;

    /* Arrotondamento al decimo di volt */
    float avg = sum / valid;
    return roundf(avg * 10.0f) / 10.0f;
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
    
    snprintf(msg, sizeof(msg),
             REPLY_FMT_CONFIG_1,
             p->t1_ms / 1000,
             p->t2_ms / 1000,
             p->t3_ms / 1000,
             p->t4_ms / 1000,
             p->t5_min,
             p->t6_min,
             (double)p->s1_v,
             p->autostart ? "ON" : "OFF");
  
    LOG_INF("CONFIG richiesto: %s", msg);

    if (REPLY_ENABLED) {
        sms_send(sender, msg);
    }

    /* Aggiungi secondo SMS - Messaggio CONFIG*/
    char msg2[160];
    snprintf(msg2, sizeof(msg2),
            REPLY_FMT_CONFIG_2,
            strlen(auth_get_number(1)) ? auth_get_number(1) : "-",
            strlen(auth_get_number(2)) ? auth_get_number(2) : "-",
            strlen(auth_get_number(3)) ? auth_get_number(3) : "-",
            auth_get_notify_index());
    if (REPLY_ENABLED) {
        k_msleep(2000);
        sms_send(sender, msg2);
    }
}

static void handle_status(const char *sender)
{
    //const sequence_params_t *p = sequence_get_params();
    char msg[256];                      // msg per SMS di STATUS
    
    float temp, hum;                    // Legge i dati del sensore SHTC3
    read_sensor(&temp, &hum);

    uint8_t rssi;               // Variabile per livello segnale RSSI (non usato direttamente qui)
    int16_t dbm;                // Variabile per livello segnale in dBm

    char signal_str[20] = "N/A";
    if (modem_get_signal(&rssi, &dbm) == 0) {
        snprintf(signal_str, sizeof(signal_str), "%d dBm", dbm);
    }

    uint32_t gen_uptime = 0;            // Variabile per uptime generatore
    uint32_t pom_uptime = 0;            // Variabile per uptime pompa
    bool gen_on = sequence_generatore_is_on(&gen_uptime);
    bool pom_on = sequence_pompa_is_on(&pom_uptime);
    char gen_str[20] = "OFF";
    char pom_str[20] = "OFF";

    if (gen_on) {
    snprintf(gen_str, sizeof(gen_str), "ON (%uh%02um)",
             gen_uptime / 3600,
             (gen_uptime % 3600) / 60);
    } else if (gpio_ctrl_exp_in_get(EXP_IN_0) == 1) {
        snprintf(gen_str, sizeof(gen_str), "ON (Manuale!!)");
    }

    if (pom_on) {
    snprintf(pom_str, sizeof(pom_str), "ON (%uh%02um)",
             pom_uptime / 3600,
             (pom_uptime % 3600) / 60);
    }

    float vext = read_vext_avg();           // Legge la tensione esterna tramite ADC

    snprintf(msg, sizeof(msg),
            REPLY_FMT_STATUS,
            (double)temp,
            (double)hum,
            (double)vext,
            signal_str,
            gen_str,
            pom_str);
  
    LOG_INF("STATUS richiesto: %s", msg);

    if (REPLY_ENABLED) {
        sms_send(sender, msg);
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
            sms_send(sender, REPLY_SET_FORMATO);
        }
        return;
    }

    /* Validazione range per tipo di parametro */
    if (strcmp(key, "T5") == 0 || strcmp(key, "T6") == 0) {
        if (val > 1440) {
            LOG_WRN("SET: valore fuori range per %s: %u (max 1440 min)", key, val);
            if (REPLY_ENABLED) {
                sms_send(sender, REPLY_SET_T56_RANGE);
            }
            return;
        }
    } else if (strcmp(key, "S1") == 0) {
        /* S1: valore in decimi di volt, range 100-140 (10.0V-14.0V) */
        if (val < 100 || val > 140) {
            LOG_WRN("SET: valore S1 fuori range: %u (valido 100-140)", val);
            if (REPLY_ENABLED) {
                sms_send(sender, REPLY_SET_S1_RANGE);
            }
            return;
        }
    } else {
        /* T1/T2/T3/T4: valore in secondi, range 1-300 */
        if (val == 0 || val > 300) {
            LOG_WRN("SET: valore fuori range per %s: %u (valido 1-300 s)", key, val);
            if (REPLY_ENABLED) {
                sms_send(sender, REPLY_SET_T14_RANGE);
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
            snprintf(msg, sizeof(msg), REPLY_FMT_SET_S1_OK, (double)volt);
            LOG_INF("%s", msg);
            if (REPLY_ENABLED) sms_send(sender, msg);
        } else {
            LOG_ERR("SET S1 fallito: %d", ret);
            if (REPLY_ENABLED) sms_send(sender, REPLY_SET_S1_ERR);
        }
        return;
    }

    /* Gestione T1-T6 */
    int ret = sequence_set_param(key, val);
    if (ret == 0) {
        char msg[64];
        if (strcmp(key, "T5") == 0 || strcmp(key, "T6") == 0) {
            snprintf(msg, sizeof(msg), REPLY_FMT_SET_MIN_OK, key, val);
        } else {
            snprintf(msg, sizeof(msg), REPLY_FMT_SET_SEC_OK, key, val);
        }
        LOG_INF("%s", msg);
        if (REPLY_ENABLED) sms_send(sender, msg);
    } else {
        LOG_ERR("SET fallito per '%s': %d", key, ret);
        if (REPLY_ENABLED) sms_send(sender, REPLY_SET_PARAM_ERR);
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
    if (!auth_is_authorized(msg->sender)) {
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

    /* Comandi riservati ai tecnici */
    if (!auth_is_tech(msg->sender)) {
        /* Numeri cliente: solo STATUS, START, START POMPA, STOP */
        if (strcmp(t, "STATUS")      != 0 &&
            strcmp(t, "START")       != 0 &&
            strcmp(t, "START POMPA") != 0 &&
            strcmp(t, "STOP")        != 0) {
            LOG_WRN("Comando non autorizzato per numero cliente: %s", t);
            if (REPLY_ENABLED) {
                sms_send(msg->sender, REPLY_CMD_LIST_CLIENTE);
            }
            return;
        }
    }
    
    /*******************************************************************************************************/
    /* --- Dispatch comandi --- */
    /*******************************************************************************************************/

    /*******************************************************************************************************/
    /* STOP              */

    if (strcmp(t, "STOP") == 0) {
        sequence_stop(msg->sender);
        return;
    }

    /*******************************************************************************************************/
    /* START POMPA                      */

    if (strcmp(t, "START POMPA") == 0) {
        sequence_state_t st = sequence_get_state();

        if (st == SEQ_IDLE) {
            if (gpio_ctrl_exp_in_get(EXP_IN_1) == 1) {
                /* Serbatoio già vuoto - pompa non può aspirare a vuoto */
                if (REPLY_ENABLED) sms_send(msg->sender, REPLY_POMPA_BLOCCATA_VUOTO);

            } else if (gpio_ctrl_exp_in_get(EXP_IN_2) == 1) {
                /* Serbatoio di versamento già pieno - pompa non va avviata */
                if (REPLY_ENABLED) sms_send(msg->sender, REPLY_POMPA_BLOCCATA_TROPPOPIENO);

            } else if (gpio_ctrl_exp_in_get(EXP_IN_0) == 1 &&
                    !sequence_generatore_is_on(NULL)) {
                /* Generatore acceso manualmente - avvia solo pompa */
                if (REPLY_ENABLED) sms_send(msg->sender, REPLY_POMPA_GEN_GIA_ACCESO);
                sequence_start_pompa(msg->sender);

            } else {
                /* Accensione normale generatore + pompa */
                if (REPLY_ENABLED) sms_send(msg->sender, REPLY_POMPA_AVVIO_GEN_POMPA);
                sequence_start_pompa(msg->sender);
            }

        } else if (st == SEQ_GEN_OK) {
            /* Generatore già acceso -> aggancia pompa */
            int ret = sequence_attach_pompa(msg->sender);
            if (ret == 0) {
                if (REPLY_ENABLED) sms_send(msg->sender, REPLY_POMPA_IN_AGGANCIO);
            }
        } else if (st == SEQ_POMPA_ON) {
            /* Pompa già in funzione */
            if (REPLY_ENABLED) sms_send(msg->sender, REPLY_POMPA_GIA_FUNZIONE);
        } else {
            /* Accensione generatore in corso */
            if (REPLY_ENABLED) sms_send(msg->sender, REPLY_POMPA_ATTENDI_GEN);
        }
        return;
    }

    /*******************************************************************************************************/
    /* START                                                                                               */

    if (strcmp(t, "START") == 0) {
        sequence_state_t st = sequence_get_state();

        if (st != SEQ_IDLE) {
            /* Altra sequenza già in corso */
            if (REPLY_ENABLED) sms_send(msg->sender, REPLY_START_GIA_IN_CORSO);
        } else if (gpio_ctrl_exp_in_get(EXP_IN_0) == 1) {
            /* Generatore già acceso manualmente - blocca avvio sequenza */
            if (REPLY_ENABLED) sms_send(msg->sender, REPLY_START_GEN_MANUALE);
        } else {
            if (REPLY_ENABLED) sms_send(msg->sender, REPLY_START_AVVIO);
            sequence_start(msg->sender);
        }
        return;
    }

    /*******************************************************************************************************/
    /* CONFIG                                                                                              */

    if (strcmp(t, "CONFIG") == 0) {
        handle_config(msg->sender);
        return;
    }

    /*******************************************************************************************************/
    /* STATUS                                                                                              */

    if (strcmp(t, "STATUS") == 0) {
        handle_status(msg->sender);
        return;
    }

    /*******************************************************************************************************/
    /* SET NUM1/2/3                                                                                        */

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
                    snprintf(reply, sizeof(reply), REPLY_FMT_NUM_SET, idx, num);
                } else {
                    snprintf(reply, sizeof(reply), REPLY_FMT_NUM_CLEARED, idx);
                }
                if (REPLY_ENABLED) sms_send(msg->sender, reply);
            }
        }
        return;
    }

    /*******************************************************************************************************/
    /* SET NOTIFY */

    if (strncmp(t, "SET NOTIFY ", 11) == 0) {
        uint8_t idx = t[11] - '0';
        int ret = auth_set_notify(idx);
        if (ret == 0) {
            char reply[64];
            snprintf(reply, sizeof(reply), REPLY_FMT_NOTIFY_SET, idx, auth_get_notify_number());
            if (REPLY_ENABLED) sms_send(msg->sender, reply);
        } else {
            if (REPLY_ENABLED) sms_send(msg->sender, REPLY_NOTIFY_IDX_ERR);
        }
        return;
    }

    /*******************************************************************************************************/
    /* SET               */
  
    if (strncmp(t, "SET ", 4) == 0) {
        handle_set(msg->sender, t);
        return;
    }
    /*******************************************************************************************************/
    /* AUTOSTART ON/OFF                                      */

    if (strcmp(t, "AUTOSTART ON") == 0) {
       sequence_set_autostart(true);
        if (REPLY_ENABLED) sms_send(msg->sender, REPLY_AUTOSTART_ON);
        return;
    }
    if (strcmp(t, "AUTOSTART OFF") == 0) {
        sequence_set_autostart(false);
        if (REPLY_ENABLED) sms_send(msg->sender, REPLY_AUTOSTART_OFF);
        return;
    }

    /*******************************************************************************************************/
    /* Comando non riconosciuto                      */

    LOG_WRN("Comando non riconosciuto: [%s]", t);
    if (REPLY_ENABLED) {
        sms_send(msg->sender, REPLY_CMD_LIST_TECH);
    }
}

static void led_blink(uint8_t count)
{
    /* Ogni blink = 150ms ON + 150ms OFF = 300ms */
    for (uint8_t i = 0; i < count; i++) {
        gpio_ctrl_led_set(false);
        k_msleep(150);
        gpio_ctrl_led_set(true);
        k_msleep(150);
    }

    /* Pausa finale per arrivare a ~5 secondi totali */
    uint32_t blink_time = count * 300U;
    if (blink_time < 5000U) {
        k_msleep(5000U - blink_time);
    }
}

/* ============================================================================
 * Entry point
 * ============================================================================ */

int main(void)
{
    int ret;

    /* Watchdog hardware - prima di tutto */
    ret = wdt_init();
    if (ret != 0) {
        LOG_ERR("WDT init fallito: %d", ret);
        /* non fatale - continua comunque */
    }

    /* Leggi causa del reset */
    g_reset_reason = nrf_power_resetreas_get(NRF_POWER);
    nrf_power_resetreas_clear(NRF_POWER, g_reset_reason);

    if (g_reset_reason & NRF_POWER_RESETREAS_DOG_MASK) {
        LOG_WRN("Riavvio da WATCHDOG!");
    } else if (g_reset_reason & NRF_POWER_RESETREAS_RESETPIN_MASK) {
        LOG_INF("Riavvio da reset pin");
    } else if (g_reset_reason & NRF_POWER_RESETREAS_OFF_MASK) {
     LOG_INF("Avvio da power-on");
    } else {
        LOG_INF("Riavvio (causa: 0x%08X)", g_reset_reason);
    }
   
    LOG_INF("=== RAK5010-M + BG95-M3 | SMS->GPIO Controller ===");

 
    /* ------------------------------------------------------------------
     * 1. Inizializzazione GPIO
     *    Deve avvenire prima di qualsiasi altra operazione hardware.
     * ------------------------------------------------------------------ */
    ret = gpio_ctrl_init();
    if (ret != 0) {
        LOG_ERR("gpio_ctrl_init fallito: %d", ret);
        return ret;
    }

    /* LED acceso durante l'inizializzazione come indicatore visivo */
    gpio_ctrl_led_set(false);

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
     * 7b. Notifica avvio o riavvio via SMS
     *    Se il numero di notifica è configurato, invia un SMS all'avvio.
     *    Se il reset è stato causato dal watchdog, invia un SMS di allerta.
     * ------------------------------------------------------------------ */
    const char *notify = auth_get_notify_number();
    if (strlen(notify) > 0) {
      if (g_reset_reason & NRF_POWER_RESETREAS_DOG_MASK) {
          sms_send(notify, REPLY_BOOT_WATCHDOG);
      } else {
          sms_send(notify, REPLY_BOOT_NORMALE);
      }
    }

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
    gpio_ctrl_led_set(true);
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
    
    float next_sms = sequence_get_params()->s1_v;
    bool sms_sended = false;
    float delta_vext = 0.2f;
    char msg[256];
    
    while (1) {

        LOG_INF("---- Ciclo principale: polling SMS e lettura sensori ----");

        // Polling SMS: controlla se ci sono nuovi SMS non letti e invoca on_sms_received per ciascuno.
        sms_poll();

        // Legge la tensione esterna (VEXT) tramite ADC.
        /* Lettura VEXT con media mobile */
        float vext = read_vext_avg();
   
        /* ----------------------------------------------------------------
         * Controllo VEXT per avvio automatico o avviso batteria bassa.
         *
         * vext_low_count: incrementato ad ogni ciclo in cui VEXT < S1,
         * azzerato non appena VEXT torna sopra soglia.
         * L'azione (autostart o SMS) viene eseguita solo quando il
         * contatore raggiunge VEXT_LOW_CYCLES.
         * ---------------------------------------------------------------- */
        static uint8_t vext_low_count = 0;

        if (vext > 0.0f) {
            if (vext < sequence_get_params()->s1_v) {

                vext_low_count++;
                LOG_WRN("VEXT bassa (%.1f V) - ciclo %u/%u",
                        (double)vext, vext_low_count, VEXT_LOW_CYCLES);

                if (vext_low_count >= VEXT_LOW_CYCLES) {

                    if (sequence_get_state() == SEQ_IDLE &&
                        sequence_get_params()->autostart) {
                        /* VEXT bassa confermata + autostart abilitato
                         * → avvio automatico generatore */
                        snprintf(msg, sizeof(msg), REPLY_FMT_BATT_AUTOSTART, (double)vext);
                        LOG_WRN("%s", msg);
                        sms_send(auth_get_notify_number(), msg);
                        sequence_start(auth_get_notify_number());

                        /* Reset contatore - evita avvii multipli */
                        vext_low_count = 0;

                    } else if (sequence_get_state() == SEQ_IDLE &&
                               !sequence_get_params()->autostart) {
                        /* VEXT bassa confermata + autostart disabilitato
                         * → SMS di avviso (con soglia progressiva) */
                        if (!sms_sended || vext <= next_sms) {
                            sms_sended = true;
                            next_sms   = vext - delta_vext;
                            snprintf(msg, sizeof(msg), REPLY_FMT_BATT_AVVISO, (double)vext, (double)next_sms);
                            LOG_WRN("%s", msg);
                            sms_send(auth_get_notify_number(), msg);
                        } else {
                            LOG_WRN("Batteria bassa (%.1fV) - "
                                    "SMS non inviato, prossimo a %.1fV",
                                    (double)vext, (double)next_sms);
                        }
                    }
                    /* Se SEQ non IDLE (generatore gia' in corso)
                     * non fare nulla - il generatore sta gia' caricando */
                }

            } else {
                /* VEXT sopra soglia - reset contatori e flag */
                if (vext_low_count > 0) {
                    LOG_INF("VEXT tornata sopra soglia (%.1fV) - reset contatore",
                            (double)vext);
                }
                vext_low_count = 0;
                sms_sended     = false;
                next_sms       = sequence_get_params()->s1_v;
            }
        }

        // Legge lo stato degli ingressi EXP_IN_0..7 e li logga.
        if (gpio_ctrl_mcp_is_ready()) {

            /* ----------------------------------------------------------------
             * Controllo IN3: segnala serbatoio di versamento vuoto.
             *
             * in3_active_count: debounce, richiede IN3_ACTIVE_CYCLES cicli
             * consecutivi HIGH prima di agire (evita falsi trigger da
             * rimbalzi del galleggiante). in3_notified evita di ripetere
             * l'azione ad ogni ciclo finche' resta HIGH.
             * ---------------------------------------------------------------- */
            static uint8_t in3_active_count = 0;
            static bool    in3_notified     = false;

            if (gpio_ctrl_exp_in_get(EXP_IN_3) == 1) {
                in3_active_count++;
                if (in3_active_count >= IN3_ACTIVE_CYCLES && !in3_notified) {
                    in3_notified = true;

                    if (sequence_get_state() == SEQ_IDLE &&
                        sequence_get_params()->autostart) {
                        /* Serbatoio versamento vuoto confermato + autostart
                         * abilitato → avvio automatico generatore + pompa */
                        LOG_WRN("%s", REPLY_IN3_AUTOSTART);
                        sms_send(auth_get_notify_number(), REPLY_IN3_AUTOSTART);
                        sequence_start_pompa(auth_get_notify_number());
                    } else {
                        /* Solo notifica: autostart disabilitato, oppure
                         * una sequenza e' gia' in corso */
                        LOG_WRN("%s", REPLY_IN3_VUOTO);
                        sms_send(auth_get_notify_number(), REPLY_IN3_VUOTO);
                    }
                }
            } else {
                if (in3_active_count > 0 || in3_notified) {
                    LOG_INF("IN3 tornato normale - reset");
                }
                in3_active_count = 0;
                in3_notified     = false;
            }

            int in0 = gpio_ctrl_exp_in_get(EXP_IN_0);
            int in1 = gpio_ctrl_exp_in_get(EXP_IN_1);
            int in2 = gpio_ctrl_exp_in_get(EXP_IN_2);
            int in3 = gpio_ctrl_exp_in_get(EXP_IN_3);
            int in4 = gpio_ctrl_exp_in_get(EXP_IN_4);
            int in5 = gpio_ctrl_exp_in_get(EXP_IN_5);
            int in6 = gpio_ctrl_exp_in_get(EXP_IN_6);
            int in7 = gpio_ctrl_exp_in_get(EXP_IN_7);

            int out0 = gpio_ctrl_exp_out_get(EXP_OUT_0);
            int out1 = gpio_ctrl_exp_out_get(EXP_OUT_1);
            int out2 = gpio_ctrl_exp_out_get(EXP_OUT_2);
            int out3 = gpio_ctrl_exp_out_get(EXP_OUT_3);
            int out4 = gpio_ctrl_exp_out_get(EXP_OUT_4);
            int out5 = gpio_ctrl_exp_out_get(EXP_OUT_5);
            int out6 = gpio_ctrl_exp_out_get(EXP_OUT_6);
            int out7 = gpio_ctrl_exp_out_get(EXP_OUT_7);

            char plant_desc[100];
            plant_level_t plant_level = plant_status_evaluate(plant_desc, sizeof(plant_desc));

            char line[220];
            snprintf(line, sizeof(line),
                     "Seq=%-8s VEXT=%.1fV IN=%d%d%d%d%d%d%d%d OUT=%d%d%d%d%d%d%d%d | %s: %s",
                     sequence_state_name(sequence_get_state()), (double)vext,
                     in0, in1, in2, in3, in4, in5, in6, in7,
                     out0, out1, out2, out3, out4, out5, out6, out7,
                     plant_level_str(plant_level), plant_desc);

            switch (plant_level) {
            case PLANT_OK:         LOG_INF("%s", line); break;
            case PLANT_ATTENZIONE: LOG_WRN("%s", line); break;
            default:                LOG_ERR("%s", line); break;
            }
        }
        
        /* Blink LED in base allo stato */
        sequence_state_t st = sequence_get_state();
        
        if (st == SEQ_IDLE && gpio_ctrl_exp_out_get(EXP_OUT_0) == 0 && gpio_ctrl_exp_in_get(EXP_IN_0) == 1) {
            /* Generatore acceso manualmente - 6 lampeggi */
            led_blink(6);
        } else if (st == SEQ_POMPA_ON) {
            /* Pompa accesa - 5 lampeggi */
            led_blink(5);
        } else if (st == SEQ_GEN_OK) {
            /* Generatore acceso - 4 lampeggi */
            led_blink(4);
        } else if (st == SEQ_RUNNING) {
            /* Ciclo in esecuzione - 3 lampeggi */
            led_blink(3);
        } else if (vext > 0 && vext < sequence_get_params()->s1_v) {
            /* Batteria sotto soglia - 2 lampeggi */
            led_blink(2);
        } else {
            /* Stato normale - 1 lampeggio */
            led_blink(1);
        }

        /* Alimenta il watchdog - prova che il loop è vivo */
        app_wdt_feed();
    }

    return 0;
}
