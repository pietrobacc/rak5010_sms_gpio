#ifndef SMS_REPLIES_H
#define SMS_REPLIES_H

/*
 * sms_replies.h - Testi di tutti i messaggi SMS inviati dal firmware.
 *
 * Tutti i messaggi di risposta/notifica sono centralizzati qui, cosi'
 * da poterli modificare in un unico posto senza dover cercare in
 * main.c / sequence.c.
 *
 * I messaggi con parametri (%.1f, %u, %s, ...) sono format string da
 * usare con snprintf(), es.:
 *     char msg[64];
 *     snprintf(msg, sizeof(msg), REPLY_FMT_SET_S1_OK, (double)volt);
 */

/* ============================================================
 * Comandi cliente / help
 * ============================================================ */

    /* Lista dei comandi disponibili per i numeri cliente (non tecnici) */
 #define REPLY_CMD_LIST_CLIENTE \
    "Comandi disponibili:\n" \
    "START\n" \
    "START POMPA\n" \
    "STOP\n" \
    "STATUS"

    /* Lista dei comandi disponibili per i numeri tecnici */
#define REPLY_CMD_LIST_TECH \
    "START\n" \
    "START POMPA\n" \
    "STOP\n" \
    "CONFIG\n" \
    "STATUS\n" \
    "AUTOSTART ON/OFF\n" \
    "SET T1/T2/T3/T4 <sec>\n" \
    "SET T5/T6 <min>\n" \
    "SET S1 <V*10>"

    /* Messaggio 1 di risposta 1 per CONFIG */
#define REPLY_FMT_CONFIG_1 \
    "T1 = %u s\nT2 = %u s\nT3 = %u s\n" \
    "T4 = %u s\nT5 = %u min\nT6 = %u min\n" \
    "S1 = %.1f V\n" \
    "Autost: %s\n"

    /* Messaggio 2 di risposta 1 per CONFIG */
#define REPLY_FMT_CONFIG_2 \
    "NUM1:%s\nNUM2:%s\nNUM3:%s\nNOTIFY:NUM%u"

    /* Messaggio di risposta per STATUS */
#define REPLY_FMT_STATUS \
    "Temp: %.1f'C\n" \
    "Umidita: %.1f%%\n" \
    "VBATT: %.1fV\n" \
    "Segnale: %s\n" \
    "Gen: %s\n" \
    "Pom: %s"

/* ============================================================
 * SET T1-T6 / S1
 * ============================================================ */
#define REPLY_SET_FORMATO \
    "Formato:\n" \
    "  SET T1/T2/T3/T4 <sec>\n" \
    "  SET T5/T6 <min>\n" \
    "  SET S1 <volt*10>\n" \
    "  (es. SET S1 125 = 12.5V)"

#define REPLY_SET_T56_RANGE   "Valore non valido. T5/T6 max 1440 min (24h)."
#define REPLY_SET_S1_RANGE    "Valore non valido. SET S1 range: 100-140 (es. 125 = 12.5V)."
#define REPLY_SET_T14_RANGE   "Valore non valido. Range: 1-300 secondi."
#define REPLY_FMT_SET_S1_OK   "S1 impostato a %.1f V (salvato)"
#define REPLY_SET_S1_ERR      "Errore salvataggio S1."
#define REPLY_FMT_SET_SEC_OK  "%s impostato a %u s (salvato)"
#define REPLY_FMT_SET_MIN_OK  "%s impostato a %u min (salvato)"
#define REPLY_SET_PARAM_ERR   "Parametro non valido. Usa T1-T6 o S1."

/* ============================================================
 * SET NUM / NOTIFY
 * ============================================================ */
#define REPLY_FMT_NUM_SET     "NUM%u impostato: %s"
#define REPLY_FMT_NUM_CLEARED "NUM%u cancellato"
#define REPLY_FMT_NOTIFY_SET  "Notifiche -> NUM%u (%s)"
#define REPLY_NOTIFY_IDX_ERR  "Indice non valido (1-3)"

/* ============================================================
 * AUTOSTART
 * ============================================================ */
#define REPLY_AUTOSTART_ON   "Autostart: ON (salvato)"
#define REPLY_AUTOSTART_OFF  "Autostart: OFF (salvato)"

/* ============================================================
 * START (solo generatore)
 * ============================================================ */
#define REPLY_START_GIA_IN_CORSO   "Sequenza gia' in corso."

#define REPLY_START_GEN_MANUALE \
    "ATTENZIONE!! Generatore gia' acceso manualmente!\n" \
    "Spegnere fisicamente prima di usare START."

#define REPLY_START_AVVIO          "Avvio generatore...attendere."
#define REPLY_GEN_FALLITO          "Accensione FALLITA - nessun feedback dal generatore"
#define REPLY_GEN_STOP             "Generatore spento - STOP ricevuto"

#define REPLY_GEN_BLOCCATO_MANUALE \
    "Avvio bloccato: generatore gia' acceso.\n" \
    "Spegnere fisicamente prima di usare START."

