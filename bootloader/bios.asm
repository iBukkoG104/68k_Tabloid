; bios.asm  -  Basic Input/Output System for the 68k-Tabloid.
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;                               ;
;             purpose           ;
;                               ;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; this program is the bios of the system, it takes care for the initial
; system initialisation and provide an interactive FORTH shell using the
; F68KANS FORTH implementation. After system startup, unless differently
; specified, the system will automatically load the first sector of the
; IDE drive and attempt to boot from it if a bootsector is found, else
; return to FORTH.
;
;
;
;
;; goodies
;
CR          equ     13
LF          equ     10


;
;; Z8530 Serial Communication Controller
;
__scc_port_a_ctrl   equ     $e00005
__scc_port_a_data   equ     $e00007
__scc_port_b_ctrl   equ     $e00001
__scc_port_b_data   equ     $e00003

;
;; IDE/ATA fixed drive
;
__ide_data_reg      equ     $e00101 ; select CF Data register
__ide_err_fet_reg   equ     $e00103 ; select CF error (RD) or feature (WR) register
__ide_sector_reg    equ     $e00105 ; select CF sector count register
__ide_lba_07        equ     $e00107 ; select CF LBA bits 0-7    (Sector No.)
__ide_lba_815       equ     $e00109 ; select CF LBA bits 8-15   (Cylinder Low)
__ide_lba_1623      equ     $e0010b ; select CF LBA bits 16-23  (Cylinder High)
__ide_LBA_2427      equ     $e0010d ; select CF LBA bits 24-27  (Card/Head)
__ide_sts_cmd_reg   equ     $e0010f ; select CF Status (RD) or Command (WR) register

;
;;   ATA commands
;
__ata_cmd_ers_sec   equ     $c0     ; erase sector command
__ata_cmd_fmt_trk   equ     $50     ; format track command
__ata_cmd_rd_sec    equ     $20     ; read sector(s) command
__ata_cmd_wr_sec    equ     $30     ; write sector(s) command
__ata_cmd_seek      equ     $70     ; seek command

;
;; SSD1963 Display Driver
;
__dspy_ssd_cmd      equ     $e00201 ; send commands to display controller
__dspy_ssd_data     equ     $e00203 ; send data to display controller

    org $000000

    dc.l $028000        ; init ssp to top of SRAM
    dc.l Start

    org $001000
Start:
    jsr __scc_init_all


    lea text_1,a0
    jsr __scc_strout_a
    stop #$dead


text_1
    dc.b CR,LF,'TUNG TUNG TUNG SAHUR!',CR,LF,$ff






;;;;;;;;;;;;;;;;;;;;;;;;
;          SCC         ;
;;;;;;;;;;;;;;;;;;;;;;;;
    even
__scc_init_all          ;;initialise PortA(19200 baud) and PortB(38400 baud)
    lea scc_init_a,a0   ; load address of portA config table into a0
    lea scc_init_b,a1   ; load address of portB config table into a1

    ;
    ;; Port A
    ;

__scc_init_a
    move.b (a0)+,d0     ; load reg byte into d0
    cmp #$ff,d0
    beq __scc_init_b    ; branch to init_b if is escape character
    move.b d0,__scc_port_a_ctrl    ; send reg select to portA
    move.b (a0)+,d0     ; load ctrl byte into d0
    move.b d0,__scc_port_a_ctrl    ; send reg select to portA
    jmp __scc_init_a    ; repeat

    ;
    ;; Port B
    ;

__scc_init_b
    move.b (a1)+,d1     ; load reg byte into d1
    cmp #$ff,d1
    beq __scc_init_end  ; branch to end if is escape character
    move.b d1,__scc_port_b_ctrl     ; send reg select to portB
    move.b (a1)+,d1     ; load ctrl byte into d1
    move.b d1,__scc_port_b_ctrl     ; send reg select to portB
    jmp __scc_init_b    ; repeat
__scc_init_end
    rts

scc_init_a              ;; init porta A: 19200 baud
    dc.b $09,$c0        ; WR9: force hardware reset (entrambe le porte)
    dc.b $04,$44        ; WR4: X16 clock; 1 stop bit; no parity
    dc.b $03,$c0        ; WR3: 8-bit Rx
    dc.b $05,$60        ; WR5: 8-bit Tx
    dc.b $09,$00        ; WR9: no reset; disable interrupts
    dc.b $0a,$00        ; WR10: NRZ
    dc.b $0b,$50        ; WR11: Tx clock = BRG; Rx clock = BRG
    dc.b $0c,$04        ; WR12: time constant LOW
    dc.b $0d,$00        ; WR13: time constant HIGH
    dc.b $0e,$03        ; WR14: BRG enabled; source = PCLK
    dc.b $03,$c1        ; WR3: 8-bit Rx; Rx enabled
    dc.b $05,$68        ; WR5: 8-bit Tx; Tx enabled
    dc.b $ff            ; fine tabella
