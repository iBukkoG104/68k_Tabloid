;; ssd1963_lo_drv.s
;;
;; This file is a low-level driver to manage the built-in
;; display, loaded with all the functions and control labels.
;;
;;
;; CONTROL LABELS
;;
;; __ssd_cmd: select the command register
;; __ssd_data: select the data register
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

;
;;  control labels
;

__ssd_cmd       equ     $e00201 ; send commands to display controller
__ssd_data      equ     $e00203 ; send data to display controller


;
;;  driver commands
;

SOFT_RST        equ $01 ; Software Reset
GET_DISP_MODE   equ $0d ; return Display Image Mode
SLEEP_MODE_IN   equ $10 ; enter sleep mode
SLEEP_MODE_OUT  equ $11 ; exit sleep mode
DISP_OFF        equ $28	; Blanks the display
DISP_ON			equ	$29	; Show image on display
SET_COL_ADDR	equ	$2a	; set column address
SET_PAGE_ADDR	equ	$2b	; set page address
WR_MEM_START	equ	$2c	; transfer data from CPU to SSD1963 from WR_MEM_START to SET_PAGE_ADDR
SET_ADDR_MODE	equ	$36	; set read order from fb to display
SET_SCRL_START	equ $37	; define vertical scrolling start point
WR_MEM_CONTINUE	equ	$3c ; transfer image info from CPU to SSD1963 from last written location
SET_LCD_MODE	equ $b0 ; set LCD panel mode and resolution
SET_PXL_DATA_IF	equ	$f0	; set pixel data format of parallel host CPU interface

;
;;  subroutines
;

__soft_reset
	move.b SOFT_RST,__ssd_cmd
	rts
