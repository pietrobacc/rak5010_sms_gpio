#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "modem.h"
#include "sms.h"

LOG_MODULE_REGISTER(sms, CONFIG_LOG_DEFAULT_LEVEL);

/* Callback utente */
static sms_received_cb_t user_cb;

/* ================================================================
 * Init
 * ================================================================ */
int sms_init(sms_received_cb_t cb)
{
    char resp[128];
    int  ret;

    user_cb = cb;

    /* Modalità testo */
    ret = modem_send_at("AT+CMGF=1", resp, sizeof(resp), 5000);
    if (ret != 0) {
        LOG_ERR("AT+CMGF=1 fallito");
        return ret;
    }

    /* Charset GSM */
    modem_send_at("AT+CSCS=\"GSM\"", resp, sizeof(resp), 5000);

    /*
     * Notifica nuovi SMS via URC +CMTI
     * mode=2, mt=1 → notifica +CMTI senza recapitare il testo
     */
    ret = modem_send_at("AT+CNMI=2,1,0,0,0", resp, sizeof(resp), 5000);
    if (ret != 0) {
        LOG_ERR("AT+CNMI fallito");
        return ret;
    }

    /* Cancella SMS residui */
    sms_delete_all();

    LOG_INF("SMS subsystem pronto");
    return 0;
}

/* ================================================================
 * Invio SMS
 *
 * Il BG95 segue questo protocollo per l'invio:
 *   1. Inviamo:  AT+CMGS="+numero"\r\n
 *   2. Modem risponde: \r\n>  (prompt, non OK)
 *   3. Inviamo:  testo del messaggio + 0x1A (Ctrl+Z)
 *   4. Modem risponde: +CMGS: <id>\r\nOK  (conferma invio)
 *
 * modem_send_at() non puo' essere usato per il passo 1 perche'
 * aspetta "OK" o "ERROR", ma il modem risponde ">" — va in timeout.
 * La gestione viene quindi fatta manualmente tramite modem_get_uart().
 * ================================================================ */
int sms_send(const char *number, const char *message)
{
    char cmd[48];
    const struct device *uart_dev = modem_get_uart();

    if (!number || !message || !uart_dev) return -EINVAL;

    if (modem_lock(25000) != 0) {
        LOG_ERR("sms_send: UART occupata, invio annullato");
        return -EAGAIN;
    }
 
    LOG_INF("Invio SMS a %s: [%s]", number, message);

    /* --- Step 1: invia AT+CMGS="numero"\r\n --- */
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", number);
    LOG_INF("AT>> [AT+CMGS=\"%s\"]", number);

    modem_rx_clear();   /* svuota buffer RX prima di iniziare */

    for (int i = 0; cmd[i] != '\0'; i++) {
        uart_poll_out(uart_dev, (uint8_t)cmd[i]);
    }

    /* --- Step 2: attendi prompt ">" dal modem (max 5s) --- */
    char rx[64];
    bool got_prompt = false;
    uint32_t elapsed = 0;

    memset(rx, 0, sizeof(rx));

    while (elapsed < 5000) {
        modem_rx_read(rx, sizeof(rx));
        if (strstr(rx, ">")) {
            got_prompt = true;
            LOG_INF("Prompt '>' ricevuto dopo %u ms", elapsed);
            break;
        }
        k_msleep(50);
        elapsed += 50;
    }

    if (!got_prompt) {
        LOG_ERR("Timeout attesa prompt '>' per AT+CMGS");
        modem_unlock();
        return -ETIMEDOUT;
    }

    /* --- Step 3: invia testo + Ctrl+Z --- */
    for (int i = 0; message[i] != '\0'; i++) {
        uart_poll_out(uart_dev, (uint8_t)message[i]);
    }
    uart_poll_out(uart_dev, 0x1A);  /* Ctrl+Z: conferma invio */

    /* --- Step 4: attendi +CMGS di conferma (max 15s) --- */
    memset(rx, 0, sizeof(rx));
    elapsed = 0;
    bool sent_ok = false;

    while (elapsed < 15000) {
        modem_rx_read(rx, sizeof(rx));
        if (strstr(rx, "+CMGS:")) {
            sent_ok = true;
            LOG_INF("SMS inviato confermato dal modem (+CMGS)");
            break;
        }
        if (strstr(rx, "ERROR")) {
            LOG_ERR("Errore invio SMS: %s", rx);
            modem_unlock();
            return -EIO;
        }
        k_msleep(100);
        elapsed += 100;
    }

    modem_unlock();

    if (!sent_ok) {
        LOG_WRN("Timeout attesa +CMGS - SMS potrebbe non essere stato inviato");
        return -ETIMEDOUT;
    }

    LOG_INF("SMS inviato con successo");
    return 0;
}

