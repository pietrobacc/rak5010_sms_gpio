#ifndef VICTRON_H
#define VICTRON_H

#include <stdint.h>
#include <stdbool.h>

/*
 * victron.h - Lettura dati dal regolatore di carica Victron BlueSolar
 * 75/10 MPPT via protocollo VE.Direct (UART1, 19200 baud, testuale).
 *
 * Collegamento: VE.Direct TX/RX del BlueSolar -> P1.02/P1.01 (NRF_IO3/
 * NRF_IO4, header J12), tramite il level-shift TXS0102 gia' presente
 * sulla scheda RAK5010-M (VCCB = EXT_VREF).
 */

typedef struct {
    float   vbatt_v;         /**< Tensione batteria (V) - campo "V" */
    float   ibatt_a;         /**< Corrente batteria (A) - campo "I" */
    float   vpv_v;           /**< Tensione pannello (V) - campo "VPV" */
    float   ppv_w;           /**< Potenza pannello (W) - campo "PPV" */
    int     cs;              /**< Stato di carica - campo "CS" */
    int     err;              /**< Codice errore - campo "ERR" */
    bool    valid;            /**< true se almeno un frame e' stato ricevuto */
    int64_t last_update_ms;   /**< k_uptime_get() dell'ultimo frame valido */
} victron_data_t;

/**
 * @brief Inizializza UART1 e avvia la ricezione asincrona (interrupt-driven)
 *        del protocollo VE.Direct dal regolatore Victron.
 * @return 0 in caso di successo, negativo se la UART non e' pronta.
 */
int victron_init(void);

/**
 * @brief Copia l'ultimo frame VE.Direct ricevuto e correttamente
 *        interpretato. Thread-safe (protetto da mutex interno).
 *
 * @param out  Struttura di destinazione.
 * @return 0 se sono disponibili dati validi, -EAGAIN se non e' ancora
 *         stato ricevuto nessun frame completo (es. subito dopo il boot,
 *         o regolatore non ancora collegato/alimentato).
 */
int victron_get_data(victron_data_t *out);

/** @brief Rappresentazione testuale dello stato di carica, per log/SMS. */
const char *victron_cs_str(int cs);

#endif /* VICTRON_H */
