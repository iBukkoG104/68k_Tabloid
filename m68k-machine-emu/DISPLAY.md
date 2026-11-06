# Pannello e touch nell'harness M68K

Simulazione del display del PDA: **SSD1963** che pilota un TFT 7" 800x480
RGB565 su bus parallelo 8080 a 16 bit, e **XPT2046** per il touch resistivo.
La finestra e' SDL2; mouse e tastiera dell'host diventano dito sul vetro e
console seriale.

```
devices/ssd1963.c   il chip: frame buffer, command set, finestre, scroll
devices/xpt2046.c   il touch: SPI modellato bit per bit
devices/panel.c     la finestra SDL2 (presentazione, mouse, tastiera)
harness.c           decode del bus, IRQ, opzioni
```

> Per la seriale (Z8530 SCC: baud rate, FIFO, interrupt, canale B su pty)
> vedi **[SERIALE.md](SERIALE.md)**.

`ssd1963.c` e `xpt2046.c` non dipendono da SDL: con `make NOGUI=1`, o se a
runtime non c'e' un display, l'emulazione dei due chip resta attiva e si
lavora headless (vedi `-S` e `--touch-script`).

### In due minuti

```sh
make                      # harness + finestra (SDL2 rilevata con pkg-config)
make roms                 # ROM di prova (servono vasmm68k_mot e python3)
make check                # suite di regressione, tutta headless

./m68k-harness test/touch.bin           # demo interattiva: disegna col mouse
./m68k-harness test/display.bin         # barre di colore e sfumatura
./m68k-harness test/features.bin -n -f -S out.ppm   # senza finestra
```

### Due cose da confrontare con lo schema elettrico

La mappa di memoria lascia aperti due punti; il modello ha scelto
l'interpretazione piu' probabile, ma se il ferro va diversamente si cambia
qui prima di scriverci sopra il BIOS.

1. **L'SSD1963 e' trattato come periferica a 16 bit** (D0-D15 della CPU sui
   D0-D15 del chip). E' l'unico modo di avere un pixel RGB565 per ciclo di
   bus, e combacia con gli indirizzi `$e00201` / `$e00202` della mappa. Vedi
   il paragrafo 1.
2. **Il clocking della porta SPI del touch** non e' deducibile dalla mappa.
   Il default (`-k rw`) e' "ogni accesso vale 8 impulsi di clock"; l'altra
   variante (`-k w`) e' lo shift register con latch. Vedi il paragrafo 1.

---

## 1. Il contratto hardware che il modello assume

La mappa (`memory map.txt`) dice:

```
SSD1963    %111xxxxx xxx00010 xxxxxx[D/c]1     $e00201 - $e00202
XPT2046    %111xxxxx xxx00011 xxxxxxx1         $e00301
```

Da cui l'harness deriva questo, che e' bene tenere presente quando si
disegna lo schema:

### SSD1963: periferica a **16 bit**

Il display e', con il registro Data della IDE, l'unica periferica con
**D0-D15 della CPU collegati direttamente a D0-D15 del chip**. Non puo'
essere altrimenti: un pannello RGB565 a 16 bit in modo 8080 vuole un pixel
per ciclo di bus.

