#ifndef SMS_H
#define SMS_H

#include <stdint.h>
#include <stdbool.h>

/* Lunghezze massime */
#define SMS_SENDER_MAX_LEN   24
#define SMS_TEXT_MAX_LEN    161   /* 160 caratteri + null terminator */

/**
 * @brief Struttura che rappresenta un SMS ricevuto.
 */
typedef struct {
    char sender[SMS_SENDER_MAX_LEN];    /* Numero mittente es. "+39XXXXXXXXXX" */
    char text[SMS_TEXT_MAX_LEN];        /* Testo del messaggio                 */
    int  index;                         /* Indice in memoria SIM               */
} sms_message_t;

/**
 * @brief Prototipo callback invocata alla ricezione di un nuovo SMS.
 *
 * @param msg  Puntatore al messaggio ricevuto (valido solo durante la callback).
 */
typedef void (*sms_received_cb_t)(const sms_message_t *msg);

/**
 * @brief Inizializza il sottosistema SMS.
 *        Configura il modem in modalità testo, abilita le notifiche
 *        +CMTI per i nuovi messaggi in arrivo.
 *
 * @param cb  Callback da invocare quando arriva un SMS (può essere NULL).
 * @return 0 OK, negativo in caso di errore.
 */
int sms_init(sms_received_cb_t cb);

/**
 * @brief Invia un SMS.
 *
 * @param number   Numero destinatario in formato internazionale (es. "+39XXXXXXXXXX").
 * @param message  Testo del messaggio (max 160 caratteri).
 * @return 0 OK, negativo in caso di errore.
 */
int sms_send(const char *number, const char *message);

/**
 * @brief Legge un SMS dalla memoria SIM tramite indice.
 *
 * @param index  Indice SIM (a partire da 1).
 * @param msg    Struttura dove salvare il messaggio.
 * @return 0 OK, -ENOENT se non trovato, negativo per altri errori.
 */
int sms_read(int index, sms_message_t *msg);

/**
 * @brief Elimina un SMS dalla memoria SIM.
 *
 * @param index  Indice SIM da eliminare.
 * @return 0 OK, negativo in caso di errore.
 */
int sms_delete(int index);

/**
 * @brief Elimina tutti gli SMS dalla memoria SIM.
 * @return 0 OK, negativo in caso di errore.
 */
int sms_delete_all(void);

/**
 * @brief Polling: controlla se sono arrivati nuovi SMS e invoca la callback.
 *        Da chiamare periodicamente nel loop principale se non si usa
 *        la notifica via interrupt UART.
 */
void sms_poll(void);

#endif /* SMS_H */
