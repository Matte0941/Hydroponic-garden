# Sistema di Irrigazione Automatica per Serra — Documentazione

## Panoramica

Questo firmware, pensato per una scheda Arduino (ATmega328-based, es. Uno/Nano), gestisce in autonomia l'irrigazione di una serra basandosi sulle condizioni ambientali reali (temperatura e umidità), regolando la quantità d'acqua tramite il calcolo del **VPD (Vapor Pressure Deficit — deficit di pressione di vapore)**, un indicatore molto più affidabile della sola umidità relativa per stimare lo stress idrico delle piante.

Il sistema è progettato per funzionare **senza supervisione continua**: salva tutto su scheda SD, si riprende da solo dopo un blocco grazie al watchdog hardware, e tollera guasti parziali dei sensori senza fermarsi.

---

## 1. Architettura generale

Il programma è organizzato attorno a un `loop()` non bloccante: nessuna funzione usa `delay()`, tutte le temporizzazioni si basano su confronti con `millis()`. Questo permette al sistema di controllare contemporaneamente più processi (lettura sensori, controllo pompa, logging, controllo SD) senza che uno blocchi gli altri.

I processi gestiti in parallelo sono:

| Processo | Intervallo | Funzione |
|---|---|---|
| Aggiornamento ambientale (temperatura/umidità/VPD) | ogni 30 minuti | `aggiornaAmbiente()` |
| Controllo presenza scheda SD | ogni 1 minuto | `controlloSD()` |
| Lettura livello acqua | ogni ~500 ms (10 campioni × 50 ms) | `controllaAcqua()` |
| Gestione accensione/spegnimento pompa | continuo (ogni ciclo di loop) | `gestionePompa()` |
| Scrittura log su SD | ogni 1 minuto | `salvaLog()` |
| Reset del watchdog | ogni ciclo di loop | `wdt_reset()` |

---

## 2. Calcolo del VPD e determinazione del tempo di irrigazione

Ogni 30 minuti il sistema:

1. Legge temperatura e umidità da **due sensori DHT11 ridondanti** (vedi sezione 3).
2. Calcola la pressione di vapore satura (SVP) con la formula di Tetens:
   
   `SVP = 0.6108 × e^(17.27 × T / (T + 237.3))`

3. Calcola il VPD:
   
   `VPD = SVP × (1 − umidità/100)`

4. In base al VPD, determina quanti minuti totali di irrigazione servono nella finestra di 30 minuti:
   - Se il VPD è sotto la soglia `VPD_START`, la pianta non è sotto stress idrico significativo → si usa il tempo minimo (`RUNTIME_MIN`, default 20 min).
   - Se il VPD supera la soglia, il tempo cresce in modo non lineare (esponente 1.4) proporzionalmente a quanto il VPD è superiore alla soglia, fino a un massimo (`RUNTIME_MAX`, default 60 min, **ma limitato internamente a un massimo di 30 min**, vedi sezione 6).

Questo tempo totale (`runtime`) viene poi **suddiviso in cicli da 10 minuti** distribuiti in modo uniforme nella finestra di 30 minuti, invece di essere erogato tutto in una volta: questo evita ristagni e favorisce l'assorbimento graduale da parte del substrato.

---

## 3. Ridondanza dei sensori di temperatura/umidità

Il sistema legge **due sensori DHT11 indipendenti** collegati ai pin 7 e 8, e gestisce quattro casistiche:

- **Entrambi funzionanti** → si usa la media dei due valori (più stabile e resistente a letture anomale singole).
- **Solo uno funzionante** → si usa il valore del sensore ancora attivo, e viene registrato un evento di errore (`DHT1_ERROR` o `DHT2_ERROR`) **solo al momento del guasto**, non ad ogni ciclo, per non intasare il log.
- **Entrambi guasti** → viene registrato un solo evento `DHT_BOTH_ERROR` (anche qui una sola volta) e il ciclo di aggiornamento ambientale viene interrotto, mantenendo gli ultimi valori validi noti.
- **Recupero** → quando un sensore guasto torna a funzionare, viene registrato un evento dedicato (`DHT1_RECOVERED` / `DHT2_RECOVERED`).

---

## 4. Gestione della pompa

La pompa (pin 6) viene attivata in base ai cicli pianificati (`plannedCycles`) calcolati a ogni aggiornamento ambientale.

Caratteristiche principali:

- **Distribuzione temporale**: i cicli non partono tutti insieme, ma sono spaziati uniformemente nella finestra di 30 minuti (`cycleInterval`), per garantire una bagnatura scaglionata.
- **Nessuna sovrapposizione**: il numero massimo di cicli pianificabili è limitato in modo che l'intervallo tra un ciclo e l'altro non sia mai inferiore alla durata massima di un ciclo (10 minuti) — altrimenti un ciclo tenterebbe di partire mentre il precedente è ancora in corso.
- **Ultimo ciclo proporzionale**: ogni ciclo dura al massimo 10 minuti, ma se il tempo di irrigazione rimanente (`remainingRuntime`) è inferiore, il ciclo viene accorciato di conseguenza, evitando di innaffiare più del necessario.
- **Blocco di sicurezza per livello acqua**: se il serbatoio è sotto la soglia minima, la pompa non parte (o viene spenta immediatamente se già in funzione), indipendentemente dai cicli pianificati.
- **Eventi tracciati**: ogni accensione e spegnimento della pompa genera un evento (`PUMP_ON` / `PUMP_OFF`) nel log eventi.

---

