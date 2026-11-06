; features.s - esercita le parti dell'SSD1963 che non si vedono riempiendo
; lo schermo: letture dal chip, rotazione, scroll hardware, inversione e
; retroilluminazione PWM.
;
;   ./m68k-harness test/features.bin -n -f -c 30000000 -S features.ppm

    include "ssd1963.i"

    org     $000000
    dc.l    $028000
    dc.l    _start

_start:
    lea     SSD_BASE,a0
    bsr     _ssd_init

; --- identificazione: read_ddb deve dare 01 57 61 01 FF ----------------
    lea     _m_ddb(pc),a1
    bsr     _strout
    move.b  #$a1,SSD_CMD(a0)
    moveq   #4,d7
.ddb:
    move.b  SSD_DATB(a0),d0
    bsr     _puthex2
    bsr     _space
    dbra    d7,.ddb
    bsr     _crlf

; --- get_pixel_data_interface: deve dare 03 (16 bit 565) ---------------
    lea     _m_pdi(pc),a1
    bsr     _strout
    move.b  #$f1,SSD_CMD(a0)
    move.b  SSD_DATB(a0),d0
    bsr     _puthex2
    bsr     _crlf

; --- get_power_mode ----------------------------------------------------
    lea     _m_pwr(pc),a1
    bsr     _strout
    move.b  #$0a,SSD_CMD(a0)
    move.b  SSD_DATB(a0),d0
    bsr     _puthex2
    bsr     _crlf

; --- scrivi 4 pixel noti e rileggili con read_memory_start -------------
    move.w  #10,d1
    move.w  #10,d2
    move.w  #13,d3
    move.w  #10,d4
    bsr     _window
    move.w  #C_RED,SSD_DAT(a0)
    move.w  #C_GREEN,SSD_DAT(a0)
    move.w  #C_BLUE,SSD_DAT(a0)
    move.w  #C_WHITE,SSD_DAT(a0)

    lea     _m_rd(pc),a1
    bsr     _strout
    move.w  #10,d1                  ; stessa finestra, poi read_memory_start
    move.w  #10,d2
    move.w  #13,d3
    move.w  #10,d4
    bsr     _window
    move.b  #$2e,SSD_CMD(a0)
    moveq   #3,d7
.rd:
    move.w  SSD_DAT(a0),d0
    bsr     _puthex4
    bsr     _space
    dbra    d7,.rd
    bsr     _crlf

; --- disegno di base: tre fasce orizzontali ---------------------------
    moveq   #0,d1                   ; fascia fissa in alto: bianca
    moveq   #0,d2
    move.w  #799,d3
    move.w  #39,d4
    bsr     _window
    move.w  #C_WHITE,d5
    move.l  #800*40,d6
    bsr     _fill

    moveq   #0,d1                   ; area di scroll: righe a strisce
    move.w  #40,d2
    move.w  #799,d3
    move.w  #439,d4
    bsr     _window
    moveq   #0,d7                   ; 400 righe, 20 blocchi da 20 righe
.band:
    move.w  #C_RED,d5
    btst    #0,d7
    beq.s   .col
    move.w  #C_BLUE,d5
.col:
    move.l  #800*20,d6
    bsr     _fill
    addq.w  #1,d7
    cmp.w   #20,d7
    bne.s   .band

    moveq   #0,d1                   ; fascia fissa in basso: verde
    move.w  #440,d2
    move.w  #799,d3
    move.w  #479,d4
    bsr     _window
    move.w  #C_GREEN,d5
    move.l  #800*40,d6
    bsr     _fill

; --- rotazione: con flipX il marchio giallo finisce a destra -----------
    move.b  #$36,SSD_CMD(a0)        ; set_address_mode: specchia le colonne
    move.b  #$40,SSD_DATB(a0)
    moveq   #0,d1                   ; chiedo (0,200)-(39,239)...
    move.w  #200,d2
    move.w  #39,d3
    move.w  #239,d4
    bsr     _window
    move.w  #C_YELLOW,d5
    move.l  #40*40,d6
    bsr     _fill
    move.b  #$36,SSD_CMD(a0)        ; ...e torno all'orientamento normale
    move.b  #$00,SSD_DATB(a0)

; --- scroll hardware: TFA 40 + VSA 400 + BFA 40 = 480 ------------------
    move.b  #$33,SSD_CMD(a0)        ; set_scroll_area
    move.b  #$00,SSD_DATB(a0)       ;   TFA = 40
    move.b  #$28,SSD_DATB(a0)
    move.b  #$01,SSD_DATB(a0)       ;   VSA = 400
    move.b  #$90,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)       ;   BFA = 40
    move.b  #$28,SSD_DATB(a0)
    move.b  #$37,SSD_CMD(a0)        ; set_scroll_start: VSP = 140
    move.b  #$00,SSD_DATB(a0)
    move.b  #$8c,SSD_DATB(a0)

; --- retroilluminazione al 50% ----------------------------------------
    move.b  #$be,SSD_CMD(a0)
    move.b  #$06,SSD_DATB(a0)
    move.b  #$80,SSD_DATB(a0)       ; duty = 128
    move.b  #$01,SSD_DATB(a0)
    move.b  #$f0,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)
    move.b  #$00,SSD_DATB(a0)

    lea     _m_done(pc),a1
    bsr     _strout
    stop    #$2700

_m_ddb:
    dc.b    13,10,'read_ddb       : ',0
_m_pdi:
    dc.b    'pixel interface: ',0
_m_pwr:
    dc.b    'power mode     : ',0
_m_rd:
    dc.b    'read_memory    : ',0
_m_done:
    dc.b    'scroll attivo (VSP=140), retroilluminazione 50%.',13,10,0
    even

    include "ssdlib.s"
