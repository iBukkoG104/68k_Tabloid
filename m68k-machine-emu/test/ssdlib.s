; ssdlib.s - routine comuni per pannello SSD1963 e touch XPT2046.
; Convenzioni: a0 = SSD_BASE, a2 = XPT_BASE.

; ------------------------------------------------------- _ssd_init
; Sequenza di accensione tipica per un 7" 800x480 con clock da 10 MHz.
; Clobber: d0.
_ssd_init:
    move.b  #$01,SSD_CMD(a0)        ; soft_reset
    bsr     _wait_ms
    move.b  #$e2,SSD_CMD(a0)        ; set_pll_mn: PLL = 10MHz*(M+1)/(N+1)
    move.b  #$23,SSD_DATB(a0)       ;   M = 35
    move.b  #$02,SSD_DATB(a0)       ;   N = 2   -> 120 MHz
    move.b  #$54,SSD_DATB(a0)       ;   usa M e N
    move.b  #$e0,SSD_CMD(a0)        ; set_pll: accendi il PLL
    move.b  #$01,SSD_DATB(a0)
    bsr     _wait_ms
    move.b  #$e0,SSD_CMD(a0)        ; set_pll: passa al PLL
    move.b  #$03,SSD_DATB(a0)
    move.b  #$01,SSD_CMD(a0)        ; soft_reset dopo il cambio di clock
    bsr     _wait_ms

    move.b  #$e6,SSD_CMD(a0)        ; set_lshift_freq: pixel clock ~30 MHz
    move.b  #$03,SSD_DATB(a0)
    move.b  #$33,SSD_DATB(a0)
    move.b  #$33,SSD_DATB(a0)

    move.b  #$b0,SSD_CMD(a0)        ; set_lcd_mode
    move.b  #$20,SSD_DATB(a0)       ;   TFT 24 bit, sync normali
    move.b  #$00,SSD_DATB(a0)
    move.b  #$03,SSD_DATB(a0)       ;   HDP = 799 -> 800 colonne
    move.b  #$1f,SSD_DATB(a0)
    move.b  #$01,SSD_DATB(a0)       ;   VDP = 479 -> 480 righe
    move.b  #$df,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)

    move.b  #$b4,SSD_CMD(a0)        ; set_hori_period: HT=928 HPS=46 HPW=8
    move.b  #$03,SSD_DATB(a0)
    move.b  #$a0,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)
    move.b  #$2e,SSD_DATB(a0)
    move.b  #$08,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)

    move.b  #$b6,SSD_CMD(a0)        ; set_vert_period: VT=525 VPS=23 VPW=3
    move.b  #$02,SSD_DATB(a0)
    move.b  #$0d,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)
    move.b  #$17,SSD_DATB(a0)
    move.b  #$03,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)

    move.b  #$36,SSD_CMD(a0)        ; set_address_mode: niente specchi, RGB
    move.b  #$00,SSD_DATB(a0)
    move.b  #$f0,SSD_CMD(a0)        ; set_pixel_data_interface: 16 bit 565
    move.b  #$03,SSD_DATB(a0)

    move.b  #$be,SSD_CMD(a0)        ; set_pwm_conf: retroilluminazione
    move.b  #$06,SSD_DATB(a0)       ;   PWMF
    move.b  #$ff,SSD_DATB(a0)       ;   duty = 255 (massimo)
    move.b  #$01,SSD_DATB(a0)       ;   PWM abilitato
    move.b  #$f0,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)

    move.b  #$29,SSD_CMD(a0)        ; set_display_on
    rts

