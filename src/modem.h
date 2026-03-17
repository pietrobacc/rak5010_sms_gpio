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

#endif /* MODEM_H */
