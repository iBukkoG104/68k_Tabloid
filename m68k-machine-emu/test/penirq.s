; penirq.s - prova del /PENIRQ dell'XPT2046 cablato su un IRQ autovector.
;
;   ./m68k-harness test/penirq.bin -n -f -p 5 -c 30000000 \
;                  --touch-script test/touch.script
;
; /PENIRQ e' una linea di livello: resta bassa finche' il dito sta sul
; vetro e si rialza durante la conversione. Il gestore qui legge X e Y a
; ogni interruzione, quindi ne arriva una ogni volta che la linea torna
; bassa. E' il comportamento del ferro: un driver vero maschera la sorgente
; mentre elabora, oppure si limita a campionare finche' il dito c'e'.

    include "ssd1963.i"

VARS        equ     $020000
v_x         equ     VARS+0
v_y         equ     VARS+2
v_count     equ     VARS+4
v_last      equ     VARS+6

    org     $000000
    dc.l    $028000                 ; 0: SSP
    dc.l    _start                  ; 1: PC
    ds.l    27                      ; vettori 2..28
    dc.l    _irq5                   ; 29: autovector di livello 5

_start:
    lea     SSD_BASE,a0
    bsr     _ssd_init
    clr.w   v_count
    clr.w   v_last

    lea     _m_intro(pc),a1
    bsr     _strout

    move.w  #$2000,sr               ; maschera a 0: gli IRQ possono entrare

.poll:
    move.w  v_count,d0
    sub.w   v_last,d0
    cmp.w   #100,d0                 ; una riga ogni 100 interruzioni
    blt.s   .poll
    add.w   #100,v_last

    lea     _m_irq(pc),a1
    bsr     _strout
    move.w  v_count,d0
    bsr     _putdec
    lea     _m_at(pc),a1
    bsr     _strout
    move.w  v_x,d0
    bsr     _putdec
    move.b  #',',d0
    bsr     _putc
    move.w  v_y,d0
    bsr     _putdec
    bsr     _crlf
    bra.s   .poll

; ---------------------------------------------------------- gestore IRQ
_irq5:
    movem.l d0-d3/a1-a2,-(sp)
    addq.w  #1,v_count
    lea     XPT_BASE,a2
    move.b  #$d0,d1                 ; X
    bsr     _xpt_rd
    bsr     _map_x
    move.w  d0,v_x
    move.b  #$90,d1                 ; Y
    bsr     _xpt_rd
    bsr     _map_y
    move.w  d0,v_y
    movem.l (sp)+,d0-d3/a1-a2
    rte

_m_intro:
    dc.b    13,10,'/PENIRQ su livello 5: aspetto che tocchi.',13,10,0
_m_irq:
    dc.b    'irq ',0
_m_at:
    dc.b    ' -> ',0
    even

    include "ssdlib.s"
