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
    incdir "lo_drvs/"
;
;;  RESET VECTOR etc. etc.
;

    org $000000

    dc.l $00028000        ; init ssp to top of SRAM
    dc.l Start

;; LOADING THE DEVICE DRIVERS
    org $001000
;
;; goodies
;

CR          equ     13
LF          equ     10


;
;; Z8530 Serial Communication Controller
;

    include "scc_lo_drv.s"

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

;;   ATA commands

__ata_cmd_ers_sec   equ     $c0     ; erase sector command
__ata_cmd_fmt_trk   equ     $50     ; format track command
__ata_cmd_rd_sec    equ     $20     ; read sector(s) command
__ata_cmd_wr_sec    equ     $30     ; write sector(s) command
__ata_cmd_seek      equ     $70     ; seek command

;
;; SSD1963 Display Driver
;

    include "ssd1963_lo_drv.s"


;
;; XPT2046 Touch Driver
;
__dspy_xpt_cs       equ     $e00301




Start:

;
;   what to do
; - Init the SCC                [DONE]
; - Init the built-in display
; - Init ZImodem
; - Init IDE/ATA device
; - Load FORTH shell
; - Load and attempt booting the first sector silently
; - Boot example code
; - Boot m68K_OS
;
;

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;           INIT SCC           ;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    jsr __scc_init_all          ; initialise all of the SCC





breakpoint
    lea text_breakpoint,a0
    jsr __scc_strout_a
    stop #$0000





text_breakpoint
    dc.b CR,LF,'TUNG TUNG TUNG SAHUR!',CR,LF,$ff

