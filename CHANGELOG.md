# Changelog

Tutte le modifiche rilevanti al firmware sono documentate qui.
Formato basato su [Keep a Changelog](https://keepachangelog.com/it/1.1.0/),
versionamento secondo [Semantic Versioning](https://semver.org/lang/it/).

## [1.2.0] - 2026-07

### Aggiunto
- Monitoraggio **IN3** nel loop principale: segnala serbatoio di
  versamento vuoto, con debounce a 3 cicli consecutivi (~15s) per
  evitare falsi trigger da rimbalzi del galleggiante
  - Notifica SMS al numero NOTIFY quando IN3 si attiva
  - Con `autostart` abilitato e sistema a riposo: avvio automatico
    di generatore + pompa (`sequence_start_pompa`), rispettando le
    sicurezze IN1/IN2 già esistenti
  - Si riarma automaticamente quando IN3 torna a riposo

## [1.1.1] - 2026-07

### Corretto
- SMS in arrivo non più rilevati da `AT+CMGL` dopo un uso prolungato:
  causa probabile, memoria SMS della SIM (`"SM"`) esaurita (spesso solo
  10-20 messaggi di capacita'). La rete GSM mette in coda i messaggi
  quando la memoria e' piena, senza segnalare errore, e li consegna
  tutti insieme al primo spazio libero (es. dopo un riavvio che
  svuota lo storage) — comportamento coerente con quanto osservato.
- `sms_init()`: storage SMS spostato dalla SIM (`"SM"`) alla memoria
  interna del modem BG95 (`"ME"`), molto più capiente; svuotamento
  esplicito di `"SM"` all'avvio come misura precauzionale; aggiunto
  log diagnostico (`AT+CPMS?`) per monitorare l'occupazione dello
  storage nel tempo

## [1.1.0] - 2026-07

### Aggiunto
- Supporto galleggiante **IN2** (troppo-pieno sul serbatoio di versamento),
  simmetrico a IN1 (vuoto sul serbatoio di pescaggio), su tutti e 3 i
  percorsi di accensione pompa:
  - `START POMPA` da sistema a riposo (blocco preventivo)
  - accensione autonoma generatore+pompa (blocco preventivo + spegnimento durante il funzionamento)
  - aggancio pompa con generatore già acceso da `START` semplice (rifiuto aggancio, generatore resta acceso)
- `sms_replies.h`: tutti i testi dei messaggi SMS centralizzati in un unico file,
  invece di sparsi tra `main.c` e `sequence.c`
- Mutex (`modem_lock`/`modem_unlock`) a protezione dell'accesso condiviso
  alla UART del modem tra loop principale e thread della sequenza

### Corretto
- Bug: alcuni messaggi di stato pompa riportavano erroneamente "pieno"
  invece di "vuoto" (IN1) e viceversa
- Race condition sulla UART del modem che poteva causare blocchi del
  sistema quando più comandi SMS arrivavano ravvicinati nel tempo
- `modem_get_signal()`: ora distingue un segnale RSSI sconosciuto (99)
  da un valore 0 dBm reale

### Rimosso
- Numeri di telefono tecnici dal codice sorgente (spostati in
  `tech_config.h`, non tracciato da Git)
- Riga `VCC` (lettura batteria interna del modem, ridondante) dal
  messaggio di risposta `STATUS`
- Dichiarazione di `sequence_test_start()`, mai implementata

### Varie
- README riscritto per riflettere i comandi realmente supportati
- Rimosso codice morto/commentato in `main.c` e `sequence.c`

## [1.0.0] - baseline

Versione di partenza: comandi `START`, `START POMPA`, `STOP`, `STATUS`,
`CONFIG`, `SET`, `AUTOSTART`; autenticazione numeri cliente/tecnici;
autostart su batteria bassa; watchdog hardware.