; ------------------------------------------------------- _wait_ms
; Attesa grossolana di qualche ms (il PLL dell'SSD1963 ha bisogno di tempo).
; Clobber: d0.
_wait_ms:
    move.l  #3000,d0
.w:
    subq.l  #1,d0
    bne.s   .w
    rts

; ------------------------------------------------------- _window
; Finestra di scrittura: d1=x0 d2=y0 d3=x1 d4=y1, poi write_memory_start.
; Clobber: d0.
_window:
    move.b  #$2a,SSD_CMD(a0)        ; set_column_address
    move.w  d1,d0
    lsr.w   #8,d0
    move.b  d0,SSD_DATB(a0)
    move.b  d1,SSD_DATB(a0)
    move.w  d3,d0
    lsr.w   #8,d0
    move.b  d0,SSD_DATB(a0)
    move.b  d3,SSD_DATB(a0)
    move.b  #$2b,SSD_CMD(a0)        ; set_page_address
    move.w  d2,d0
    lsr.w   #8,d0
    move.b  d0,SSD_DATB(a0)
    move.b  d2,SSD_DATB(a0)
    move.w  d4,d0
    lsr.w   #8,d0
    move.b  d0,SSD_DATB(a0)
    move.b  d4,SSD_DATB(a0)
    move.b  #$2c,SSD_CMD(a0)        ; write_memory_start
    rts

; ------------------------------------------------------- _fill
; Riempie la finestra corrente: d5 = colore RGB565, d6.l = numero di pixel.
; Clobber: d0, d6, a1.
;
; Vale la pena srotolare: con la porta dati in a1 ogni pixel costa un solo
; move.w Dn,(An) (8 cicli), contro i ~30 di un ciclo con contatore e salto.
; Lo schermo intero (384000 pixel) passa cosi' da ~1.15 s a ~0.31 s di tempo
; emulato: e' il costo reale di un bus a 16 bit pilotato da un 68010 a 10 MHz.
_fill:
    lea     SSD_DAT(a0),a1
    move.l  d6,d0
    and.l   #15,d6                  ; pixel che avanzano
    lsr.l   #4,d0                   ; blocchi da 16
    beq.s   .tail
.blk:
    rept    16
    move.w  d5,(a1)
    endr
    subq.l  #1,d0
    bne.s   .blk
.tail:
    tst.l   d6
    beq.s   .end
.one:
    move.w  d5,(a1)
    subq.l  #1,d6
    bne.s   .one
.end:
    rts

; ------------------------------------------------------- _xpt_rd
; Una conversione dell'XPT2046: d1.b = byte di controllo, d0.w = 12 bit.
; Tre cicli di bus: il byte di controllo esce, i due successivi fanno
; entrare BUSY + D11..D0 + 3 zeri, che si riallineano con uno shift di 3.
; Clobber: d0, d2.
_xpt_rd:
    move.b  d1,(a2)
    moveq   #0,d0
    move.b  (a2),d0
    lsl.w   #8,d0
    moveq   #0,d2
    move.b  (a2),d2
    or.w    d2,d0
    lsr.w   #3,d0
    and.w   #$0fff,d0
    rts

; ------------------------------------------------------- _putc
; Manda d0.b sulla console (SCC canale A) aspettando che il buffer di
; trasmissione sia libero. Senza l'attesa su RR0 D2 i caratteri si
; perdono appena il firmware programma il baud rate: a 9600 baud il
; trasmettitore resta occupato circa 10.000 cicli per carattere.
; Clobber: nessun registro.
_putc:
.wait:
    btst    #2,SCC_A_CTRL           ; RR0 D2 = Tx Buffer Empty
    beq.s   .wait
    move.b  d0,SCC_A_DATA
    rts

; ------------------------------------------------------- _strout
; Stringa terminata da 0 in a1 verso la console. Clobber: d0, a1.
_strout:
    move.b  (a1)+,d0
    beq.s   .end
    bsr     _putc
    bra.s   _strout
.end:
    rts

; ------------------------------------------------------- _putdec
; Stampa d0.w in decimale sulla console. Clobber: d0, d1, d3.
_putdec:
    and.l   #$ffff,d0
    moveq   #0,d3
.div:
    divu.w  #10,d0
    move.l  d0,d1
    swap    d1                      ; resto
    and.w   #$000f,d1
    add.w   #'0',d1
    move.w  d1,-(sp)
    addq.w  #1,d3
    and.l   #$ffff,d0
    bne.s   .div
.out:
    move.w  (sp)+,d0
    bsr     _putc
    subq.w  #1,d3
    bne.s   .out
    rts

; ------------------------------------------------------- _puthex2
; Stampa il byte basso di d0 in esadecimale. Clobber: d0, d1, d2.
_puthex2:
    move.w  d0,d2
    lsr.w   #4,d0
    bsr     _nibble
    move.w  d2,d0
    bsr     _nibble
    rts
_nibble:
    and.w   #15,d0
    cmp.w   #10,d0
    blt.s   .dec
    add.w   #'A'-10,d0
    bra.s   .put
.dec:
    add.w   #'0',d0
.put:
    bsr     _putc
    rts

; ------------------------------------------------------- _puthex4
; Stampa d0.w in esadecimale. Clobber: d0, d1, d2, d3.
_puthex4:
    move.w  d0,d3
    lsr.w   #8,d0
    bsr     _puthex2
    move.w  d3,d0
    bsr     _puthex2
    rts

; ------------------------------------------------------- _space / _crlf
; Non toccano nessun registro.
_space:
    move.l  d0,-(sp)
    move.b  #' ',d0
    bsr     _putc
    move.l  (sp)+,d0
    rts
_crlf:
    move.l  d0,-(sp)
    move.b  #13,d0
    bsr     _putc
    move.b  #10,d0
    bsr     _putc
    move.l  (sp)+,d0
    rts

; ----------------------------------------------- _map_x / _map_y
; ADC a 12 bit -> pixel. Il pannello simulato ha fondo scala 200..3900
; (vedi l'opzione -X dell'harness): x = (adc-200)*800/3700.
_map_x:
    sub.w   #200,d0
    bge.s   .ok
    moveq   #0,d0
.ok:
    and.l   #$ffff,d0
    mulu.w  #800,d0
    divu.w  #3700,d0
    and.l   #$ffff,d0
    cmp.w   #800,d0
    blt.s   .done
    move.w  #799,d0
.done:
    rts

_map_y:
    sub.w   #200,d0
    bge.s   .ok
    moveq   #0,d0
.ok:
    and.l   #$ffff,d0
    mulu.w  #480,d0
    divu.w  #3700,d0
    and.l   #$ffff,d0
    cmp.w   #480,d0
    blt.s   .done
    move.w  #479,d0
.done:
    rts
