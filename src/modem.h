#ifndef MODEM_H
#define MODEM_H

#include <stddef.h>
#include <zephyr/device.h>

int modem_init(void);
int modem_send_at(const char *cmd, char *resp, size_t resp_len, int timeout_ms);
int modem_check(void);
int modem_configure_network(void);
int modem_get_battery(uint8_t *percent, uint16_t *mv);

/* Restituisce il device UART del modem (usato da sms.c per Ctrl+Z) */
const struct device *modem_get_uart(void);

/* Svuota il buffer RX del modem */
void modem_rx_clear(void);

/* Copia il contenuto attuale del buffer RX in buf (max len bytes) */
void modem_rx_read(char *buf, size_t len);

/**
 * @brief Legge l'ora corrente dal modem via AT+CCLK.
 *
 * @param hour    Ore (0-23)
 * @param minute  Minuti (0-59)
 * @param second  Secondi (0-59)
 * @param day     Giorno (1-31)
 * @param month   Mese (1-12)
 * @param year    Anno (es. 2026)
 * @return 0 in caso di successo, negativo in caso di errore.
 */
int modem_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second,
                   uint8_t *day, uint8_t *month, uint16_t *year);

/** @brief Legge il livello del segnale dal modem via AT+CSQ.
 *
 * @param rssi  Livello del segnale (0-31, 99 se sconosciuto)
 * @param dbm   Livello del segnale in dBm (se disponibile)
 * @return 0 in caso di successo, negativo in caso di errore.
 */
int modem_get_signal(uint8_t *rssi, int16_t *dbm);

#endif /* MODEM_H */