| indirizzo | A1 (D/C#) | accesso | significato |
|---|---|---|---|
| `$E00201` | 0 | `.B` (LDS) | byte di comando |
| `$E00203` | 1 | `.B` (LDS) | byte di parametro |
| `$E00202` | 1 | `.W` | dato a 16 bit: un pixel RGB565 |

Comandi e parametri viaggiano su **D7-D0**: e' quello che fa l'SSD1963 vero
in modo 16 bit, quindi la `.B` all'indirizzo dispari e' corretta ed e' il
modo piu' naturale di scriverli dal 68010.

Tre cose da sapere:

* **`.L` sulla porta dati non funziona** e il modello lo dice con un
  warning. Un `move.l` genera due cicli di bus, a `$E00202` e `$E00204`:
  nel secondo A1 e' tornato a 0, quindi il byte finisce sulla porta
  comandi. Per riempire in fretta si usa `move.w Dn,(An)` con An fisso a
  `$E00202` (vedi `_fill` in `test/ssdlib.s`).
* Una `.B` a indirizzo **pari** (UDS) e' elettricamente dubbia: sul ferro il
  byte uscirebbe su D15-D8. Il modello l'accetta su D7-D0 ma avvisa una
  volta.
* Tutta la pagina `$E00200-$E002FF` e' il display: A0 e A2..A7 sono
  "don't care", conta solo A1.

### XPT2046: porta SPI a 8 bit

Il controller e' SPI, ma sul bus del 68010 si affaccia come una porta a 8
bit in `$E00300-$E003FF` (A0 fisso a 1, quindi `$E00301`). Il glue logic
deve generare 8 impulsi di clock e allungare il DTACK. Il modello offre due
varianti, perche' entrambe sono realizzabili:

| `-k` | comportamento |
|---|---|
| `rw` (default) | **ogni** accesso, lettura o scrittura, vale un trasferimento full duplex a 8 bit. In lettura MOSI resta a 0 per non generare un bit di start spurio. |
| `w` | clockano solo le scritture; la lettura restituisce il byte entrato durante l'ultima scrittura (shift register + latch). |

Con il default una conversione costa tre cicli di bus:

```asm
    move.b  #$d0,(a2)       ; byte di controllo: canale X
    move.b  (a2),d0         ; BUSY + D11..D5
    move.b  (a2),d1         ; D4..D0 + tre zeri
    ; valore = ((d0<<8)|d1) >> 3
```

---

## 2. Cosa e' modellato dell'SSD1963

**Frame buffer** RGB565 interno, indirizzato tramite `set_column_address`
(`$2A`) e `set_page_address` (`$2B`) con wrap dentro la finestra, scritto da
`write_memory_start` / `_continue` (`$2C` / `$3C`) e riletto da
`read_memory_start` / `_continue` (`$2E` / `$3E`). All'accensione contiene
rumore, come la DRAM vera: se il firmware non pulisce lo schermo si vede.

**`set_address_mode` (`$36`)**: scambio pagina/colonna (bit 5), specchio
sulle colonne (bit 6) e sulle pagine (bit 7), ordine RGB/BGR (bit 3).
Applicati in scrittura, come sul chip.

**`set_pixel_data_interface` (`$F0`)**: `03` = 16 bit 565 (quello del nostro
pannello, ed e' il default dopo il reset), `00` = 8 bit 888 su tre
trasferimenti, `01` = 12 bit 444 impaccato, `02` = 16 bit impaccato (tre
word = due pixel), `05` = 24 bit. `04` (18 bit) e `06` (9 bit) non stanno su
un bus a 16 bit: il modello avvisa e li tratta come 565.

**`set_lcd_mode` (`$B0`)** ridimensiona davvero il pannello: HDP+1 colonne e
VDP+1 righe, e la finestra SDL segue. Prima dell'init il modello parte a
800x480, cosi' si puo' provare anche senza sequenza completa.

**Stati del display**: `set_display_on/off` (`$29`/`$28`), sleep
(`$10`/`$11`), invert (`$21`/`$20`), idle mode (`$39`/`$38`, riduce a 8
colori), partial mode.

**Scroll verticale hardware**: `set_scroll_area` (`$33`, TFA/VSA/BFA) e
`set_scroll_start` (`$37`, VSP). Le aree fisse in alto e in basso restano
ferme e la banda centrale scorre, senza toccare il frame buffer. Se
TFA+VSA+BFA non fa l'altezza del pannello arriva un warning e lo scroll
viene ignorato, come sul chip.

**`set_pwm_conf` (`$BE`)**: il duty (parametro 1) e il bit di abilitazione
(parametro 2) diventano la luminosita' della finestra. Duty 0 o PWM spento =
schermo nero.

**Letture**: `read_ddb` (`$A1`) restituisce l'ID vero dell'SSD1963
(`01 57 61 01 FF`), e i `get_*` restituiscono quello che e' stato scritto
con i rispettivi `set_*` (`$0A`, `$0B`, `$0D`, `$0E`, `$B1`, `$B5`, `$B7`,
`$B9`, `$BB`, `$BD`, `$BF`, `$D1`, `$D5`, `$E4`, `$E5`, `$E7`, `$F1`).

**Accettati, memorizzati e rileggibili ma senza effetto visivo**: PLL
(`$E0`, `$E2`), pixel clock (`$E6`), periodi di sincronismo (`$B4`, `$B6`),
GPIO (`$B8`, `$BA`), post-process (`$BC`), DBC (`$D0`, `$D4`), generatori
LCD (`$C0`-`$CF`), gamma (`$26`). Servono: la macchina a stati dei parametri
resta allineata, quindi un init reale scorre senza sballare i comandi che
contano. Un comando sconosciuto viene segnalato e ignorato.

**Non modellato**: temporizzazioni reali (un comando e' istantaneo), tearing
effect sul pin TE (`$34`/`$35` cambiano solo lo stato leggibile), DBC
dinamico, gamma, dithering/FRC.

### Comandi dell'SSD1963 usati nell'init di esempio

`test/ssdlib.s::_ssd_init` fa la sequenza tipica: soft reset, PLL
(`$E2`/`$E0`), reset, pixel clock (`$E6`), `set_lcd_mode` 800x480,
`set_hori_period`, `set_vert_period`, `set_address_mode`,
`set_pixel_data_interface` 565, `set_pwm_conf`, `set_display_on`.
E' una base buona da copiare nel BIOS.

---

## 3. Cosa e' modellato dell'XPT2046

L'SPI e' simulato **bit per bit**, non a byte, quindi funziona con tutte le
sequenze che usano i driver reali, comprese quelle a pipeline dove il byte
di controllo successivo viaggia insieme ai dati della conversione in corso.

Il byte di controllo e' `S A2 A1 A0 MODE SER/DFR# PD1 PD0`. Dopo gli 8 bit
il chip accoda in uscita 1 bit di BUSY + 12 bit di dato (MSB per primo) + 3
zeri: i 16 bit che i driver leggono e scalano di 3.

| controllo | canale |
|---|---|
| `$90` | Y |
| `$D0` | X |
| `$B0` | Z1 (pressione) |
| `$C0` | Z2 |
| `$92` | VBAT |
| `$84` | AUX |
| `$80`/`$F0` | temperatura |

Con `MODE=1` (bit 3, p.es. `$98`) la conversione e' a 8 bit.

**/PENIRQ**: abilitato solo con PD1=PD0=0 (cioe' `$90`, `$D0`, `$B0`), come
sul chip vero. Resta asserito mentre il dito e' appoggiato e si rialza
durante una conversione. Con `-p LIV` viene cablato su un IRQ autovector di
livello LIV; senza, si fa polling di Z1. E' una linea di livello: se il
gestore non legge il touch, l'interrupt si ripresenta - esattamente come sul
ferro.

**Il pannello resistivo** ha fondo scala 200..3900 su entrambi gli assi,
rumore di +/-3 LSB e pressione Z1 ~420 / Z2 ~3550 quando premuto, Z1 0 /
Z2 4095 da fermo. Tutto configurabile (`-X`, `-N`).

> Un artefatto reale che il modello riproduce: X e Y sono due conversioni
> distinte, quindi se il dito si alza o si sposta fra le due esce un
> campione mezzo buono. Il rimedio classico e' rileggere Z1 **dopo** X e Y e
> scartare il campione se la pressione e' calata; `test/touch.s` lo fa.

---

## 4. Opzioni dell'harness

```
Pannello
  -n, --no-gui          niente finestra (headless)
  -z, --scale N         ingrandimento 1..4 (default: automatico)
  -F, --fullscreen      parti a schermo intero
      --no-marker       non disegnare il mirino sul punto toccato
  -S, --screenshot FILE salva lo schermo in PPM all'uscita

Touch
  -p, --pen-irq LIV     /PENIRQ su IRQ autovector di livello LIV
  -k, --touch-clock M   rw (default) oppure w
  -X, --touch-cal SPEC  xmin,xmax,ymin,ymax[,swapxy,invx,invy]
  -N, --touch-noise N   rumore dell'ADC in LSB (0 = nessuno)
      --touch-script F  tocchi finti a tempo da file
      --panel-selftest  verifica la geometria finestra/pannello ed esce

  -v / -vv              log delle periferiche (-vv logga anche i dati)
```

`-S` salva il **raster del pannello**, senza il PWM di retroilluminazione:
cosi' lo screenshot e' confrontabile byte a byte nei test anche se il
firmware ha abbassato la luminosita'.

Se il pannello vero risulta montato ruotato o specchiato rispetto al
display, si allinea l'emulatore al ferro con `-X` invece di cambiare il
firmware. Esempio per un pannello con gli assi scambiati e Y invertita:

```
./m68k-harness rom.bin -X 200,3900,3900,200,1,0,0
```

### Nella finestra

| | |
|---|---|
| click / trascinamento | dito sul vetro |
| tastiera | console SCC canale A (anche le frecce, come ESC [ A..D) |
| F1 | mostra/nasconde il mirino |
| F11 | schermo intero |
| F12 | screenshot `screenNNN.ppm` |
| Ctrl-Q, chiusura finestra | esci |

Il titolo della finestra riporta dimensione del pannello, stato del display,
retroilluminazione, posizione del tocco con i valori ADC grezzi, fps e
percentuale di tempo reale raggiunta.

### Ingrandimento, schermo intero e piu' monitor

La finestra nasce sul **monitor dove si trova il puntatore**, non sempre sul
primo: cosi' F11 manda lo schermo intero sul monitor che stai guardando
(SDL porta il full screen sul display dove la finestra si trova). Se il
driver video non espone la posizione globale del puntatore - Wayland non lo
fa - si ripiega sul monitor 0.

Il pannello viene ingrandito a **scala intera** finche' ci sta (pixel netti,
niente interpolazione) e centrato, con una cornice scura intorno. Su un
monitor 1080p a schermo intero significa scala 2 con bande da 160 px ai lati
e 60 px sopra e sotto. Se la finestra e' piu' piccola di 800x480 il pannello
viene ridotto per intero invece di essere tagliato. Un click nella cornice
non e' un tocco sul vetro: il dito si stacca.

> **Nota per chi mette le mani in `panel.c`.** Il modulo **non** usa
> `SDL_RenderSetLogicalSize`. Da SDL 2.0.18 un renderer con logical size
> converte da solo le coordinate degli eventi mouse, e chi poi chiama anche
> `SDL_RenderWindowToLogical` divide due volte: con la finestra a scala 2
> (cioe' a schermo intero su un 1080p) resta raggiungibile solo il quarto in
> alto a sinistra del pannello, e per andare a destra bisogna trascinare
> oltre il bordo, su un altro monitor. Qui la trasformazione e' una sola
> (`fit_panel` / `unfit_panel`), usata sia per disegnare sia per il mouse, e
> `panel_init` verifica e azzera la logical size se qualcuno la rimette.

---

## 5. Lavorare senza finestra

Per le prove automatiche, o su una macchina senza display:

```sh
./m68k-harness test/touch.bin -n -f \
    --touch-script test/touch.script -S out.ppm
```

Lo script del touch e' un file di testo, un evento per riga, con i tempi in
millisecondi **di tempo emulato** dall'avvio:

```
500   down  100 100
550   move  200 150
850   up
1300  down  400  20     # commento
```

`make check` fa girare le ROM di prova headless, piu' i due controlli sulla
geometria della finestra, e confronta console e immagini: barre di colore, cornice, sfumatura, `read_ddb`, lettura della
memoria video, specchio di `set_address_mode`, scroll hardware, quadrati
disegnati sotto i tocchi.

---

## 6. ROM di prova

```sh
make roms          # servono vasmm68k_mot e python3
```

| | |
|---|---|
| `test/display.s` | barre di colore, sfumatura a 32 gradini, cornice con quattro finestre |
| `test/touch.s` | disegna sotto il dito, stampa le coordinate, la barra in alto pulisce |
| `test/features.s` | letture dal chip, `set_address_mode`, scroll hardware, backlight al 50% |
| `test/penirq.s` | /PENIRQ cablato su IRQ autovector 5, gestore che legge X e Y |
| `test/ssdlib.s` | routine riusabili: `_ssd_init`, `_window`, `_fill`, `_xpt_rd`, `_strout`, `_putdec`, `_puthex2/4` |
| `test/ssd1963.i` | indirizzi e colori |

---

## 7. Quanto costa davvero disegnare

Vale la pena saperlo prima di progettare la UI: su un 68010 a 10 MHz,
riempire lo schermo intero (384000 pixel) costa

| modo | cicli per pixel | schermo intero |
|---|---|---|
| `move.w Dn,d16(An)` in un ciclo con contatore | ~30 | ~1,15 s |
| `move.w Dn,(An)` srotolato 16 volte (`_fill`) | ~10 | ~0,39 s |

Il limite e' il bus, non il chip. Per una interfaccia utilizzabile conviene
ridisegnare solo le finestre che cambiano (e' esattamente a questo che
servono `set_column_address` / `set_page_address`) e appoggiarsi allo scroll
hardware per far scorrere il testo senza riscrivere un pixel.

Dal lato host l'emulatore non e' il collo di bottiglia: in modo `-f`, con
traffico MMIO pesante, gira a circa **38x il tempo reale** (200 milioni di
cicli, cioe' 20 s emulati, in ~0,5 s di muro). Con il pacing a 10 MHz
attivo la finestra sta comodamente a 60 fps.

---

## 8. Cosa controlla `make check`

La suite gira **tutta headless**, senza finestra e senza dipendenze oltre
python3: va bene anche in CI. Confronta la console riga per riga e le
immagini pixel per pixel.

**`display.bin`** - nessun bus error; 384000 pixel disegnati; le 8 barre a
y=100 sono esattamente nero, blu, rosso, verde, magenta, giallo, ciano,
bianco; la cornice c'e' su tutti e quattro i lati; la sfumatura cresce da
sinistra a destra.

**`features.bin`** - `read_ddb` risponde `01 57 61 01 FF` (l'ID vero
dell'SSD1963); `get_pixel_data_interface` risponde `03`; `get_power_mode`
risponde `9C` (booster + sleep out + normal + display on);
`read_memory_start` rilegge `F800 07E0 001F FFFF`, cioe' esattamente i
quattro pixel scritti poco prima; con `set_address_mode` flipX il marchio
giallo scritto alle colonne 0..39 finisce alle colonne 760..799; con lo
scroll hardware a VSP=140 quello stesso marchio, scritto alle righe
200..239, si vede alle righe 100..139; le due fasce fisse (TFA e BFA)
restano ferme.

**`touch.bin`** con `--touch-script` - le coordinate stampate seguono i
tocchi dello script entro il rumore dell'ADC; il tocco sulla barra in alto
pulisce davvero la tela; restano solo i due quadrati attesi, entro 3 pixel
dal punto toccato.

**`penirq.bin`** con `-p 5` - nessuna eccezione durante il gestore; il
gestore parte, segue il dito durante il trascinamento e arriva fino
all'ultimo tocco (600 interruzioni servite nei 3 s emulati della prova).

**Geometria della finestra** - due controlli, entrambi senza bisogno di un
display:

* `./m68k-harness --panel-selftest` verifica solo la matematica: scala
  intera, centratura, bande, e i quattro angoli del vetro che mappano sui
  quattro angoli del pannello. Casi coperti: schermo intero 1080p (scala 2),
  finestra a scala 1, finestra piu' piccola del pannello (riduzione a 0,5),
  pannello ruotato dal firmware, 4K (scala 4, non 4,5).
* `build/panel-e2e` con `SDL_VIDEODRIVER=dummy` prova la **catena intera**:
  apre la finestra, muove il puntatore con SDL e controlla dove finisce il
  dito sul vetro, prima a scala 2 e poi a schermo intero con le bande.
  Questo e' il controllo che prende la classe di bug legata alla logical
  size, che il test matematico da solo non vedrebbe; verifica anche che la
  logical size del renderer sia 0x0.

`make check-gui` fa girare la stessa prova end-to-end contro il **display
vero** invece che contro il driver dummy. Si salta da sola se il compositore
non permette di spostare il puntatore via software (Wayland).

Il tutto da `make clean` in poi, senza warning di compilazione.
