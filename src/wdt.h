/*
 * wdt.h - Watchdog hardware nRF52840
 *
 * Il WDT resetta il dispositivo se non viene "alimentato"
 * (wdt_feed) entro il timeout configurato.
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WDT_H
#define WDT_H

/**
 * @brief Inizializza il watchdog hardware.
 *
 * Configura il WDT con un timeout di 30 secondi.
 * Da chiamare una volta all'avvio prima del loop principale.
 *
 * @return 0 in caso di successo, negativo in caso di errore.
 */
int wdt_init(void);

/**
 * @brief Alimenta il watchdog (reset del contatore).
 *
 * Va chiamato periodicamente dal loop principale.
 * Se non viene chiamato entro il timeout, il dispositivo
 * viene resettato automaticamente.
 */
void app_wdt_feed(void);

#endif /* WDT_H */