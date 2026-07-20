#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include "modem.h"
#include "gpio_ctrl.h"

LOG_MODULE_REGISTER(modem, CONFIG_LOG_DEFAULT_LEVEL);

/* ----------------------------------------------------------------
 * Costanti
 * ---------------------------------------------------------------- */
#define MODEM_UART_NODE     DT_NODELABEL(uart0)
#define RX_BUF_SIZE         2048
#define AT_DEFAULT_TIMEOUT  5000    /* ms */
#define AT_TIMEOUT_MAX      3  /* reset dopo 3 timeout consecutivi */

/* ----------------------------------------------------------------
 * Variabili statiche
 * ---------------------------------------------------------------- */
static const struct device *uart_dev;
static char  rx_buf[RX_BUF_SIZE];
static volatile int rx_pos;
static int at_timeout_count = 0;

/* ----------------------------------------------------------------
 * Callback UART interrupt-driven
 * ---------------------------------------------------------------- */
static void uart_rx_cb(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        uint8_t c;
        if (uart_fifo_read(dev, &c, 1) == 1) {
            if (rx_pos < RX_BUF_SIZE - 1) {
                rx_buf[rx_pos++] = (char)c;
                rx_buf[rx_pos]   = '\0';
            }
        }
    }
}

/* ----------------------------------------------------------------
 * Pulisce il buffer RX
 * ---------------------------------------------------------------- */
static void rx_clear(void)
{
    rx_pos = 0;
    memset(rx_buf, 0, RX_BUF_SIZE);
}

/* ----------------------------------------------------------------
 * Invia stringa sull'UART
 * ---------------------------------------------------------------- */
static void uart_tx_str(const char *s)
{
    while (*s) {
        uart_poll_out(uart_dev, (unsigned char)*s++);
    }
}

/* ----------------------------------------------------------------
 * Controlla se la risposta contiene OK o ERROR
 * Il BG95 risponde con \r\nOK\r\n oppure \r\nERROR\r\n
 * ---------------------------------------------------------------- */
static bool resp_has_ok(void)
{
    return (strstr(rx_buf, "OK") != NULL);
}

static bool resp_has_error(void)
{
    return (strstr(rx_buf, "ERROR") != NULL);
}

/* ================================================================
 * Funzioni esposte per sms.c
 * ================================================================ */
const struct device *modem_get_uart(void)
{
    return uart_dev;
}

/*
 * Svuota il buffer RX — da chiamare prima di inviare AT+CMGS
 * per evitare che dati residui nel buffer confondano il parsing
 * del prompt ">".
 */
void modem_rx_clear(void)
{
    rx_clear();
}

/*
 * Copia il contenuto attuale del buffer RX in buf.
 * Usato da sms_send() per verificare la presenza del prompt ">"
 * e della conferma "+CMGS:" senza passare per modem_send_at().
 */
void modem_rx_read(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, rx_buf, len - 1);
    buf[len - 1] = '\0';
}

/* ================================================================
 * API pubblica
 * ================================================================ */

int modem_init(void)
{
    uart_dev = DEVICE_DT_GET(MODEM_UART_NODE);
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART0 non pronto");
        return -ENODEV;
    }

    uart_irq_callback_set(uart_dev, uart_rx_cb);
    uart_irq_rx_enable(uart_dev);

    LOG_INF("Modem UART inizializzato");
    return 0;
}

int modem_send_at(const char *cmd, char *resp, size_t resp_len, int timeout_ms)
{
    LOG_INF("AT>> [%s]", cmd);

    rx_clear();

    uart_tx_str(cmd);
    uart_tx_str("\r\n");

    /* Attendi OK/ERROR con polling ogni 10 ms */
    int waited = 0;
    while (waited < timeout_ms) {
        k_msleep(10);
        waited += 10;
        if (resp_has_ok() || resp_has_error()) {
            break;
        }
    }

    /* Rimuovi \r e \n dalla risposta per log leggibili */
    char log_buf[256];
    int li = 0;
    for (int i = 0; rx_buf[i] && li < 254; i++) {
        if (rx_buf[i] != '\r' && rx_buf[i] != '\n') log_buf[li++] = rx_buf[i];
    }
    log_buf[li] = '\0';
    LOG_INF("AT<< [%s] (atteso %d ms)", log_buf, waited);

    if (resp && resp_len > 0) {
        strncpy(resp, rx_buf, resp_len - 1);
        resp[resp_len - 1] = '\0';
    }

    if (waited >= timeout_ms && !resp_has_ok()) {
        at_timeout_count++;
        LOG_WRN("Timeout AT comando: %s (%d/%d)",
                cmd, at_timeout_count, AT_TIMEOUT_MAX);
        if (at_timeout_count >= AT_TIMEOUT_MAX) {
            LOG_ERR("Troppi timeout consecutivi - reset modem");
            at_timeout_count = 0;
            gpio_ctrl_bg95_reset();
            modem_configure_network();
        }
    } else {
        at_timeout_count = 0;  /* reset contatore su risposta OK */
    }

    return resp_has_ok() ? 0 : -EIO;
}

