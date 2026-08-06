#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "victron.h"

LOG_MODULE_REGISTER(victron, CONFIG_LOG_DEFAULT_LEVEL);

#define VICTRON_UART_NODE   DT_NODELABEL(uart1)
#define LINE_BUF_SIZE       64
#define VICTRON_STALE_MS    10000

static const struct device *uart_dev;

/* Buffer di riga in costruzione (contesto interrupt) */
static char line_buf[LINE_BUF_SIZE];
static int  line_pos;

/* Frame in costruzione: accumula i campi finche' non arriva "Checksum",
 * che segna la fine del blocco VE.Direct (contesto interrupt) */
static victron_data_t working;
static bool working_has_data;

/* Ultimo frame completo, valido e "pubblicato": protetto da mutex per
 * l'accesso da altri thread (es. handle_status() in main.c) */
static victron_data_t latest;
static struct k_mutex latest_mutex;

const char *victron_cs_str(int cs)
{
    switch (cs) {
    case 0:   return "Off";
    case 2:   return "Fault";
    case 3:   return "Bulk";
    case 4:   return "Absorption";
    case 5:   return "Float";
    case 7:   return "Equalize";
    case 245: return "Avvio";
    default:  return "?";
    }
}

/*
 * Elabora una singola riga "Label\tValore" del blocco VE.Direct.
 *
 * NOTA: il protocollo prevede anche una validazione tramite un byte di
 * checksum grezzo (non necessariamente stampabile) sulla riga finale
 * "Checksum". Questa prima versione NON verifica il checksum - accetta
 * i campi cosi' come arrivano. E' un miglioramento possibile in futuro,
 * se si notano dati corrotti/incoerenti sul campo.
 */
static void process_line(const char *line)
{
    char label[16];
    const char *tab = strchr(line, '\t');
    if (!tab) {
        return;  /* riga malformata, ignorata */
    }

    size_t label_len = (size_t)(tab - line);
    if (label_len >= sizeof(label)) {
        return;
    }
    memcpy(label, line, label_len);
    label[label_len] = '\0';

    const char *value = tab + 1;

    if (strcmp(label, "V") == 0) {
        working.vbatt_v = (float)atoi(value) / 1000.0f;
        working_has_data = true;
    } else if (strcmp(label, "I") == 0) {
        working.ibatt_a = (float)atoi(value) / 1000.0f;
        working_has_data = true;
    } else if (strcmp(label, "VPV") == 0) {
        working.vpv_v = (float)atoi(value) / 1000.0f;
        working_has_data = true;
    } else if (strcmp(label, "PPV") == 0) {
        working.ppv_w = (float)atoi(value);
        working_has_data = true;
    } else if (strcmp(label, "CS") == 0) {
        working.cs = atoi(value);
        working_has_data = true;
    } else if (strcmp(label, "ERR") == 0) {
        working.err = atoi(value);
        working_has_data = true;
    } else if (strcmp(label, "Checksum") == 0) {
        /* Fine blocco: pubblica se abbiamo raccolto almeno un campo utile */
        if (working_has_data) {
            working.valid = true;
            working.last_update_ms = k_uptime_get();

            k_mutex_lock(&latest_mutex, K_FOREVER);
            latest = working;
            k_mutex_unlock(&latest_mutex);                  
        }
        memset(&working, 0, sizeof(working));
        working_has_data = false;
    }
    /* altre etichette (PID, FW, SER#, H19...H23, HSDS, ecc.) ignorate
     * volutamente in questa prima versione - non servono per il
     * monitoraggio base carica/scarica */
}

static void uart_cb(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
        return;
    }

    uint8_t c;
    while (uart_fifo_read(dev, &c, 1) == 1) {
        if (c == '\r') {
            continue;  /* il protocollo usa CRLF, ignoriamo il CR */
        }
        if (c == '\n') {
            line_buf[line_pos] = '\0';
            if (line_pos > 0) {
                process_line(line_buf);
            }
            line_pos = 0;
            continue;
        }
        if (line_pos < (int)(sizeof(line_buf) - 1)) {
            line_buf[line_pos++] = (char)c;
        } else {
            /* riga anomala/troppo lunga - scarta e riparti pulito */
            line_pos = 0;
        }
    }
}

int victron_init(void)
{
    uart_dev = DEVICE_DT_GET(VICTRON_UART_NODE);
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("Victron: UART1 non pronta");
        return -ENODEV;
    }

    k_mutex_init(&latest_mutex);
    memset(&working, 0, sizeof(working));
    memset(&latest, 0, sizeof(latest));
    line_pos = 0;

    uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    LOG_INF("Victron VE.Direct: UART1 pronta (19200 baud, P1.01/P1.02)");
    return 0;
}

int victron_get_data(victron_data_t *out)
{
    if (!out) {
        return -EINVAL;
    }

    k_mutex_lock(&latest_mutex, K_FOREVER);
    *out = latest;
    k_mutex_unlock(&latest_mutex);

    if (!out->valid) {
        return -EAGAIN;
    }

    int64_t age_ms = k_uptime_get() - out->last_update_ms;
    if (age_ms > VICTRON_STALE_MS) {
        return -EAGAIN;
    }

    return 0;
}
