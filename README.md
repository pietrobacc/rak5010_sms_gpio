# RAK5010-M + BG95-M3 — SMS → GPIO Controller

Firmware Zephyr per **nRF Connect SDK v3.2.3** che gestisce l'accensione
di un generatore e di una pompa tramite comandi SMS, con feedback GPIO
tramite espansore MCP23017 e monitoraggio di batteria/sensori ambientali.

---

## Struttura progetto

```
rak5010_sms_gpio/
├── CMakeLists.txt          — definizione target e sorgenti
├── prj.conf                — Kconfig (driver abilitati)
├── app.overlay             — device tree (pin reali RAK5010-M)
└── src/
    ├── main.c               — loop principale + dispatcher comandi SMS
    ├── gpio_ctrl.c/h        — LED, PWRKEY BG95, output/input MCP23017
    ├── modem.c/h            — comunicazione AT via UART0
    ├── sms.c/h              — invio/ricezione/parsing SMS
    ├── sequence.c/h         — macchina a stati sequenza generatore/pompa
    ├── auth.c/h             — numeri autorizzati e privilegi
    ├── wdt.c/h              — watchdog hardware
    └── tech_config.h        — numeri tecnici (NON committato, vedi sotto)
```

---

## Configurazione prima della compilazione

### Numeri tecnici (privilegio completo)

I numeri con privilegio "tech" (accesso a tutti i comandi, incluso `SET`
e `AUTOSTART`) **non sono nel codice sorgente** per motivi di privacy.

```bash
cp src/tech_config.h.example src/tech_config.h
```

Poi modifica `src/tech_config.h` inserendo i numeri reali in formato
internazionale. Questo file è in `.gitignore` e non verrà mai committato.

### Altri parametri

- `REPLY_ENABLED` in `src/main.c`: abilita/disabilita le risposte SMS.
- APN Swisscom già configurato in `src/modem.c` (`AT+CGDCONT`).

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
west build -b rak5010/nrf52840 .
west flash --softreset

# Monitor seriale (USB CDC su UART1)
west espressif monitor   # oppure
screen /dev/ttyACM0 115200
# Windows: usa PuTTY su COMx a 115200
```

---

## Comandi SMS supportati

### Numeri cliente (numeri configurati in NUM1/2/3)

| Comando        | Effetto                                             |
|----------------|------------------------------------------------------|
| `START`        | Avvia il generatore                                  |
| `START POMPA`  | Avvia generatore + pompa (o solo pompa se già acceso) |
| `STOP`         | Interrompe la sequenza in corso, spegne tutto        |
| `STATUS`       | Risponde con temperatura, batteria, segnale, stato   |

### Numeri tecnici (privilegio completo, in aggiunta ai precedenti)

| Comando                 | Effetto                                             |
|---------------------------|--------------------------------------------------|
| `CONFIG`                 | Risponde con i parametri T1-T6, S1 e autostart      |
| `SET T1/T2/T3/T4 <sec>`  | Imposta un timer in secondi (1-300) e salva in NVS  |
| `SET T5/T6 <min>`        | Imposta un timer in minuti (0-1440) e salva in NVS  |
| `SET S1 <V*10>`          | Soglia batteria bassa, es. `SET S1 125` = 12.5 V    |
| `SET NUM1/2/3 <numero>`  | Imposta/cancella un numero cliente                  |
| `SET NOTIFY <1-3>`       | Sceglie quale NUMx riceve le notifiche automatiche  |
| `AUTOSTART ON/OFF`       | Abilita/disabilita avvio automatico su batteria bassa |

I comandi sono **case-insensitive** (es. `start` = `START`).

---

## Comportamento LED verde

| Stato                          | Lampeggi ogni ciclo (5s) |
|---------------------------------|--------------------------|
| Generatore acceso manualmente   | 6                        |
| Pompa accesa                    | 5                        |
| Generatore acceso (SEQ_GEN_OK)  | 4                        |
| Sequenza in esecuzione          | 3                        |
| Batteria sotto soglia S1        | 2                        |
| Normale / idle                  | 1                        |

---

## Note importanti

- Il **BG95-M3** non supporta GNSS e rete cellulare contemporaneamente.
  Questo firmware usa **solo la rete cellulare** (GNSS disabilitato).
- I GPIO del RAK5010-M lavorano a **1.8 V** — usa buffer/level shifter
  se devi pilotare carichi a 3.3 V o 5 V.
- La SIM deve supportare SMS (non solo dati).
- Il polling SMS avviene ogni **5 secondi** nel loop principale.
- Un watchdog hardware (30s) resetta il sistema in caso di blocco;
  al riavvio viene inviato un SMS di allerta al numero di notifica.