int modem_check(void)
{
    char resp[64];
    for (int i = 0; i < 3; i++) {
        if (modem_send_at("AT", resp, sizeof(resp), AT_DEFAULT_TIMEOUT) == 0) {
            LOG_INF("Modem risponde OK");
            return 0;
        }
        k_msleep(1000);
    }
    LOG_ERR("Modem non risponde dopo 3 tentativi");
    return -ETIMEDOUT;
}

int modem_configure_network(void)
{
    char resp[128];

    /* Echo off */
    modem_send_at("ATE0", resp, sizeof(resp), AT_DEFAULT_TIMEOUT);

    /* LTE-M + NB-IoT */
    modem_send_at("AT+QCFG=\"nwscanseq\",020301,1",
                  resp, sizeof(resp), AT_DEFAULT_TIMEOUT);
    modem_send_at("AT+QCFG=\"iotopmode\",2,1",
                  resp, sizeof(resp), AT_DEFAULT_TIMEOUT);

    /* APN Swisscom Switzerland */
    modem_send_at("AT+CGDCONT=1,\"IP\",\"gprs.swisscom.ch\"",
                  resp, sizeof(resp), AT_DEFAULT_TIMEOUT);

    /* Attendi registrazione rete (max 30 s) */
    for (int i = 0; i < 30; i++) {
        modem_send_at("AT+CREG?", resp, sizeof(resp), AT_DEFAULT_TIMEOUT);
        if (strstr(resp, "+CREG: 0,1") || strstr(resp, "+CREG: 0,5")) {
            LOG_INF("Registrato in rete");
            return 0;
        }
        k_msleep(1000);
    }

    LOG_ERR("Timeout registrazione rete");
    return -ETIMEDOUT;
}

int modem_get_battery(uint8_t *percent, uint16_t *mv)
{
    char resp[64];
    int ret = modem_send_at("AT+CBC", resp, sizeof(resp), 2000);
    if (ret != 0) {
        return -EIO;
    }

    /* Parsing: +CBC: <status>,<percent>,<mv> */
    int status, pct, millivolt;
    if (sscanf(resp, " +CBC: %d,%d,%d", &status, &pct, &millivolt) != 3) {
        LOG_ERR("AT+CBC: parsing fallito: %s", resp);
        return -EINVAL;
    }

    *percent = (uint8_t)pct;
    *mv      = (uint16_t)millivolt;
    return 0;
}

int modem_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second,
                   uint8_t *day, uint8_t *month, uint16_t *year)
{
    char resp[64];
    int ret = modem_send_at("AT+CCLK?", resp, sizeof(resp), 2000);
    if (ret != 0) {
        LOG_ERR("AT+CCLK fallito");
        return -EIO;
    }

    /* Formato risposta: +CCLK: "YY/MM/DD,HH:MM:SS+TZ" */
    uint8_t yy, mo, dd, hh, mm, ss;
    int tz;

    if (sscanf(resp, " +CCLK: \"%hhu/%hhu/%hhu,%hhu:%hhu:%hhu%d\"",
               &yy, &mo, &dd, &hh, &mm, &ss, &tz) != 7) {
        LOG_ERR("AT+CCLK: parsing fallito: %s", resp);
        return -EINVAL;
    }

    if (hour)   *hour   = hh;
    if (minute) *minute = mm;
    if (second) *second = ss;
    if (day)    *day    = dd;
    if (month)  *month  = mo;
    if (year)   *year   = 2000 + yy;

    return 0;
}

int modem_get_signal(uint8_t *rssi, int16_t *dbm)
{
    char resp[32];
    if (modem_send_at("AT+CSQ", resp, sizeof(resp), 1000) != 0) {
        return -EIO;
    }

    uint8_t r, ber;
    if (sscanf(resp, " +CSQ: %hhu,%hhu", &r, &ber) != 2) {
        return -EINVAL;
    }

    if (rssi) *rssi = r;
    
    if (r == 99) {
        /* 99 = segnale sconosciuto/non rilevabile - dbm non significativo */
        if (dbm) *dbm = 0;
        return -ENODATA;
    }

    if (dbm) *dbm = -113 + (r * 2);
    return 0;
}
