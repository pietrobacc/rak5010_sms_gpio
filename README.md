# RAK5010-M + BG95-M3 — SMS → GPIO Controller

Firmware Zephyr per **nRF Connect SDK v3.2.3** che riceve comandi via SMS
e attiva/disattiva i GPIO del RAK5010-M.

---

## Struttura progetto

```
rak5010_sms_gpio/
├── CMakeLists.txt       — definizione target e sorgenti
├── prj.conf             — Kconfig (driver abilitati)
├── app.overlay          — device tree (pin reali RAK5010-M)
└── src/
    ├── main.c           — loop principale + tabella comandi SMS
    ├── gpio_ctrl.c/h    — gestione LED, PWRKEY, output utente
    ├── modem.c/h        — comunicazione AT via UART0
    └── sms.c/h          — invio/ricezione/parsing SMS
```

---

## Pin mapping RAK5010-M (BG95-M3)

| Segnale        | Pin nRF52840 | Direzione  |
|----------------|-------------|------------|
| UART0 TX→BG95  | P0.15       | Output     |
| UART0 RX←BG95  | P0.13       | Input      |
| BG95 PWRKEY    | P0.02       | Output     |
| BG95 RESET     | P0.28       | Output     |
| BG95 DTR       | P0.26       | Output     |
| LED verde      | P0.12       | Output     |
| Output 1       | P0.19       | Output     |
| Output 2       | P0.20       | Output     |
| Output 3       | P1.02       | Output     |
| Output 4       | P1.01       | Output     |

---

## Configurazione prima della compilazione

Modifica in `src/main.c`:

```c
// Numero autorizzato (lascia "" per accettare tutti)
#define AUTHORIZED_NUMBER   "+39XXXXXXXXXX"

// Abilita/disabilita risposta SMS di conferma
#define REPLY_ENABLED       true
```

L'APN Swisscom è già configurato in `src/modem.c`:

```c
modem_send_at("AT+CGDCONT=1,\"IP\",\"gprs.swisscom.ch\"", ...);
```

---

## Build e flash (VS Code)

### Tramite interfaccia grafica nRF Connect
1. Apri VS Code con l'estensione **nRF Connect for VS Code**
2. Click **"Add existing application"** → seleziona la cartella
3. Click **"Add build configuration"**
   - Board: `rak5010/nrf52840`
   - Lascia tutto il resto di default
4. Click **"Build"**
5. Collega il J-Link al connettore 4-pin del RAK5010
6. Click **"Flash"**

### Tramite terminale
```bash
# Dalla cartella del progetto
west build -b rak5010/nrf52840 .
west flash --softreset

# Monitor seriale (USB CDC su UART1)
west espressif monitor   # oppure
screen /dev/ttyACM0 115200
# Windows: usa PuTTY su COMx a 115200
```

---

## Comandi SMS supportati

Invia un SMS al numero della SIM inserita nel RAK5010:

| Testo SMS    | Effetto                          |
|--------------|----------------------------------|
| `OUT1 ON`    | Attiva Output 1 (P0.19) → HIGH   |
| `OUT1 OFF`   | Disattiva Output 1 → LOW         |
| `OUT2 ON`    | Attiva Output 2 (P0.20) → HIGH   |
| `OUT2 OFF`   | Disattiva Output 2 → LOW         |
| `OUT3 ON`    | Attiva Output 3 (P1.02) → HIGH   |
| `OUT3 OFF`   | Disattiva Output 3 → LOW         |
| `OUT4 ON`    | Attiva Output 4 (P1.01) → HIGH   |
| `OUT4 OFF`   | Disattiva Output 4 → LOW         |
| `ALL ON`     | Attiva tutti gli output           |
| `ALL OFF`    | Disattiva tutti gli output        |
| `STATUS`     | Risponde con lo stato di tutti    |

I comandi sono **case-insensitive** (es. `out1 on` = `OUT1 ON`).

---

## Comportamento LED verde

| Stato LED          | Significato                    |
|--------------------|--------------------------------|
| Acceso fisso       | Inizializzazione in corso      |
| Spento             | Sistema pronto                 |
| Lampeggio 100 ms   | Ciclo polling attivo (ogni 5s) |

---

## Note importanti

- Il **BG95-M3** non supporta GNSS e rete cellulare contemporaneamente.
  Questo firmware usa **solo la rete cellulare** (GNSS disabilitato).
- I GPIO del RAK5010-M lavorano a **1.8 V** — usa buffer/level shifter
  se devi pilotare carichi a 3.3 V o 5 V.
- La SIM deve supportare SMS (non solo dati).
- Il polling SMS avviene ogni **5 secondi**. Per reattività immediata
  è possibile estendere il codice con interrupt UART sui URC +CMTI.
