#ifndef PLANT_STATUS_H
#define PLANT_STATUS_H

#include <stddef.h>

/*
 * plant_status.h - Classificazione sintetica dello stato impianto.
 *
 * Combina sequenza + IN0-3 + OUT0-2 in un unico giudizio (OK/ATTENZIONE/
 * ANOMALIA/IMPOSSIBILE) con descrizione testuale, secondo le regole
 * documentate in STATI_IMPIANTO.md. Pensato per il logging ad ogni ciclo
 * del loop principale.
 */

typedef enum {
    PLANT_OK,           /* stato normale/atteso */
    PLANT_ATTENZIONE,   /* situazione reale, gestita attivamente dal firmware */
    PLANT_ANOMALIA,     /* incoerenza SW/HW - non dovrebbe mai capitare */
    PLANT_IMPOSSIBILE,  /* combinazione sensori fisicamente contraddittoria */
} plant_level_t;

/**
 * @brief Valuta lo stato attuale dell'impianto (legge da solo sequenza,
 *        IN0-3, OUT0-2 - nessun parametro necessario).
 *
 * @param desc      Buffer dove scrivere la descrizione testuale.
 * @param desc_len  Dimensione del buffer.
 * @return Livello di gravita' rilevato.
 */
plant_level_t plant_status_evaluate(char *desc, size_t desc_len);

/** @brief Rappresentazione testuale del livello (es. "OK", "ANOMALIA"). */
const char *plant_level_str(plant_level_t level);

#endif /* PLANT_STATUS_H */
