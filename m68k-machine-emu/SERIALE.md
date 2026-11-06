# Seriale: lo Z8530 SCC nell'harness

Canale **A** = console RS232. Canale **B** = ESP32 con ZiModem.
Il chip sta in `devices/z8530.c`; `harness.c` lo attacca al mondo esterno.

```
$E00001  B controllo      $E00005  A controllo
$E00003  B dati           $E00007  A dati
```
(A2 = A/B#, A1 = D/C#, A0 non entra nel decode.)

---

## 1. Quello che e' cambiato rispetto allo stub

Prima RR0 era la costante `$24`: Tx Buffer Empty cablato a 1, nessun tempo,
nessun interrupt, canale B inesistente. Adesso:

* **tempi veri al baud rate programmato**, sia in trasmissione sia in
  ricezione;
* **FIFO di ricezione da 3 byte** come il chip vero, con Rx Overrun;
* **interrupt** Rx / Tx / External-Status con latch IP e IUS, catena di
  priorita' interna e vettore modificato dallo stato;
* **canale B collegabile a un pty**, cosi' ci si puo' attaccare `pppd`,
  `picocom`, `socat` o quello che serve.

> **Le ROM che non programmano l'SCC continuano a funzionare.** Finche' il
> firmware non scrive nessun registro di controllo del canale, il modello
> resta passante come prima: trasmissione istantanea e Tx Buffer Empty
> sempre a 1. Al primo WR scritto entra in funzione il modello completo, e
> l'harness lo dice con una riga su stderr.

---

## 2. Baud rate

```
baud = clock / (2 * (TC + 2) * clockmode)      quando la sorgente e' il BRG
baud = clock / clockmode                       quando e' RTxC o TRxC
```

| registro | cosa decide |
|---|---|
| WR4 D7-D6 | clockmode: x1, x16, x32, x64 |
| WR4 D3-D2 | stop bit: 1, 1.5, 2 (00 = sync, non modellato) |
| WR4 D0 | parita' abilitata (aggiunge un bit alla durata) |
| WR3 D7-D6 | bit per carattere in ricezione |
| WR5 D6-D5 | bit per carattere in trasmissione |
| WR5 D3 | Tx Enable: se e' 0 i byte scritti si perdono |
| WR11 D4-D3 | sorgente del clock di trasmissione (10 = BRG) |
| WR11 D6-D5 | sorgente del clock di ricezione |
| WR12/WR13 | time constant del BRG, byte basso e alto |
| WR14 D0 | BRG acceso |
| WR14 D1 | sorgente del BRG: PCLK o RTxC |

Il clock di riferimento e' `--scc-clock`, default **3686400 Hz**, il
cristallo classico da baud rate. Sulla scheda PCLK e RTxC arrivano dallo
stesso quarzo, quindi nel modello valgono uguale.

Con quel clock, `x16`:

| time constant (WR12) | baud |
|---|---|
| 10 | 9600 |
| 4 | 19200 |
| 1 | 57600 |
| 0 | 115200 |

**8N1 a 9600 baud = 10 bit per carattere = 10416 cicli** del 68010 a
10 MHz, cioe' poco piu' di un millisecondo. E' il numero da tenere in testa
quando si decide se una routine puo' permettersi di aspettare.

---

## 3. RR0 e RR1: cosa ci si trova adesso

| bit | RR0 | nel modello |
|---|---|---|
| D0 | Rx Character Available | c'e' almeno un byte nella FIFO del chip |
| D1 | Zero Count | sempre 0 (il conteggio del BRG non e' modellato) |
| D2 | **Tx Buffer Empty** | **vero**: 0 quando il buffer e' pieno |
| D3 | DCD | 1 sul canale B quando `--scc-b` ha un capo collegato |
| D4 | Sync/Hunt | sempre 0 (niente modo sincrono) |
| D5 | CTS | sempre 1 (niente controllo di flusso hardware sulla scheda) |
| D6 | Tx Underrun/EOM | 1 quando il trasmettitore e' del tutto fermo |
| D7 | Break/Abort | sempre 0 (nessun break viene generato) |

RR1: D0 **All Sent** (buffer e shift register vuoti), D5 **Rx Overrun**
quando arriva un quarto byte con la FIFO piena. Parity error (D4) e
framing error (D6) restano 0: sulla linea simulata non c'e' rumore.

Quando un interrupt External/Status e' latchato, **RR0 resta congelata** sul
valore del momento finche' non si manda il comando Reset Ext/Status
Interrupts, esattamente come sul chip vero.

---

## 4. Trasmissione: il doppio buffer

Il chip ha un buffer piu' uno shift register, e il modello li tiene
separati. Conseguenza pratica:

* la **prima** scrittura a trasmettitore fermo non blocca niente: il byte
  passa subito allo shift register e D2 resta a 1;
* la **seconda** scrittura entro il tempo di carattere riempie il buffer e
  **D2 va a 0**;
* D2 torna a 1 quando il buffer si svuota nello shift register.

Quindi un firmware che scrive senza mai guardare D2 funziona per il primo
carattere e poi comincia a perderne. L'harness lo dice:

```
[SCC A] scrittura su buffer pieno: byte 0x48 perso (manca il polling di RR0 D2)
```

e alla fine conta i byte persi. La routine giusta e' in
`test/ssdlib.s::_putc`:

```asm
_putc:
.wait:
    btst    #2,SCC_A_CTRL       ; RR0 D2 = Tx Buffer Empty
    beq.s   .wait
    move.b  d0,SCC_A_DATA
    rts
```

---

## 5. Ricezione: FIFO da 3 byte

Fra l'host e il chip c'e' una coda di transito che fa da **filo**: i byte
entrano nella FIFO uno ogni tempo di carattere, non tutti insieme. La FIFO
e' profonda **3 byte** come quella vera; il quarto byte sovrascrive
l'ultimo e alza Rx Overrun in RR1 D5.

A 9600 baud con un 68010 a 10 MHz hai circa 10.000 cicli per svuotare la
FIFO fra un byte e l'altro, quindi l'overrun non e' un problema. A 115200
scendono a 868: li' il polling non basta piu' e servono gli interrupt.

---

## 6. Interrupt

### Cablaggio

```
--scc-irq LIV      /INT dell'SCC su IRQ autovector di livello LIV
--scc-vectored     il chip mette il vettore sul bus nel ciclo di IACK
```

Senza `--scc-vectored` l'harness risponde con l'autovector e il gestore
capisce da solo chi ha chiamato, leggendo **RR3** (solo sul canale A) o
**RR2** (sul canale B, dove torna il vettore con lo stato). Con
`--scc-vectored` il vettore lo decide il chip e la CPU salta direttamente
al gestore giusto: serve che la tabella dei vettori copra `WR2` e i sette
indirizzi successivi.

La mappa di memoria non dice come e' cablato /INT sulla scheda, quindi
sono supportate entrambe le cose. Se il timer e l'SCC condividono lo stesso
livello, nel ciclo di riconoscimento vince l'SCC.

### Registri

| | |
|---|---|
| WR1 D0 | Ext Int Enable |
| WR1 D1 | Tx Int Enable |
| WR1 D4-D3 | 00 Rx int spento, 01 sul primo carattere, 10 su tutti, 11 solo condizioni speciali |
| WR2 | vettore base (condiviso fra i canali) |
| WR9 D0 | VIS: lo stato entra nel vettore |
| WR9 D1 | NV: il chip non pilota il vettore |
| WR9 D3 | MIE: Master Interrupt Enable |
| WR9 D4 | lo stato va nei bit alti invece che bassi |
| WR9 D7-D6 | 01 reset canale B, 10 reset canale A, 11 reset hardware |

Comandi in WR0 (bit 5-3): `010` Reset Ext/Status Int, `100` Enable Int on
Next Rx Character, `101` Reset Tx Int Pending, `110` Error Reset, `111`
**Reset Highest IUS** (da mandare in fondo al gestore, o le sorgenti di
priorita' piu' bassa restano bloccate).

### Priorita' e codici di stato

Dalla piu' alta: A Rx, A Tx, A Ext, B Rx, B Tx, B Ext. Con VIS attivo i
bit 3-1 del vettore diventano:

| stato | sorgente | | stato | sorgente |
|---|---|---|---|---|
| 0 | B Tx Buffer Empty | | 4 | A Tx Buffer Empty |
| 1 | B Ext/Status | | 5 | A Ext/Status |
| 2 | B Rx Char Available | | 6 | A Rx Char Available |
| 3 | B Special Rx | | 7 | A Special Rx |

Quindi con `WR2 = $40` un interrupt di ricezione sul canale B da' `$44`.

### Due cose che sorprendono (ma sono giuste)

**Il Tx IP si rialza subito dopo la scrittura.** Scrivi un byte, quello
passa allo shift register, il buffer e' di nuovo vuoto e il chip richiede
il byte successivo. E' cosi' che si tiene il flusso: si continua finche' si
ha roba da mandare, e quando non ce n'e' piu' si chiude con **Reset Tx Int
Pending**, altrimenti /INT resta basso.

**Abilitare il Tx Int a buffer vuoto genera subito una richiesta.** E' il
modo normale di far partire una trasmissione a interrupt: si riempie la
coda, si mette a 1 WR1 D1 e il gestore parte da solo.

---

## 7. Il canale B verso l'host

```sh
--scc-b pty           crea una coppia e stampa il nome dello slave
--scc-b /dev/ttyUSB0  oppure un device vero, un pty esistente, una fifo
```

Con `pty` l'harness stampa:

```
[SCC B] canale B collegato a /dev/pts/7
```

e da li' in poi ci si attacca quello che si vuole:

```sh
picocom -b 9600 /dev/pts/7          # per parlare a mano
sudo pppd /dev/pts/7 9600 noauth    # per il PPP vero
```

Lo slave del pty resta aperto dentro l'harness per tutta la sua vita: se lo
chiudesse, alla disconnessione del programma host il master comincerebbe a
dare EIO. DCD sul canale B va a 1 quando `--scc-b` e' attivo.

---

## 8. Esempio: ricezione a interrupt

`test/serial.s` e' la forma minima di quello che serve sotto il PPP: i byte
dal canale B arrivano per interrupt e finiscono in una coda circolare, il
programma principale li riprende con comodo e li rimanda indietro
aspettando Tx Buffer Empty.

```sh
make roms
./m68k-harness test/serial.bin -n -f --scc-irq 4 --scc-b pty
# da un altro terminale:
picocom -b 9600 /dev/pts/N
```

Funziona sia in autovector sia con `--scc-vectored`: la tabella dei vettori
della ROM copre l'autovector 4 e gli otto indirizzi `$40..$47`.

---

## 9. Verifica

`make check` esegue due controlli sul seriale, tutti e due senza bisogno di
hardware:

* **`--scc-selftest`** prova il chip da solo, a tempo controllato: baud rate
  con TC=10 (9600) e durata del carattere, Tx Buffer Empty che si azzera
  solo alla seconda scrittura e torna dopo esattamente 10416 cicli, All
  Sent, ricezione ritardata di un tempo di carattere, overrun con 5 byte in
  una FIFO da 3, interrupt di ricezione con RR3 e RR2, vettore `$44`
  nell'IACK, IUS che blocca le sorgenti sotto, Reset Highest IUS, flusso di
  trasmissione a interrupt, un canale a polling che non deve generare
  richieste, priorita' del canale A sul B, e il comportamento passante a
  chip non programmato.
* **`test/scc-pty-test.py`** fa il giro completo: lancia l'harness con
  `--scc-b pty`, si attacca al pty, manda 19 byte e controlla che tornino
  identici. Il percorso tocca pty, coda di transito, FIFO del chip,
  interrupt di livello 4, gestore nella ROM e trasmissione a polling.

---

## 10. Cosa non e' modellato

Modo sincrono (SDLC/HDLC, CRC, DPLL, loop mode), DMA e le richieste di
wait, parity ed errori di framing (la linea simulata non ha rumore), il
conteggio Zero Count del BRG, il controllo di flusso hardware (CTS resta
sempre alto), il baud rate in ricezione diverso da quello in trasmissione
se la sorgente di clock e' esterna. Il canale A ignora DCD.