scc_init_b              ;; init porta B: 9600 baud
    dc.b $04,$44        ; WR4: X16 clock; 1 stop bit; no parity
    dc.b $03,$c0        ; WR3: 8-bit Rx
    dc.b $05,$60        ; WR5: 8-bit Tx
    dc.b $0a,$00        ; WR10: NRZ
    dc.b $0b,$50        ; WR11: Tx clock = BRG; Rx clock = BRG
    dc.b $0c,$0a        ; WR12: time constant LOW
    dc.b $0d,$00        ; WR13: time constant HIGH
    dc.b $0e,$03        ; WR14: BRG enabled; source = PCLK
    dc.b $03,$c1        ; WR3: 8-bit Rx; Rx enabled
    dc.b $05,$68        ; WR5: 8-bit Tx; Tx enabled
    dc.b $ff            ; fine tabella


__scc_strout_a          ;;print a string to serial monitor/rs232,
                        ;; uses $FF as string terminator.
                        ;; a0: outgoing string pointer.
                        ;; d0: outgoing character
    move.b (a0)+,d0     ; outgoing char
    cmp.b #$ff,d0       ; check if is terminator char
    beq __scc_strout_end; exit if it is
                        ; else continue
    jsr __scc_charout_a ; print character
    jmp __scc_strout_a  ; rinse and repeat

__scc_strout_b          ;;print a string to ZImodem,
                        ;; uses $FF as string terminator.
                        ;; a0: outgoing string pointer.
                        ;; d0: outgoing character
    move.b (a0)+,d0     ; outgoing char
    cmp.b #$ff,d0       ; check if is terminator char
    beq __scc_strout_end; exit if it is
                        ; else continue
    jsr __scc_charout_b ; print character
    jmp __scc_strout_b  ; rinse and repeat
__scc_strout_end
    rts


    ;
    ;;  Port A (Serial Monitor)
    ;


__scc_charout_a         ;;print character to serial monitor/rs232;
                        ;;d0: outgoing character
                        ;;d1: temp operations
    move.b #$00,__scc_port_a_ctrl   ; select rr0
    move.b __scc_port_a_ctrl,d1     ; read rr0
    btst #2,d1          ; test if tx buffer (bit 2) if empty (1)
    beq __scc_charout_a ; if not empty (0) await
                        ; else continue
    move.b d0,__scc_port_a_data     ; print character
    rts


__scc_charin_available_a   ;;return $ff if a character is awaiting on PortA,
                        ;;else return 0; d0: return status
                        ;;d1: temp operations
    move.b #$00,__scc_port_a_ctrl   ; select rr0
    move.b __scc_port_a_ctrl,d1     ; read rr0
    btst #0,d1          ; test if a character (bit 0) is incoming (1)
    bne __charin_avb_false  ; if not available (0) return 0
                        ; else return $ff
    move.b #$ff,d0      ; return true
    rts
__charin_avb_false
    move.b #0,d0        ; return false
    rts


__scc_charin_a          ;;fetch incoming character on portA;
                        ;;d0: incoming character
    move.b __scc_port_a_data,d0     ; read data from portA
    rts


    ;
    ;;  Port B (ESP32 ZImodem)
    ;


__scc_charout_b         ;;print character to ESP32 ZImodem
                        ;;d0: outgoing character
                        ;;d1: temp operations
    move.b #$00,__scc_port_b_ctrl   ; select rr0
    move.b __scc_port_b_ctrl,d1     ; read rr0
    btst #2,d1          ; test if tx buffer (bit 2) if empty (1)
    beq __scc_charout_b ; if not empty (0) await
                        ; else continue
    move.b d0,__scc_port_b_data     ; print character
    rts


__scc_charin_available_b;;return $ff if a character is awaiting on PortB,
                        ;;else return 0; d0: return status
                        ;;d1: temp operations
    move.b #$00,__scc_port_b_ctrl   ; select rr0
    move.b __scc_port_b_ctrl,d1     ; read rr0
    btst #0,d1          ; test if a character (bit 0) is incoming (1)
    bne __charin_avb_false  ; if not available (0) return 0
                        ; else return $ff
    move.b #$ff,d0      ; return true
    rts


__scc_charin_b          ;;fetch incoming character on portA;
                        ;;d0: incoming character
    move.b __scc_port_b_data,d0     ; read data from portA
    rts
