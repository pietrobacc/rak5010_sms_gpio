/*
 * auth.h - Gestione numeri autorizzati e notifiche
 *
 * Numeri 1-3: settabili via SMS, salvati in NVS
 * Numeri 4-5: hardcodati (supporto tecnico)
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stdint.h>

/** Numeri tecnici fissi - modificare prima della compilazione */
#define TECH_NUMBER_1  "+41792793879"   // Ciocio
#define TECH_NUMBER_2  "+41798904050"   // Bacc

/**
 * @brief Inizializza il modulo auth, carica i numeri da NVS.
 */
int auth_init(void);

/**
 * @brief Verifica se il mittente e' autorizzato.
 * @param sender  Numero mittente.
 * @return true se autorizzato.
 */
bool auth_is_authorized(const char *sender);

/**
 * @brief Imposta un numero cliente (1-3). Stringa vuota = cancella.
 * @param index  1, 2 o 3.
 * @param number Numero in formato internazionale o "" per cancellare.
 * @return 0 in caso di successo.
 */
int auth_set_number(uint8_t index, const char *number);

/**
 * @brief Restituisce un numero cliente.
 * @param index  1, 2 o 3.
 * @return Stringa numero o "" se non impostato.
 */
const char *auth_get_number(uint8_t index);

/**
 * @brief Imposta il numero di notifica automatica (1-3).
 * @param index  1, 2 o 3.
 * @return 0 in caso di successo.
 */
int auth_set_notify(uint8_t index);

/**
 * @brief Restituisce il numero destinatario delle notifiche automatiche.
 * @return Numero notifiche, o TECH_NUMBER_1 se non configurato.
 */
const char *auth_get_notify_number(void);

/**
 * @brief Restituisce l'indice del numero di notifica (1-3).
 */
uint8_t auth_get_notify_index(void);

#endif /* AUTH_H */