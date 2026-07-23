# Stati dell'impianto — tabella di riferimento

Questo documento elenca le combinazioni significative di **sequenza +
ingressi + uscite** e cosa rappresentano per l'impianto nel suo insieme.
È pensato come riferimento per la diagnosi (via log RTT o debug).

> **Aggiornamento v1.3.0**: il rilevamento della perdita di IN0
> durante `GEN_OK`/`POMPA_ON` (generatore fermo inaspettatamente) è
> stato implementato — vedi sezione 2, non più una lacuna aperta.

---

## Legenda segnali

| Segnale | Significato | Dove viene gestito |
|---|---|---|
| **Sequenza** | `IDLE` / `RUNNING` / `GEN_OK` / `POMPA_ON` | `sequence_get_state()` |
| **IN0** | Generatore acceso **manualmente** (feedback fisico) | usato per bloccare/permettere START; conferma accensione durante la sequenza; monitorato in continuo durante `GEN_OK`/`POMPA_ON` (v1.3.0) |
| **IN1** | Serbatoio di **pescaggio** vuoto (sorgente) | ferma/blocca la pompa per sicurezza (non può aspirare a vuoto) |
| **IN2** | Serbatoio di **versamento** pieno (destinazione) | ferma/blocca la pompa (eviterebbe fuoriuscite) |
| **IN3** | Serbatoio di **versamento** vuoto (destinazione) | notifica, ed eventualmente autostart generatore+pompa |
| **OUT0** | Relè comando generatore | pilotato solo dalla sequenza (`RUNNING`→`GEN_OK`) |
| **OUT1** | Impulso avviamento (choke/starter) | attivo solo durante T2, dentro `RUNNING` |
| **OUT2** | Relè comando pompa | pilotato solo durante `POMPA_ON` |

**Nota fisica importante**: IN2 e IN3 sono due galleggianti sullo
**stesso serbatoio di versamento** (livello alto e livello basso). Non
possono essere entrambi a 1 contemporaneamente — se succede, è un
problema di sensore/cablaggio, non una condizione reale dell'impianto.

---

## 1. Stati normali (✅ OK)

| Sequenza | IN0 | OUT0 | OUT2 | IN1 | IN2 | IN3 | Descrizione |
|---|---|---|---|---|---|---|---|
| IDLE | 0 | 0 | 0 | 0 | 0 | 0 | Impianto fermo, tutto a riposo |
| IDLE | 1 | 0 | 0 | — | — | — | Generatore acceso a mano, sequenza non coinvolta |
| RUNNING | 0→1 | 1 | 0 | — | — | — | Accensione generatore in corso (T1/T2/T3) |
| GEN_OK | 1 | 1 | 0 | — | — | — | Generatore acceso (da sequenza), pompa non richiesta |
| POMPA_ON | 1 | 1 | 1 | 0 | 0 | — | Generatore+pompa avviati insieme, tutto normale |
| POMPA_ON | 1 | 0 | 1 | 0 | 0 | — | Pompa avviata su generatore già acceso **a mano** (firmware non tocca OUT0) |

---

## 2. Stati operativi da segnalare (⚠️ ATTENZIONE — non guasti)

| Condizione | Descrizione |
|---|---|
| IN1 = 1 (in qualsiasi stato) | Pescaggio vuoto: pompa bloccata o appena fermata per sicurezza |
| IN2 = 1 (in qualsiasi stato) | Versamento pieno: pompa bloccata o appena fermata |
| IN3 = 1, sequenza IDLE, autostart OFF | Versamento vuoto, in attesa di intervento manuale (`START POMPA`) |
| IN3 = 1, sequenza IDLE, autostart ON | Versamento vuoto → dovrebbe già essere partito l'avvio automatico |
| IN0 = 1 durante `RUNNING`, prima del previsto | Generatore partito più veloce del timeout T2 atteso (non un problema, solo un timing) |
| **IN0 = 0** durante `GEN_OK`/`POMPA_ON` (qualsiasi punto) | Generatore fermo inaspettatamente (feedback perso) — dalla v1.3.0 rilevato attivamente in tutti e 4 i punti di attesa (T5, T4×2, `attendi_spegnimento`): spegnimento sicuro + SMS `ATTENZIONE: ...` |

---

## 3. Incoerenze da indagare (🔴 ANOMALIA — SW/HW non allineati)

Queste combinazioni **non dovrebbero mai verificarsi** secondo la
logica firmware attuale. Se le vedi nel log, indicano un relè
bloccato, un sensore rotto, o uno stato perso (es. dopo un reset non
pulito).

| Condizione | Cosa significherebbe |
|---|---|
| Sequenza IDLE, ma **OUT0 = 1** | Relè generatore bloccato acceso, oppure stato software non sincronizzato con l'hardware |
| Sequenza IDLE, ma **OUT2 = 1** | Relè pompa bloccato acceso — nessuna sequenza lo sta comandando |
| Sequenza `RUNNING`, ma **OUT2 = 1** | La pompa si è attivata prima ancora che il generatore fosse confermato acceso |
| **OUT1 = 1** fuori da `RUNNING` | Impulso di avviamento rimasto attivo oltre il previsto (relè bloccato) |
| **IN2 = 1 e IN3 = 1** insieme | Fisicamente impossibile sullo stesso serbatoio → galleggiante/cablaggio da controllare |

### Nota storica

Fino alla v1.2.0, la combinazione *"sequenza `GEN_OK`/`POMPA_ON` ma
IN0=0"* era la lacuna principale di questa tabella: il firmware non
monitorava IN0 una volta raggiunto `GEN_OK`, quindi un generatore
fermatosi da solo (es. carburante finito) sarebbe passato inosservato
fino alla prossima `STATUS` manuale. Dalla **v1.3.0** questo caso è
rilevato attivamente — vedi sezione 2.

---

## Come si potrebbe usare in futuro

Se in seguito vorrai renderlo operativo (non richiesto ora), i mattoni
sono già tutti presenti — basterebbe una funzione che legge sequenza +
IN0-3 + OUT0/OUT2 e restituisce un'etichetta sintetica (`OK` /
`ATTENZIONE: ...` / `ANOMALIA: ...`), da loggare o aggiungere alla
risposta `STATUS`.