## 5. Controllo del livello dell'acqua

Il livello viene letto da un sensore analogico (pin A0) con un **filtro a media mobile**: 10 campioni raccolti a intervalli di 50 ms vengono mediati prima di prendere qualunque decisione, per evitare che un singolo valore rumoroso causi accensioni/spegnimenti indesiderati della pompa.

Sono previste tre soglie/stati:

- **Sensore scollegato o guasto** (valore quasi a zero): la pompa viene bloccata immediatamente e viene generato l'evento `WATER_SENSOR_ERROR`. Questo protegge da un possibile funzionamento a secco in caso di rottura del sensore.
- **Livello basso** (`WATER_STOP`): la pompa viene bloccata, evento `WATER_LOW`.
- **Livello ripristinato** (`WATER_OK`): la pompa viene sbloccata, evento `WATER_OK`.

L'uso di **due soglie distinte** (`WATER_STOP` più basso di `WATER_OK`) invece di una sola crea un'isteresi: evita che il sistema oscilli continuamente tra blocco/sblocco quando il livello dell'acqua è vicino al limite (ad esempio per il moto ondoso nel serbatoio).

---

## 6. Configurazione da file esterno

All'avvio, se la SD è presente, il sistema legge un file `config.txt` e sovrascrive i parametri di default se trova le seguenti righe (formato `CHIAVE=valore`):

```
VPD_START=1.0
RUNTIME_MIN=20
RUNTIME_MAX=60
WATER_STOP=250
WATER_OK=320
```

Questo permette di modificare la calibrazione del sistema semplicemente editando un file di testo sulla SD, senza dover ricaricare il firmware.

> **Nota**: indipendentemente dal valore di `RUNTIME_MAX` impostato, il tempo totale di irrigazione per ogni finestra di 30 minuti viene comunque limitato internamente a un massimo di 30 minuti, perché non è fisicamente possibile erogare più minuti di pompa di quanti la finestra stessa ne contenga senza generare sovrapposizioni tra i cicli.

---

## 7. Registrazione dati su SD

Vengono generati e mantenuti due file CSV distinti:

- **`serra.csv`** — log periodico (ogni minuto) dei valori ambientali e di stato:
  `DATE, TIME, TEMP, HUM, VPD, WATER, PUMP, RUNTIME, REMAINING`

- **`eventi.csv`** — log degli eventi discreti (accensioni/spegnimenti pompa, errori sensori, avvii di sistema, ecc.):
  `DATE, TIME, EVENT`

Ogni riga viene marcata con data e ora prelevate da un **modulo RTC DS1307**. Se l'RTC non è rilevato all'avvio, il sistema non si blocca: passa a una modalità di fallback in cui le righe vengono marcate con `NO_RTC` e i secondi trascorsi dall'accensione della scheda, così i dati restano comunque interpretabili anche senza orologio funzionante.

---

## 8. Resilienza e recupero automatico

Il firmware è pensato per funzionare per lunghi periodi senza intervento umano:

- **Watchdog hardware** (`wdt_enable(WDTO_8S)`): se il codice si blocca per qualunque motivo (loop infinito, blocco I2C/SPI, ecc.), la scheda si resetta automaticamente dopo 8 secondi di inattività del `wdt_reset()`. Al riavvio, il sistema riconosce che il reset è stato causato dal watchdog e lo registra come evento (`WATCHDOG_RESET`), utile per capire in fase di manutenzione se ci sono stati blocchi anomali.
- **Recupero automatico della SD**: se la scheda SD non viene rilevata all'avvio (magari perché non inserita), il sistema continua comunque a funzionare (semplicemente senza salvare nulla) e ritenta l'inizializzazione ogni minuto; appena la SD torna disponibile, i file vengono creati/ripristinati e viene registrato l'evento `SD_RECOVERED`.
- **Tolleranza ai guasti dei sensori**: come descritto, la perdita di uno o entrambi i sensori DHT11 non blocca il sistema, che continua a operare con i dati disponibili o con gli ultimi validi.
- **Blocco di sicurezza sull'acqua**: in nessuna circostanza la pompa può restare accesa se il livello dell'acqua è sotto soglia o il sensore risulta scollegato.

---

## 9. Riepilogo delle costanti principali

| Costante | Valore default | Significato |
|---|---|---|
| `ENV_INTERVAL` | 30 min | Intervallo tra un ricalcolo ambientale/irrigazione e il successivo |
| `LOG_INTERVAL` | 1 min | Frequenza di scrittura del log dati su SD |
| `PUMP_TIME` | 10 min | Durata massima di un singolo ciclo di pompa |
| `VPD_START` | 1.0 kPa | Soglia di VPD oltre la quale si aumenta l'irrigazione |
| `RUNTIME_MIN` | 20 min | Tempo minimo di irrigazione per finestra |
| `RUNTIME_MAX` | 60 min | Tempo massimo richiesto (poi limitato a 30 min, vedi sez. 6) |
| `WATER_STOP` | 250 | Soglia sotto la quale la pompa viene bloccata |
| `WATER_OK` | 320 | Soglia sopra la quale la pompa viene sbloccata |

---

## 10. In sintesi

Il sistema traduce condizioni ambientali reali in un piano di irrigazione dinamico e sicuro, distribuito nel tempo, con protezioni multiple contro i guasti (sensori, SD, RTC, blocchi software) e una tracciabilità completa di ogni evento e misura tramite log su SD — caratteristiche tipiche di un sistema pensato per funzionare "sul campo" per lunghi periodi senza supervisione.
