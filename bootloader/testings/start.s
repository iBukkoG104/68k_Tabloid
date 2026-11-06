_scc_port_a_ctrl    equ     $e00005
_scc_port_a_data    equ     $e00007
_scc_port_b_ctrl    equ     $e00001
_scc_port_b_data    equ     $e00003

    org 000000


    dc.l $028000        ; init ssp to top of SRAM
    dc.l _start_main


    org $001000
_start_main:

    lea _text,a0
    jsr _strout
    stop #00


_text:
    dc.b 10,13,'TEST TEST TEST',10,13,$80
    even

_strout:
    move.b (a0)+,d0     ; load character
_charout:
    cmp.b #$80,d0       ; compare num with termination char
    beq.b _charout_end  ; if is term, jump
                        ; else, continue
    move.b d0,_scc_port_a_data     ; print char loaded in d0
    jmp _strout
_charout_end:
    rts