/* ================================================================
 * Lettura SMS per indice
 * ================================================================ */
int sms_read(int index, sms_message_t *msg)
{
    char cmd[32];
    char resp[512];
    int  ret;

    if (!msg) return -EINVAL;

    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
    ret = modem_send_at(cmd, resp, sizeof(resp), 5000);
    if (ret != 0) {
        LOG_WRN("AT+CMGR=%d fallito", index);
        return -ENOENT;
    }

    /*
     * Risposta formato testo BG95:
     * +CMGR: "REC UNREAD","+41XXXXXXXXX","","25/01/01,12:00:00+04"
     * Testo del messaggio
     * OK
     */
    char *cmgr = strstr(resp, "+CMGR:");
    if (!cmgr) {
        LOG_WRN("Nessun +CMGR nella risposta");
        return -EINVAL;
    }

    memset(msg, 0, sizeof(*msg));
    msg->index = index;

    /* Estrai numero mittente (secondo campo tra virgolette) */
    char *q = strchr(cmgr, '"');            /* inizio "REC UNREAD" o "REC READ" */
    if (q) q = strchr(q + 1, '"');         /* fine status */
    if (q) q = strchr(q + 1, '"');         /* inizio numero */
    if (q) {
        char *end = strchr(q + 1, '"');
        if (end) {
            int len = (int)(end - q - 1);
            if (len > SMS_SENDER_MAX_LEN - 1) len = SMS_SENDER_MAX_LEN - 1;
            strncpy(msg->sender, q + 1, len);
            msg->sender[len] = '\0';
        }
    }

    /* Estrai testo: seconda riga dopo \n */
    char *nl = strchr(cmgr, '\n');
    if (nl) {
        nl++;
        if (*nl == '\r') nl++;  /* salta \r eventuale */
        char *end = strpbrk(nl, "\r\n");
        int len = end ? (int)(end - nl) : (int)strlen(nl);
        if (len > SMS_TEXT_MAX_LEN - 1) len = SMS_TEXT_MAX_LEN - 1;
        strncpy(msg->text, nl, len);
        msg->text[len] = '\0';
    }

    LOG_INF("SMS[%d] da=[%s] testo=[%s]", index, msg->sender, msg->text);
    return 0;
}

/* ================================================================
 * Eliminazione SMS
 * ================================================================ */
int sms_delete(int index)
{
    char cmd[32];
    char resp[64];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    int ret = modem_send_at(cmd, resp, sizeof(resp), 5000);
    LOG_INF("Eliminato SMS[%d]: %s", index, ret == 0 ? "OK" : "ERRORE");
    return ret;
}

int sms_delete_all(void)
{
    char resp[64];
    return modem_send_at("AT+CMGD=1,4", resp, sizeof(resp), 8000);
}

/* ================================================================
 * Polling SMS non letti
 * ================================================================ */
void sms_poll(void)
{
    char resp[1024];

    /* Verifica modalità testo - potrebbe essere persa dopo reset modem */
    if (modem_send_at("AT+CMGF?", resp, sizeof(resp), 1000) == 0) {
        if (strstr(resp, "+CMGF: 0") != NULL) {
            LOG_WRN("SMS: modalità PDU rilevata - ripristino testo");
            modem_send_at("AT+CMGF=1", NULL, 0, 1000);
        }
    }
    
    LOG_INF("Polling SMS non letti...");

    int ret = modem_send_at("AT+CMGL=\"REC UNREAD\"",
                            resp, sizeof(resp), 10000);

    if (ret != 0) {
        LOG_WRN("AT+CMGL fallito o nessun SMS non letto");
        return;
    }

    /* Controlla se ci sono messaggi */
    if (!strstr(resp, "+CMGL:")) {
        LOG_INF("Nessun SMS non letto");
        return;
    }

    /* Processa ogni +CMGL trovato */
    char *p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL) {
        /* Formato: +CMGL: <index>,"REC UNREAD",... */
        /* Salta "+CMGL:" e spazi */
        char *num_start = p + 6;
        while (*num_start == ' ') num_start++;

        /* Verifica che ci sia davvero un numero */
        if (*num_start >= '0' && *num_start <= '9') {
            int sms_idx = atoi(num_start);
            LOG_INF("Trovato SMS non letto all'indice %d", sms_idx);

            sms_message_t msg;
            if (sms_read(sms_idx, &msg) == 0) {
                if (user_cb) {
                    user_cb(&msg);
                }
            }
            sms_delete(sms_idx);
        }
        p++;
    }
}