#define REPLY_GEN_ACCESO           "Generatore acceso"
#define REPLY_GEN_TIMEOUT_T5       "Generatore spento - Timeout T5"

/* ============================================================
 * START POMPA
 *
 * IN1 = galleggiante di livello minimo sul serbatoio di alimento:
 *       HIGH quando il serbatoio e' VUOTO (la pompa non puo' aspirare
 *       a vuoto, quindi viene bloccata/spenta).
 * IN2 = galleggiante di troppo-pieno sul serbatoio di versamento:
 *       HIGH quando il serbatoio di destinazione e' pieno (pompa
 *       spenta per evitare fuoriuscite).
 * ============================================================ */
#define REPLY_POMPA_BLOCCATA_VUOTO         "Avvio pompa bloccato: serbatoio di pescaggio vuoto!"
#define REPLY_POMPA_BLOCCATA_TROPPOPIENO   "Avvio pompa bloccato: serbatoio di versamento pieno!"
#define REPLY_POMPA_GEN_GIA_ACCESO   "Generatore gia' acceso - avvio pompa..."
#define REPLY_POMPA_AVVIO_GEN_POMPA  "Avvio generatore + pompa..."
#define REPLY_POMPA_IN_AGGANCIO      "Pompa in aggancio..."
#define REPLY_POMPA_GIA_FUNZIONE     "Pompa gia' in funzione."
#define REPLY_POMPA_ATTENDI_GEN      "Attendere accensione generatore."

#define REPLY_POMPA_ACCESA           "Pompa accesa"

#define REPLY_POMPA_NON_AVVIATA_VUOTO \
    "Pompa non avviata!\n" \
    "Serbatoio di pescaggio vuoto."

#define REPLY_POMPA_NON_AVVIATA_TROPPOPIENO \
    "Pompa non avviata!\n" \
    "Serbatoio di versamento pieno."

#define REPLY_GENPOMPA_TIMEOUT_T6      "Generatore e pompa spenti - Timeout T6"
#define REPLY_GENPOMPA_VUOTO           "Generatore e pompa spenti - Rilevato serbatoio vuoto"
#define REPLY_GENPOMPA_TROPPOPIENO     "Generatore e pompa spenti - Rilevato troppo-pieno"
#define REPLY_GENPOMPA_STOP            "Generatore e pompa spenti - STOP ricevuto"
#define REPLY_GENPOMPA_DEFAULT         "Generatore e pompa spenti"

/* Spegnimento pompa già agganciata (dopo T6/IN1/IN2/STOP) */
#define REPLY_POMPA_STOP_TIMEOUT_T6      "Pompa spenta - Timeout T6"
#define REPLY_GENPOMPA_STOP_TIMEOUT_T6   "Generatore e pompa spenti - Timeout T6"
#define REPLY_POMPA_STOP_VUOTO           "Pompa spenta - serbatoio di pescaggio vuoto"
#define REPLY_GENPOMPA_STOP_VUOTO        "Generatore e pompa spenti - serbatoio di pescaggio vuoto"
#define REPLY_POMPA_STOP_TROPPOPIENO     "Pompa spenta - troppo-pieno"
#define REPLY_GENPOMPA_STOP_TROPPOPIENO  "Generatore e pompa spenti - troppo-pieno"
#define REPLY_POMPA_STOP_RICHIESTO       "Pompa spenta - STOP ricevuto"
#define REPLY_GENPOMPA_STOP_RICHIESTO    "Generatore e pompa spenti - STOP ricevuto"
#define REPLY_POMPA_SPENTA_DEFAULT       "Pompa spenta"

/* ============================================================
 * STOP diretto (nessuna sequenza attiva)
 * ============================================================ */
#define REPLY_STOP_NESSUNA_SEQUENZA  "STOP ricevuto (nessuna sequenza in corso)"

/* ============================================================
 * Boot / watchdog
 * ============================================================ */
#define REPLY_BOOT_WATCHDOG   "ATTENZIONE: Riavvio da watchdog!"
#define REPLY_BOOT_NORMALE    "INFO: Sistema avviato!"

/* ============================================================
 * Batteria bassa
 * ============================================================ */
#define REPLY_FMT_BATT_AUTOSTART \
    "ATTENZIONE: Batteria bassa (%.1fV) - avvio automatico generatore!"

#define REPLY_FMT_BATT_AVVISO \
    "ATTENZIONE: Batteria bassa (%.1fV)!\n" \
    "Autostart disabilitato.\n" \
    "Prossimo SMS a %.1fV."

/* ============================================================
 * IN3 - Serbatoio di versamento vuoto
 * Monitorato nel loop principale (non nelle sequenze pompa)
 * ============================================================ */
#define REPLY_IN3_VUOTO \
    "ATTENZIONE: Serbatoio di versamento vuoto!"

#define REPLY_IN3_AUTOSTART \
    "ATTENZIONE: Serbatoio di versamento vuoto - avvio automatico generatore+pompa!"

#endif /* SMS_REPLIES_H */