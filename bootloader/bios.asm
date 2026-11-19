; bios.asm  -  Basic Input/Output System for the 68k-Tabloid.
;
; ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; ;                               ;
; ;             purpose           ;
; ;                               ;
; ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; this program is the bios of the system, it takes care for the initial
; system initialisation and provide an interactive FORTH shell using the
; F68KANS FORTH implementation. After system startup, unless differently
; specified, the system will automatically load the first sector of the
; IDE drive and attempt to boot from it if a bootsector is found, else
; return to FORTH.
;
;
;   assembler directive to consider low drivers folder
    incdir "lo_drvs/"
;
;;  RESET VECTOR etc. etc.
;

    org $000000
    dc.l $00028000        ; init ssp to top of SRAM
    dc.l Start


    org $001000
;
;; goodies
;
CR          equ     13
LF          equ     10

;
;;
;; driver loading
;;
;; scc_lo_drv.s:        Z8530 Serial Communication Controller low driver
;; ide_if_lo_drv.s:     IDE/ATA interface low driver
;; ssd1963_lo_drv.s:    SSD1963 Display controller low driver
;; esp32wifi_lo_drv.s:  ESP32 Wifi interface low driver
;; xpt2046_lo_drv.s:    XPT2046 RTP controller low driver
;;
;

    include "scc_lo_drv.s"
    ;include "ide_if_lo_drv.s"  ; driver not ready for inclusion
    include "ssd1963_lo_drv.s"



Start:

;
;   what to do
; - write device drivers                [ONGOING]
;   - SCC driver                        [DONE]
;   - SSD1963 display driver            [ONGOING]
;   - IDE interface driver              [ONGOING]
;   - ESP32 Wifi interface driver
;   - XPT2046 driver
;   - PIC MCU svr driver
;
; - "Init" the MCU
;   - RTC clock bringup
;   - tick clock setup
;   - get the FORTH shell parameters from NVRAM
;
; - Init the SCC                        [DONE]
; - Init the built-in display
; - Load FORTH shell
; - Init IDE/ATA device
; - Init ESP32 Wifi interface
;   - Raise the Wifi baudrate to 38400
; - Boot m68K_OS
;
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;          INIT STUFF          ;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; PIC stuff


; SCC
    jsr __scc_init_all          ; initialise the SCC





breakpoint
    lea text_breakpoint,a0
    jsr __scc_strout_a
    bsr breakpoint





text_breakpoint
    dc.b CR,LF,'TUNG TUNG TUNG SAHUR!',CR,LF,$ff
