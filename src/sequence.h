/*
 * sequence.h - Macchina a stati per la sequenza di avvio
 *
 * Gestisce la sequenza temporizzata di attivazione GPIO e il
 * successivo controllo del segnale di feedback. I parametri
 * temporali sono configurabili via SMS e persistiti in flash NVS.
 *
 * Flusso della sequenza START:
 *
 *   [SMS "START" ricevuto]
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
 *   EXP_OUT0 = OFF, EXP_OUT1 = OFF
 *          |
 *     polling EXP_IN_0
 *     (ogni 50ms, max T3)
 *          |
 *          +---> EXP_IN_0 HIGH entro T3 --> SMS "Accensione OK"
 *          |
 *          +---> Timeout T3              --> SMS "Accensione FALLITA"
 *
 * Flusso della sequenza TEST:
 *
 *   [SMS "TEST" ricevuto]
 *          |
 *          v
 *   EXP_OUT0..7 attivati in sequenza (0.5s ciascuno)
 *          |
 *     ciclo continuo finche' EXP_IN_0 = 0
 *          |
 *          +---> EXP_IN_0 HIGH --> stop + SMS "TEST completato"
 *
 * Le sequenze vengono eseguite tramite k_work nel system work queue
 * di Zephyr, senza bloccare il thread principale (polling SMS).
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
    uint32_t t1_ms;  /**< Pausa tra attivazione EXP_OUT0 e EXP_OUT1 */
    uint32_t t2_ms;  /**< Pausa tra attivazione EXP_OUT1 e disattivazione */
    uint32_t t3_ms;  /**< Timeout massimo attesa segnale HIGH su EXP_IN_0 */
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
 * Carica i parametri T1/T2/T3 salvati in NVS tramite il Settings
 * subsystem di Zephyr. Se i parametri non sono presenti (primo avvio
 * o flash cancellata), usa i valori di default: T1=5s, T2=5s, T3=5s.
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