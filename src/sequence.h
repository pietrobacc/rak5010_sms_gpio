/*
 * sequence.h - Macchina a stati per la sequenza di avvio
  ******************************************************************************
 * Flusso della sequenza START (generatore):
 *
 *   [SMS "START" | VEXT < S1]
 *          |
 *          v
 *   EXP_OUT0 = ON
 *          |
 *       attesa T1
 *          |
 *          v
 *   EXP_OUT1 = ON
 *          |
 *       attesa T2
 *          |
 *          v
 *   EXP_OUT1 = OFF
 *          |
 *     polling EXP_IN_0 (ogni 50ms, max T3)
 *          |
 *          +---> FALLITA --> SMS + EXP_OUT0 OFF
 *          |
 *          +---> OK --> SMS "Accensione OK"
 *                  |
 *               attesa T4 (minuti)  [o SMS "STOP"]
 *                  |
 *               EXP_OUT0 OFF
 *                  |
 *               SMS "Spegnimento generatore"
 *
 ******************************************************************************
 * Flusso della sequenza START POMPA (generatore + pompa):
 *
 *   [SMS "START POMPA"]
 *          |
 *          v
 *   (stessa accensione generatore di START)
 *          |
 *          v
 *   OK --> attesa T5 (secondi)
 *          |
 *          v
 *   EXP_OUT2 = ON
 *          |
 *     attesa T6 (minuti) | EXP_IN_1 HIGH | SMS "STOP"
 *          |
 *          v
 *   EXP_OUT2 OFF + EXP_OUT0 OFF
 *          |
 *   SMS "Spegnimento generatore e pompa"
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/


#ifndef SEQUENCE_H
#define SEQUENCE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Struttura dei parametri temporali della sequenza.
 *
 * I valori vengono salvati in NVS e persistono ai riavvii. Se non
 * presenti, vengono usati i default specificati nell'inizializzazione.
 */
typedef struct {
    uint32_t t1_ms;   /**< Pausa EXP_OUT0 ON -> EXP_OUT1 ON (secondi) */
    uint32_t t2_ms;   /**< Pausa EXP_OUT1 ON -> EXP_OUT1 OFF (secondi) */
    uint32_t t3_ms;   /**< Timeout polling EXP_IN_0 (secondi) */
    uint32_t t4_min;  /**< Attesa dopo OK prima di spegnere generatore (minuti) */
    uint32_t t5_ms;   /**< Attesa dopo OK prima di accendere pompa (secondi) */
    uint32_t t6_min;  /**< Timeout massimo pompa accesa (minuti, 0=infinito) */
    float    s1_v;    /**< Soglia VEXT per avvio automatico (Volt) */
    bool     autostart; /**< Avvio automatico quando VEXT < S1 */
} sequence_params_t;


/**
 * @brief Stati della macchina a stati della sequenza.
 */
typedef enum {
    SEQ_IDLE    = 0,  /**< Sequenza non in corso, pronta a ricevere comandi */
    SEQ_RUNNING = 1,  /**< Sequenza in esecuzione nel work queue */
} sequence_state_t;

/**
 * @brief Inizializza il modulo sequenza.
 *
 * Carica i parametri salvati in NVS tramite il Settings
 * subsystem di Zephyr. Se i parametri non sono presenti (primo avvio
 * o flash cancellata), usa i valori di default.
 *
 * @return 0 in caso di successo. In caso di errore Settings, usa i
 *         default e restituisce 0 comunque (non fatale).
 */
int sequence_init(void);

/**
 * @brief Avvia la sequenza di attivazione GPIO (comando START).
 *
 * Sottomette seq_work al system work queue. Se una sequenza e' gia'
 * in corso, restituisce -EBUSY senza fare nulla.
 *
 * @param sender  Numero del mittente a cui inviare il SMS di esito.
 *                La stringa viene copiata internamente.
 * @return 0 in caso di avvio riuscito, -EBUSY se gia' in corso.
 */
int sequence_start(const char *sender);

/**
 * @brief Avvia la sequenza di avvio con pompa (comando START POMPA).
 *
 * Simile a sequence_start(), ma include anche l'attivazione della
 * pompa dopo un certo intervallo. La sequenza completa e' descritta
 * nel diagramma all'inizio del file.
 *
 * @param sender  Numero del mittente a cui inviare il SMS di esito.
 *                La stringa viene copiata internamente.
 * @return 0 in caso di avvio riuscito, -EBUSY se gia' in corso.
 */
int sequence_start_pompa(const char *sender);

/**
 * @brief Avvia la sequenza di test GPIO (comando TEST).
 *
 * Attiva EXP_OUT0-7 in sequenza a 0.5s ciascuno, in loop continuo
 * finche' EXP_IN_0 rimane LOW. Si interrompe non appena EXP_IN_0
 * va HIGH e invia SMS di conferma al mittente.
 *
 * @param sender  Numero del mittente a cui inviare il SMS di esito.
 *                La stringa viene copiata internamente.
 * @return 0 in caso di avvio riuscito, -EBUSY se gia' in corso.
 */
int sequence_test_start(const char *sender);

/**
 * @brief Interrompe la sequenza in corso (comando STOP).
 *
 * Se la sequenza e' in esecuzione, viene terminata immediatamente,
 * tutte le uscite vengono spente e viene inviato un SMS di conferma
 * al mittente. Se non c'e' nessuna sequenza in corso, restituisce
 * -EINVAL senza fare nulla.
 *
 * @param sender  Numero del mittente a cui inviare il SMS di conferma.
 *                La stringa viene copiata internamente.
 * @return 0 in caso di interruzione riuscita, -EINVAL se nessuna
 *         sequenza in corso.
 */
void sequence_stop(const char *sender);

/**
 * @brief Restituisce lo stato corrente della macchina a stati.
 *
 * @return SEQ_IDLE o SEQ_RUNNING.
 */
sequence_state_t sequence_get_state(void);

/**
 * @brief Restituisce un puntatore ai parametri correnti (sola lettura).
 *
 * @return Puntatore alla struttura interna dei parametri.
 */
const sequence_params_t *sequence_get_params(void);

/**
 * @brief Imposta un parametro timer e lo salva in NVS.
 *
 * I valori vengono salvati tramite settings_save_one() e persistono
 * ai riavvii. La scrittura avviene sul settore NVS della flash interna
 * del nRF52840 con wear-leveling automatico.
 *
 * @param key      Nome del parametro: "T1", "T2" o "T3" (maiuscolo).
 * @param value_s  Valore in secondi (1-300).
 * @return 0 in caso di successo, -EINVAL se key o valore non validi,
 *         codice negativo in caso di errore di scrittura NVS.
 */
int sequence_set_param(const char *key, uint32_t value_s);

/**
 * @brief Imposta il parametro di soglia VEXT e lo salva in NVS.
 *
 * @param volt  Valore della soglia in volt (0.1-10.0).
 * @return 0 in caso di successo, -EINVAL se valore non valido,
 *         codice negativo in caso di errore di scrittura NVS.
 */
int sequence_set_s1(float volt);

/**
 * @brief Imposta il parametro di avvio automatico e lo salva in NVS.
 *
 * @param enabled  true per abilitare l'avvio automatico quando VEXT < S1,
 *                 false per disabilitare.
 * @return 0 in caso di successo, codice negativo in caso di errore di
 *         scrittura NVS.
 */
int sequence_set_autostart(bool enabled);

#endif /* SEQUENCE_H */