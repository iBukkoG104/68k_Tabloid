; serial.s - Z8530 a 9600 8N1 con ricezione a interrupt sul canale B.
;
; E' la forma che serve a un driver PPP: i byte dall'ESP32 arrivano quando
; vogliono loro, il gestore li mette in una coda e il programma principale
; se li riprende con comodo. La trasmissione invece e' a polling su
; RR0 D2 (Tx Buffer Empty), che con il baud rate programmato adesso
; significa davvero aspettare.
;
;   ./m68k-harness test/serial.bin -n -f --scc-irq 4 --scc-b pty
;   (poi, da un altro terminale, picocom -b 9600 /dev/pts/N)

    include "ssd1963.i"

SCC_B_CTRL  equ     $e00001
SCC_B_DATA  equ     $e00003

VARS        equ     $020000
v_head      equ     VARS+0          ; indice di scrittura nella coda
v_tail      equ     VARS+2          ; indice di lettura
v_irqs      equ     VARS+4          ; interrupt serviti
v_buf       equ     VARS+16         ; coda circolare da 256 byte

    org     $000000
    dc.l    $028000                 ; 0: SSP
    dc.l    _start                  ; 1: PC
    ds.l    26                      ; vettori 2..27
    dc.l    _irq4                   ; 28: autovector di livello 4
    ds.l    35                      ; vettori 29..63
    ; Vettori $40..$47: servono solo se l'SCC mette il vettore sul bus
    ; (--scc-vectored). Con WR2 = $40 e VIS attivo il chip ne sceglie uno
    ; in base alla sorgente: $40 B Tx, $42 B Ext, $44 B Rx, $46 B special.
    dc.l    _irq4,_irq4,_irq4,_irq4
    dc.l    _irq4,_irq4,_irq4,_irq4

; ---------------------------------------------------------------- start
_start:
    clr.w   v_head
    clr.w   v_tail
    clr.w   v_irqs

    ; Reset dell'intero chip: va fatto una volta sola, tocca i due canali.
    move.b  #$09,SCC_A_CTRL
    move.b  #$C0,SCC_A_CTRL         ; WR9 = force hardware reset
    moveq   #20,d0
.rwait:
    dbra    d0,.rwait

    lea     SCC_A_CTRL,a2
    bsr     _scc_line               ; canale A: console
    lea     SCC_B_CTRL,a2
    bsr     _scc_line               ; canale B: ESP32

    ; Vettore base e interrupt di ricezione sul solo canale B.
    move.b  #$02,SCC_B_CTRL
    move.b  #$40,SCC_B_CTRL         ; WR2 = $40
    move.b  #$01,SCC_B_CTRL
    move.b  #$10,SCC_B_CTRL         ; WR1 = int su ogni carattere ricevuto
    move.b  #$09,SCC_B_CTRL
    move.b  #$09,SCC_B_CTRL         ; WR9 = MIE + VIS

    lea     _m_intro(pc),a1
    bsr     _strout

    move.w  #$2000,sr               ; maschera a 0: gli IRQ possono entrare

; ---------------------------------------------------------------- loop
; Svuota la coda riempita dal gestore: rimanda il byte all'ESP32 e lo
; mostra anche sulla console.
_loop:
    move.w  v_tail,d0
    cmp.w   v_head,d0
    beq.s   _loop

    lea     v_buf,a1
    move.b  0(a1,d0.w),d1
    addq.w  #1,d0
    and.w   #$00ff,d0
    move.w  d0,v_tail

.txb:
    btst    #2,SCC_B_CTRL           ; aspetta Tx Buffer Empty sul canale B
    beq.s   .txb
    move.b  d1,SCC_B_DATA

    move.b  d1,d0                   ; e la stessa cosa sulla console
    bsr     _putc
    bra     _loop

; ---------------------------------------------------------- gestore IRQ
_irq4:
    movem.l d0-d2/a1,-(sp)
    addq.w  #1,v_irqs

    move.b  #$03,SCC_A_CTRL         ; RR3 si legge solo dal canale A
    move.b  SCC_A_CTRL,d0
    btst    #2,d0                   ; D2 = canale B, Rx Character Available
    beq.s   .done

    move.b  SCC_B_DATA,d1           ; leggere il dato azzera il Rx IP
    move.w  v_head,d2
    lea     v_buf,a1
    move.b  d1,0(a1,d2.w)
    addq.w  #1,d2
    and.w   #$00ff,d2
    move.w  d2,v_head
.done:
    move.b  #$38,SCC_B_CTRL         ; Reset Highest IUS: chiude il servizio
    movem.l (sp)+,d0-d2/a1
    rte

; ------------------------------------------------------------- _scc_line
; Configura il canale puntato da a2 (porta di controllo) a 9600 8N1.
; 9600 = 3686400 / (2 * (10 + 2) * 16), quindi time constant = 10.
; Clobber: d0.
_scc_line:
    move.b  #$04,(a2)
    move.b  #$44,(a2)               ; WR4: x16, 1 stop bit, niente parita'
    move.b  #$03,(a2)
    move.b  #$C1,(a2)               ; WR3: Rx 8 bit, Rx abilitato
    move.b  #$05,(a2)
    move.b  #$68,(a2)               ; WR5: Tx 8 bit, Tx abilitato
    move.b  #$0B,(a2)
    move.b  #$50,(a2)               ; WR11: Tx e Rx prendono il clock dal BRG
    move.b  #$0C,(a2)
    move.b  #10,(a2)                ; WR12: time constant, byte basso
    move.b  #$0D,(a2)
    move.b  #0,(a2)                 ; WR13: byte alto
    move.b  #$0E,(a2)
    move.b  #$03,(a2)               ; WR14: BRG acceso, sorgente PCLK
    rts

_m_intro:
    dc.b    13,10,'SCC a 9600 8N1. Canale B in ricezione a interrupt:'
    dc.b    13,10,'quello che arriva torna indietro e si vede qui.',13,10,0
    even

    include "ssdlib.s"
