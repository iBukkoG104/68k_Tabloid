beep_ce     equ $800000

do_lo       equ %0000 0000 00000000
re          equ %0000 0001 00000000
mi          equ %0000 0010 00000000
fa          equ %0000 0011 00000000
sol         equ %0000 0100 00000000
la          equ %0000 0101 00000000
si          equ %0000 0110 00000000
do_hi       equ %0000 0111 00000000



    org $000000
    dc.l 000000

    dc.l .start



    org $001000
.start:
    move.w #do_lo,beep_ce
    move.w #do_lo,beep_ce
    move.w #do_lo,beep_ce
    move.w #do_lo,beep_ce

    move.w #re,beep_ce
    move.w #re,beep_ce
    move.w #re,beep_ce
    move.w #re,beep_ce

    move.w #mi,beep_ce
    move.w #mi,beep_ce
    move.w #mi,beep_ce
    move.w #mi,beep_ce

    move.w #fa,beep_ce
    move.w #fa,beep_ce
    move.w #fa,beep_ce
    move.w #fa,beep_ce

    move.w #sol,beep_ce
    move.w #sol,beep_ce
    move.w #sol,beep_ce
    move.w #sol,beep_ce

    move.w #la,beep_ce
    move.w #la,beep_ce
    move.w #la,beep_ce
    move.w #la,beep_ce

    move.w #si,beep_ce
    move.w #si,beep_ce
    move.w #si,beep_ce
    move.w #si,beep_ce

    move.w #do_hi,beep_ce
    move.w #do_hi,beep_ce
    move.w #do_hi,beep_ce
    move.w #do_hi,beep_ce
    jmp .start


    org $00ffff
    dc.b 0
