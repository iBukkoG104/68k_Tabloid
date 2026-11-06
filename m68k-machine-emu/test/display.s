; display.s - prova del pannello SSD1963: barre di colore, sfumatura,
;             cornice disegnata con tre finestre diverse.
;
;   vasmm68k_mot -Fbin -m68010 -o display.bin display.s
;   ./m68k-harness test/display.bin -f -S schermo.ppm

    include "ssd1963.i"

    org     $000000
    dc.l    $028000                 ; SSP in cima alla SRAM
    dc.l    _start

; ---------------------------------------------------------------- start
_start:
    lea     SSD_BASE,a0
    bsr     _ssd_init

    lea     _msg_hello(pc),a1
    bsr     _strout

; --- meta' alta: 8 barre verticali da 100 px ----------------------------
    moveq   #0,d1
    moveq   #0,d2
    move.w  #799,d3
    move.w  #239,d4
    bsr     _window

    move.w  #239,d7                 ; 240 righe
.row:
    lea     _bars(pc),a1
    moveq   #7,d6                   ; 8 barre
.bar:
    move.w  (a1)+,d5
    move.w  #99,d4                  ; 100 pixel per barra
.px:
    move.w  d5,SSD_DAT(a0)
    dbra    d4,.px
    dbra    d6,.bar
    dbra    d7,.row

; --- meta' bassa: sfumatura di grigi in 32 gradini ---------------------
    moveq   #0,d1
    move.w  #240,d2
    move.w  #799,d3
    move.w  #479,d4
    bsr     _window

    move.w  #239,d7                 ; 240 righe
.grow:
    moveq   #0,d5                   ; livello 0..31
.gstep:
    move.w  d5,d0                   ; grigio = L<<11 | L<<6 | L
    lsl.w   #6,d0                   ;   L nei 6 bit del verde
    move.w  d0,d2
    lsl.w   #5,d2                   ;   L<<11 nei 5 bit del rosso
    add.w   d5,d0                   ;   L nei 5 bit del blu
    add.w   d2,d0
    move.w  #24,d6                  ; 25 pixel per gradino
.gpx:
    move.w  d0,SSD_DAT(a0)
    dbra    d6,.gpx
    addq.w  #1,d5
    cmp.w   #32,d5
    bne.s   .gstep
    dbra    d7,.grow

; --- cornice bianca: quattro finestre da una riga/colonna --------------
    move.w  #C_WHITE,d5
    moveq   #0,d1                   ; riga in alto
    moveq   #0,d2
    move.w  #799,d3
    moveq   #0,d4
    bsr     _window
    move.l  #800,d6
    bsr     _fill

    moveq   #0,d1                   ; riga in basso
    move.w  #479,d2
    move.w  #799,d3
    move.w  #479,d4
    bsr     _window
    move.l  #800,d6
    bsr     _fill

    moveq   #0,d1                   ; colonna a sinistra
    moveq   #0,d2
    moveq   #0,d3
    move.w  #479,d4
    bsr     _window
    move.l  #480,d6
    bsr     _fill

    move.w  #799,d1                 ; colonna a destra
    moveq   #0,d2
    move.w  #799,d3
    move.w  #479,d4
    bsr     _window
    move.l  #480,d6
    bsr     _fill

    lea     _msg_done(pc),a1
    bsr     _strout
    stop    #$2700                   ; fine: la finestra resta aperta

; ---------------------------------------------------------------- dati
_bars:
    dc.w    C_BLACK,C_BLUE,C_RED,C_GREEN
    dc.w    C_MAGENTA,C_YELLOW,C_CYAN,C_WHITE

_msg_hello:
    dc.b    13,10,'SSD1963: init, 800x480 RGB565...',13,10,0
_msg_done:
    dc.b    'quadro disegnato: 384000 pixel.',13,10,0
    even

    include "ssdlib.s"
