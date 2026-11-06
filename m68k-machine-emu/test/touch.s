; touch.s - prova interattiva del touch XPT2046 sul pannello SSD1963.
;
; Tocca (o clicca e trascina) sullo schermo: sotto il dito compare un
; quadrato giallo e le coordinate finiscono sulla console seriale.
; Toccando la barra grigia in alto la tela si pulisce.
;
;   ./m68k-harness test/touch.bin

    include "ssd1963.i"

; variabili in SRAM
VARS        equ     $020000
v_x         equ     VARS+0
v_y         equ     VARS+2
v_down      equ     VARS+4

BAR_H       equ     40              ; altezza della barra in alto

    org     $000000
    dc.l    $028000                 ; SSP in cima alla SRAM
    dc.l    _start

_start:
    lea     SSD_BASE,a0
    lea     XPT_BASE,a2
    bsr     _ssd_init

    lea     _msg_intro(pc),a1
    bsr     _strout

    clr.w   v_down
    bsr     _paint_bar
    bsr     _clear_canvas

; ---------------------------------------------------------------- loop
_loop:
    move.b  #$b0,d1                 ; Z1: pressione sul vetro
    bsr     _xpt_rd
    cmp.w   #250,d0
    bge.s   .pressed
    clr.w   v_down                  ; dito alzato
    bra     _loop

.pressed:
    move.b  #$d0,d1                 ; canale X
    bsr     _xpt_rd
    bsr     _map_x
    move.w  d0,v_x
    move.b  #$90,d1                 ; canale Y
    bsr     _xpt_rd
    bsr     _map_y
    move.w  d0,v_y

    ; Seconda lettura della pressione: su un pannello resistivo vero il dito
    ; puo' staccarsi fra la conversione di X e quella di Y, e il campione
    ; esce mezzo buono e mezzo spazzatura. Chi legge prima e dopo lo scarta.
    move.b  #$b0,d1
    bsr     _xpt_rd
    cmp.w   #250,d0
    blt     _loop

    tst.w   v_down                  ; primo contatto?
    bne.s   .draw
    move.w  #1,v_down
    lea     _msg_tap(pc),a1
    bsr     _strout
    move.w  v_x,d0
    bsr     _putdec
    move.b  #',',d0
    bsr     _putc
    move.w  v_y,d0
    bsr     _putdec
    bsr     _crlf
    cmp.w   #BAR_H,v_y              ; tocco nella barra: pulisci
    bge.s   .draw
    bsr     _clear_canvas
    bra     _loop

.draw:
    move.w  v_x,d1                  ; quadrato 10x10 centrato sul dito
    subq.w  #5,d1
    bge.s   .x0
    moveq   #0,d1
.x0:
    cmp.w   #790,d1
    blt.s   .x1
    move.w  #789,d1
.x1:
    move.w  v_y,d2
    subq.w  #5,d2
    cmp.w   #BAR_H,d2
    bge.s   .y0
    move.w  #BAR_H,d2
.y0:
    cmp.w   #470,d2
    blt.s   .y1
    move.w  #469,d2
.y1:
    move.w  d1,d3
    add.w   #9,d3
    move.w  d2,d4
    add.w   #9,d4
    bsr     _window
    move.w  #C_YELLOW,d5
    move.l  #100,d6
    bsr     _fill
    bra     _loop

; ------------------------------------------------------ _clear_canvas
_clear_canvas:
    moveq   #0,d1
    move.w  #BAR_H,d2
    move.w  #799,d3
    move.w  #479,d4
    bsr     _window
    move.w  #C_NAVY,d5
    move.l  #800*(480-BAR_H),d6
    bsr     _fill
    rts

; --------------------------------------------------------- _paint_bar
_paint_bar:
    moveq   #0,d1
    moveq   #0,d2
    move.w  #799,d3
    move.w  #BAR_H-1,d4
    bsr     _window
    move.w  #C_GREY,d5
    move.l  #800*BAR_H,d6
    bsr     _fill
    rts

; ---------------------------------------------------------------- dati
_msg_intro:
    dc.b    13,10,'Touch XPT2046 pronto: tocca lo schermo.',13,10
    dc.b    'La barra grigia in alto pulisce la tela.',13,10,0
_msg_tap:
    dc.b    'tap @ ',0
    even

    include "ssdlib.s"
