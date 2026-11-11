;; scc_lo_drv.s
;;
;; This file is a low-level driver to manage the Z8530
;; Serial Communication Controller, loaded with all the
;; functions and control labels.
;;
;; CONTROL LABELS
;;
;;
;;
;;
;; COMMANDS
;;
;;
;;
;; SUBROUTINES
;;
;;
;;
;; MACROS
;;
;;
;;


;
;;  CONTROL LABELS
;


SCC_A_CTRL   equ     $e00005
SCC_A_DATA   equ     $e00007
SCC_B_CTRL   equ     $e00001
SCC_B_DATA   equ     $e00003

;
;;  COMMANDS
;

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
scc_init_b              ;; init porta B: 38400 baud
    dc.b $04,$44        ; WR4: X16 clock; 1 stop bit; no parity
    dc.b $03,$c0        ; WR3: 8-bit Rx
    dc.b $05,$60        ; WR5: 8-bit Tx
    dc.b $0a,$00        ; WR10: NRZ
    dc.b $0b,$50        ; WR11: Tx clock = BRG; Rx clock = BRG
    dc.b $0c,$01        ; WR12: time constant LOW
    dc.b $0d,$00        ; WR13: time constant HIGH
    dc.b $0e,$03        ; WR14: BRG enabled; source = PCLK
    dc.b $03,$c1        ; WR3: 8-bit Rx; Rx enabled
    dc.b $05,$68        ; WR5: 8-bit Tx; Tx enabled
    dc.b $ff            ; fine tabella


;
;; SUBROUTINES
;


    ;
    ;; Port A
    ;

_scc_init_a
    move.b (a0)+,d0     ; load reg byte into d0
    cmp.b #$ff,d0
    beq _scc_init_b     ; branch to init_b if is escape character
    move.b d0,SCC_A_CTRL    ; send reg select to portA
    move.b (a0)+,d0     ; load ctrl byte into d0
    move.b d0,SCC_A_CTRL    ; send reg select to portA
    bra _scc_init_a     ; repeat

    ;
    ;; Port B
    ;

_scc_init_b
    move.b (a1)+,d1     ; load reg byte into d1
    cmp.b #$ff,d1
    beq _scc_init_end  ; branch to end if is escape character
    move.b d1,SCC_B_CTRL     ; send reg select to portB
    move.b (a1)+,d1     ; load ctrl byte into d1
    move.b d1,SCC_B_CTRL     ; send reg select to portB
    bra _scc_init_b     ; repeat
_scc_init_end
    rts


    ;
    ;;  Port A (Serial Monitor)
    ;


_scc_charout_a          ;;print character to serial monitor/rs232;
                        ;;d0: outgoing character
                        ;;d1: temp operations
    move.b #$00,SCC_A_CTRL   ; select rr0
    move.b SCC_A_CTRL,d1     ; read rr0
    btst #2,d1          ; test if tx buffer (bit 2) if empty (1)
    beq _scc_charout_a  ; if not empty (0) await
                        ; else continue
    move.b d0,SCC_A_DATA     ; print character
    rts


_scc_charin_available_a    ;;return $ff if a character is awaiting on PortA,
                        ;;else return 0; d0: return status
                        ;;d1: temp operations
    move.b #$00,SCC_A_CTRL   ; select rr0
    move.b SCC_A_CTRL,d1     ; read rr0
    btst #0,d1          ; test if a character (bit 0) is incoming (1)
    beq _charin_avb_false   ; if not available (0) return 0
                        ; else return $ff
    move.b #$ff,d0      ; return true
    rts
_charin_avb_false
    move.b #0,d0        ; return false
    rts


_scc_charin_a           ;;fetch incoming character on portA;
                        ;;d0: incoming character
    move.b SCC_A_DATA,d0     ; read data from portA
    rts


    ;
    ;;  Port B (ESP32 ZImodem)
    ;


_scc_charout_b          ;;print character to ESP32 ZImodem
                        ;;d0: outgoing character
                        ;;d1: temp operations
    move.b #$00,SCC_B_CTRL   ; select rr0
    move.b SCC_B_CTRL,d1     ; read rr0
    btst #2,d1          ; test if tx buffer (bit 2) if empty (1)
    beq _scc_charout_b  ; if not empty (0) await
                        ; else continue
    move.b d0,SCC_B_DATA     ; print character
    rts


_scc_charin_available_b;;return $ff if a character is awaiting on PortB,
                        ;;else return 0; d0: return status
                        ;;d1: temp operations
    move.b #$00,SCC_B_CTRL   ; select rr0
    move.b SCC_B_CTRL,d1     ; read rr0
    btst #0,d1          ; test if a character (bit 0) is incoming (1)
    beq _charin_avb_false   ; if not available (0) return 0
                        ; else return $ff
    move.b #$ff,d0      ; return true
    rts


_scc_charin_b                ;;fetch incoming character on portB;
                             ;;d0: incoming character
    move.b SCC_B_DATA,d0     ; read data from portB
    rts



;
;;  MACROS
;

__scc_init_all          ;;initialise PortA(19200 baud) and PortB(38400 baud)
    lea scc_init_a,a0   ; load address of portA config table into a0
    lea scc_init_b,a1   ; load address of portB config table into a1
    bra _scc_init_a     ; jump to individual port initialisation


__scc_strout_a          ;;print a string to serial monitor/rs232,
                        ;; uses $FF as string terminator.
                        ;; a0: outgoing string pointer.
                        ;; d0: outgoing character
    move.b (a0)+,d0     ; outgoing char
    cmp.b #$ff,d0       ; check if is terminator char
    beq __scc_strout_end; exit if it is
                        ; else continue
    jsr _scc_charout_a  ; print character
    bra __scc_strout_a  ; rinse and repeat


__scc_strout_b          ;;print a string to ZImodem,
                        ;; uses $FF as string terminator.
                        ;; a0: outgoing string pointer.
                        ;; d0: outgoing character
    move.b (a0)+,d0     ; outgoing char
    cmp.b #$ff,d0       ; check if is terminator char
    beq __scc_strout_end; exit if it is
                        ; else continue
    jsr _scc_charout_b  ; print character
    bra __scc_strout_b  ; rinse and repeat
__scc_strout_end
    rts
