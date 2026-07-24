# Changelog

Tutte le modifiche rilevanti al firmware sono documentate qui.
Formato basato su [Keep a Changelog](https://keepachangelog.com/it/1.1.0/),
versionamento secondo [Semantic Versioning](https://semver.org/lang/it/).

## [1.5.0] - 2026-07

### Aggiunto
- Flag persistenti `sens_pieno_installato` (IN2) e `sens_vuoto_installato`
  (IN3)...
- Nuovi comandi SMS tecnici: `SENS PIENO ON/OFF`, `SENS VUOTO ON/OFF`
- `CONFIG` ora mostra anche `SensPieno`/`SensVuoto`

### Modificato
- **`STATUS` ridisegnato** per il cliente finale...

## [1.4.1] - 2026-07

### Corretto
- `plant_status.c` classificava erroneamente come ANOMALIA la finestra
  transitoria (fino a ~1s) tra uno STOP ricevuto (uscite gia' spente
  da `sequence_stop()`) e l'aggiornamento di `state` a `SEQ_IDLE` da
  parte del thread della sequenza. Aggiunto `sequence_is_stop_pending()`
  per riconoscere questo caso e classificarlo come ATTENZIONE
  ("spegnimento in corso") anziche' ANOMALIA.

## [1.4.0] - 2026-07

### Aggiunto
- Nuovo modulo `plant_status.c/h`: classifica lo stato complessivo
  dell'impianto (sequenza + IN0-3 + OUT0-2) in OK/ATTENZIONE/ANOMALIA/
  IMPOSSIBILE con descrizione testuale, secondo le regole di
  `STATI_IMPIANTO.md`
- `sequence_state_name()`: nome testuale dello stato sequenza

### Modificato
- Loop principale: i log separati `VEXT`/`EXP_IN`/`EXP_OUT` sostituiti
  da un'unica riga per ciclo (`Seq=... VEXT=... IN=... OUT=... | LIVELLO:
  descrizione`), con livello di log (`INF`/`WRN`/`ERR`) allineato alla
  gravità rilevata

## [1.3.0] - 2026-07

### Aggiunto
- Rilevamento perdita feedback **IN0** (generatore fermo
  inaspettatamente) durante `GEN_OK`/`POMPA_ON`, nei 4 punti in cui il
  firmware attende con il generatore acceso: attesa T5 (solo
  generatore), attesa T4 prima della pompa (entrambi i percorsi
  `START` e `START POMPA`), e durante il funzionamento pompa
  (`attendi_spegnimento`). Spegnimento sicuro + SMS di allerta
  (`ATTENZIONE: ...`) distinto dai normali messaggi di fine sequenza
- `STATI_IMPIANTO.md`: tabella di riferimento con tutte le
  combinazioni sequenza/ingressi/uscite significative (stati normali,
  operativi, e incoerenze SW/HW)

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