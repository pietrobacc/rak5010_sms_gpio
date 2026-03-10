/*
 * gpio_ctrl.h - Gestione GPIO del RAK5010-M
 *
 * Astrae l'accesso ai pin GPIO della board, separando la logica
 * hardware dal resto dell'applicazione. Tutti i pin sono definiti
 * nel Device Tree (app.overlay + DTS ufficiale); questo modulo
 * li legge tramite le macro DT_* di Zephyr e non contiene numeri
 * di pin hardcodati nel codice C.
 *
 * Pin gestiti:
 *   LED verde  : P0.12  (DTS ufficiale, alias led0)
 *   BG95 PWRKEY: P0.02  (DTS ufficiale, nodo modem in uart0)
 *   OUT1       : P0.19  (app.overlay, J10 pin 4, level-shifted)
 *   OUT2       : P0.20  (app.overlay, J12 pin 2, level-shifted)
 *   OUT3       : P1.02  (app.overlay, J12 pin 3, level-shifted)
 *   IN4        : P1.01  (app.overlay, J12 pin 4, level-shifted, INGRESSO)
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPIO_CTRL_H
#define GPIO_CTRL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Indici degli output GPIO utente.
 *
 * Usati come argomento di gpio_ctrl_output_set() e
 * gpio_ctrl_output_get(). Il pin NRF_IO4 (P1.01) NON e' incluso
 * in questo enum perche' e' configurato come ingresso (IN4) e
 * gestito separatamente da gpio_ctrl_in4_get().
 */
typedef enum {
    GPIO_OUT_1 = 0,   /**< NRF_IO1 - P0.19 - J10 pin 4 */
    GPIO_OUT_2,       /**< NRF_IO2 - P0.20 - J12 pin 2 */
    GPIO_OUT_3,       /**< NRF_IO3 - P1.02 - J12 pin 3 */
    GPIO_OUT_COUNT    /**< Numero totale di output (non usare come indice) */
} gpio_out_id_t;

/**
 * @brief Inizializza tutti i GPIO dell'applicazione.
 *
 * Configura LED e PWRKEY come output inattivi, OUT1/OUT2/OUT3 come
 * output inattivi, IN4 come ingresso con pull-down interno.
 * Da chiamare prima di qualsiasi altra funzione di questo modulo.
 *
 * @return 0 in caso di successo, codice di errore negativo altrimenti.
 */
int gpio_ctrl_init(void);

/**
 * @brief Accende o spegne il LED verde utente (P0.12).
 *
 * @param on  true = LED acceso, false = LED spento.
 */
void gpio_ctrl_led_set(bool on);

/**
 * @brief Imposta lo stato di un output GPIO utente.
 *
 * @param id     Indice dell'output (GPIO_OUT_1 .. GPIO_OUT_3).
 * @param value  true = HIGH (attivo), false = LOW (inattivo).
 * @return 0 in caso di successo, -EINVAL se id non valido.
 */
int gpio_ctrl_output_set(gpio_out_id_t id, bool value);

/**
 * @brief Legge lo stato corrente di un output GPIO.
 *
 * @param id  Indice dell'output.
 * @return 1 = HIGH, 0 = LOW, valore negativo = errore.
 */
int gpio_ctrl_output_get(gpio_out_id_t id);

/**
 * @brief Legge lo stato dell'ingresso IN4 (NRF_IO4 - P1.01 - J12 pin 4).
 *
 * Il pin e' configurato con pull-down interno: restituisce 0 quando
 * nessun segnale e' presente, 1 quando il segnale esterno e' HIGH.
 * Utilizzato dalla sequenza di avvio per verificare il feedback
 * del dispositivo controllato.
 *
 * @return 1 = HIGH (segnale rilevato), 0 = LOW, negativo = errore.
 */
int gpio_ctrl_in4_get(void);

/**
 * @brief Esegue la sequenza di accensione del modem BG95-M3.
 *
 * Genera un impulso sul pin PWRKEY (P0.02) della durata di 600 ms
 * come richiesto dal datasheet Quectel BG95-M3 (minimo 500 ms).
 * Attende poi 5 secondi per il completamento del boot del modem.
 * Questa funzione e' bloccante per ~5.6 secondi totali.
 */
void gpio_ctrl_bg95_power_on(void);

/**
 * @brief Esegue la sequenza di spegnimento del modem BG95-M3.
 *
 * Genera un impulso sul pin PWRKEY della durata di 700 ms
 * (datasheet richiede minimo 650 ms per lo spegnimento).
 * Questa funzione e' bloccante per ~3.7 secondi totali.
 */
void gpio_ctrl_bg95_power_off(void);

/**
 * @brief Esegue un reset hardware del modem BG95-M3 via power cycle.
 *
 * Poiche' il pin RESET del BG95 non e' esposto nel DTS ufficiale,
 * il reset viene realizzato come sequenza: power off + pausa + power on.
 * Funzione bloccante per ~10 secondi totali.
 */
void gpio_ctrl_bg95_reset(void);

#endif /* GPIO_CTRL_H */
