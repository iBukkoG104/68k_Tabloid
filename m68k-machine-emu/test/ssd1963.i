; ssd1963.i - indirizzi e routine comuni per il pannello del PDA M68K.
;
; Il display sta a $E00200-$E002FF con A1 = D/C# (A0 non conta):
;   $E00201  comando   (.B, LDS, D7-D0)
;   $E00202  dato      (.W, 16 bit: pixel RGB565)
;   $E00203  dato      (.B, LDS, D7-D0: parametri dei comandi)
; Le routine qui dentro si aspettano a0 = $E00200.

SSD_BASE    equ     $e00200
SSD_CMD     equ     1               ; offset da a0: porta comandi
SSD_DAT     equ     2               ; offset da a0: porta dati a word
SSD_DATB    equ     3               ; offset da a0: porta dati a byte

XPT_BASE    equ     $e00301         ; touch XPT2046 (porta SPI a 8 bit)

SCC_A_CTRL  equ     $e00005
SCC_A_DATA  equ     $e00007

; colori RGB565
C_BLACK     equ     $0000
C_WHITE     equ     $ffff
C_RED       equ     $f800
C_GREEN     equ     $07e0
C_BLUE      equ     $001f
C_YELLOW    equ     $ffe0
C_CYAN      equ     $07ff
C_MAGENTA   equ     $f81f
C_NAVY      equ     $0010
C_GREY      equ     $8410
