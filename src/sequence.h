/*
 * sequence.h - Macchina a stati per la sequenza di avvio
 *
 * Gestisce la sequenza temporizzata di attivazione GPIO e il
 * successivo controllo del segnale di feedback. I parametri
 * temporali sono configurabili via SMS e persistiti in flash NVS.
 *
 * Flusso della sequenza:
 *
 *   [SMS "START" ricevuto]
 *          |
 *          v
 *   OUT1 = ON
 *          |
 *       attesa T1
 *          |
 *          v
 *   OUT2 = ON
 *          |
 *       attesa T2
 *          |
 *          v
 *   OUT1 = OFF, OUT2 = OFF
 *          |
 *     polling IN4
 *     (ogni 50ms, max T3)
 *          |
 *          +---> IN4 HIGH entro T3 --> SMS "Accensione OK"
 *          |
 *          +---> Timeout T3        --> SMS "Accensione FALLITA"
 *
 * La sequenza viene eseguita in un thread Zephyr dedicato per non
 * bloccare il thread principale durante le attese (k_msleep).
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SEQUENCE_H
#define SEQUENCE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Parametri temporali della sequenza di avvio.
 *
 * Tutti i valori sono in millisecondi internamente, ma vengono
 * impostati via SMS in secondi interi (1-300).
 * Vengono salvati in NVS e caricati ad ogni riavvio.
 */
typedef struct {
    uint32_t t1_ms;  /**< Pausa tra attivazione OUT1 e attivazione OUT2 */
    uint32_t t2_ms;  /**< Pausa tra attivazione OUT2 e disattivazione OUT1+OUT2 */
    uint32_t t3_ms;  /**< Timeout massimo attesa segnale HIGH su IN4 */
} sequence_params_t;

/**
 * @brief Stati della macchina a stati della sequenza.
 */
typedef enum {
    SEQ_IDLE    = 0,  /**< Sequenza non in corso, pronta a ricevere START */
    SEQ_RUNNING = 1,  /**< Sequenza in esecuzione nel thread dedicato */
} sequence_state_t;

/**
 * @brief Inizializza il modulo sequenza.
 *
 * Carica i parametri T1/T2/T3 salvati in NVS tramite il Settings
 * subsystem di Zephyr. Se i parametri non sono presenti (primo avvio
 * o flash cancellata), usa i valori di default: T1=5s, T2=5s, T3=5s.
 *
 * @return 0 in caso di successo. In caso di errore Settings, usa i
 *         default e restituisce 0 comunque (non fatale).
 */
int sequence_init(void);

/**
 * @brief Avvia la sequenza di attivazione GPIO.
 *
 * Lancia il thread dedicato della sequenza. Se una sequenza e' gia'
 * in corso, restituisce -EBUSY senza fare nulla (il chiamante decide
 * come gestire il caso, tipicamente ignorando il comando START).
 *
 * @param sender  Numero del mittente a cui inviare il SMS di esito.
 *                La stringa viene copiata internamente.
 * @return 0 in caso di avvio riuscito, -EBUSY se gia' in corso.
 */
int sequence_start(const char *sender);

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

#endif /* SEQUENCE_H */
