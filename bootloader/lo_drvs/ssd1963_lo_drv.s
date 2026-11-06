;; ssd1963_lo_drv.s
;;
;; This file is a low-level driver to manage the built-in
;; display, loaded with all the functions and control labels.
;;
;;
;; AI USAGE
;;
;; AI was used for understanding unclear passages of the Solomon Systech
;; SSD1963 datasheet, code review (I know shit about m68k assembly and
;; basically larping OSDEV) and debug. Everything produced here is the
;; fruit of my own thinking and research.
;;
;;
;;
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
;;
;; MACROS
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
SLEEP_MODE_ON   equ $10 ; enter sleep mode
SLEEP_MODE_OFF  equ $11 ; exit sleep mode
PARTIAL_MODE	equ $12	; enter partial mode
NORMAL_MODE		equ $13 ; enter normal mode
NO_INV_MODE		equ	$20	; exit inverted mode
GO_INV_MODE		equ $21 ; enter inverted mode
DISP_OFF        equ $28	; Blanks the display
DISP_ON			equ	$29	; Show image on display
SET_COL_ADDR	equ	$2a	; set column address
SET_PAGE_ADDR	equ	$2b	; set page address
WR_MEM_START	equ	$2c	; transfer data from CPU to SSD1963 from WR_MEM_START and SET_PAGE_ADDR
SET_PART_AREA	equ	$30	; define partial display area on dsp panel
SET_SCRL_AREA	equ $33	; define vertical scrolling and fixed area on dsp
SET_ADDR_MODE	equ	$36	; set read order from fb to display
SET_SCRL_START	equ $37	; define vertical scrolling start point
WR_MEM_CONTINUE	equ	$3c ; transfer image info from CPU to SSD1963 from last written location
RD_DDB			equ $A1	; read the DDB from provided location
SET_LCD_MODE	equ $b0 ; set LCD panel mode and resolution
SET_PXL_DATA_IF	equ	$f0	; set pixel data format of parallel host CPU interface

;
;;  subroutines
;

await_5ms
	move.l #4166,d7
await_loop
	dbra d7,await_loop
	rts




__soft_reset		; 0 parameters
	move.b SOFT_RST,__ssd_cmd
	bsr await_5ms
	rts


__get_display_mode	; 1 parameter
	; a7 x a5 x x a2 a1 a0 -- d0
	;
	; a7: vertical scrolling | 0 = off 1 = on
	; a5: invert mode        | 0 = off 1 = on
	; A2-0: gamma curve selection
	; 				000 = gamma curve 0
	; 				001 = gamma curve 1
	; 				010 = gamma curve 2
	; 				011 = gamma curve 3
	; returns to d0
	;
	move.b GET_DISP_MODE,__ssd_cmd
	clr.l d0					; clear d0
	move.b __ssd_data,d0		; get parameter
	rts


__sleep_mode_on		; 0 parameters
	move.b SLEEP_MODE_ON,__ssd_cmd
	bsr await_5ms
	rts


__sleep_mode_off	; 0 parameters
	move.b SLEEP_MODE_OFF,__ssd_cmd
	bsr await_5ms
	rts


__enter_partial_mode; no parameters
	move.b PARTIAL_MODE,__ssd_cmd
	rts


__enter_normal_mode; no parameters
	move.b NORMAL_MODE,__ssd_cmd
	rts


__exit_invert_mode; no parameters
	move.b NO_INV_MODE,__ssd_cmd
	rts


__enter_invert_mode; no parameters
	move.b GO_INV_MODE,__ssd_cmd
	rts


__set_display_off	;no parameters
	move.b DISP_OFF,__ssd_cmd
	rts


__set_display_on	;no parameters
	move.b DISP_ON,__ssd_cmd
	rts


__set_column_address	;4 parameters
	; sc15 - sc8 -- d0 low word high byte
	; sc7  - sc0 -- d0 low word low byte
	; ec15 - ec8 -- d1 low word high byte
	; ec7  - ec0 -- d1 low word low byte
	;
	; sc15..8 : Start column number high byte
	; sc7..0  : Start column number low byte
	; ec15..8 : End column number high byte
	; ec7..0  : End column number low byte
	;
	;
	move.b SET_COL_ADDR,__ssd_cmd
	; transfer start column high byte
	rol.w #8,d0		;prepare high byte
	move.b d0,__ssd_data
	; transfer start column low byte
	rol.w #8,d0		;prepare low byte
	move.b d0,__ssd_data

	; transfer end column high byte
	rol.w #8,d1		;prepare high byte
	move.b d1,__ssd_data
	; transfer end column low byte
	rol.w #8,d1		;prepare low byte
	move.b d1,__ssd_data
	rts


__set_page_address	;4 parameters
	; sc15 - sc8 -- d0 low word high byte
	; sc7  - sc0 -- d0 low word low byte
	; ec15 - ec8 -- d1 low word high byte
	; ec7  - ec0 -- d1 low word low byte
	;
	; sc15..8 : Start column number high byte
	; sc7..0  : Start column number low byte
	; ec15..8 : End column number high byte
	; ec7..0  : End column number low byte
	;
	;
	move.b SET_PAGE_ADDR,__ssd_cmd
	; transfer start column high byte
	rol.w #8,d0		;prepare high byte
	move.b d0,__ssd_data
	; transfer start column low byte
	rol.w #8,d0		;prepare low byte
	move.b d0,__ssd_data

	; transfer end column high byte
	rol.w #8,d1		;prepare high byte
	move.b d1,__ssd_data
	; transfer end column low byte
	rol.w #8,d1		;prepare low byte
	move.b d1,__ssd_data
	rts


__write_memory_start	;no parameters
	move.b NORMAL_MODE,__ssd_cmd
	rts


__set_partial_area	;4 parameters
	; sc15 - sc8 -- d0 low word high byte
	; sc7  - sc0 -- d0 low word low byte
	; ec15 - ec8 -- d1 low word high byte
	; ec7  - ec0 -- d1 low word low byte
	;
	; sc15..8 : Start column number high byte
	; sc7..0  : Start column number low byte
	; ec15..8 : End column number high byte
	; ec7..0  : End column number low byte
	;
	; SR and ER cannot be 0000h nor exceed last vertical line number (479)
	; error reported in d0(SR) or d1(ER)

	;check if SR < 480
	cmp.w #479,d0				; is d0 less than 480?
	bhi .partial_area_abrt_SR	; if higher, quit with error on d0
							    ; else, continue
	;check if ER < 480
	cmp.w #479,d1				; is d1 less than 480?
	bhi .partial_area_abrt_ER	; if higher, quit with error on d1
								; else, continue


	move.b DISP_OFF,__ssd_cmd

	; transfer start column high byte
	rol.w #8,d0		;prepare high byte
	move.b d0,__ssd_data
	; transfer start column low byte
	rol.w #8,d0		;prepare low byte
	move.b d0,__ssd_data

	; transfer end column high byte
	rol.w #8,d1		;prepare high byte
	move.b d1,__ssd_data
	; transfer end column low byte
	rol.w #8,d1		;prepare low byte
	move.b d1,__ssd_data
	rts
.partial_area_abrt_SR
	clr.l d0			;flush d0
	move.l #$deadbeef,d0;load errorcode
	rts
.partial_area_abrt_ER
	clr.l d1			;flush d1
	move.l #$deadbeef,d1;load errorcode
	rts


__set_scroll_area	;6 parameters
	; tfa15 - tfa8 -- d0 low word high byte
	; tfa7  - tfa0 -- d0 low word low byte
	; vsa15 - vsa8 -- d1 low word high byte
	; vsa7  - vsa0 -- d1 low word low byte
	; bfa15 - bfa8 -- d2 low word high byte
	; bfa7  - bfa0 -- d2 low word low byte
	;
	;
	; tfa15..8: framebuffer Top Fixed Area high byte
	; tfa7..0:  framebuffer Top Fixed Area low byte
	; vsa15..8: framebuffer Vertical Scrolling Area high byte
	; vsa7..0:  framebuffer Vertical Scrolling Area low byte
	; bfa15..8: framebuffer Bottom Fixed Area high byte
; 	; bfa7..0:  framebuffer Bottom Fixed Area low byte
	;
	;
	; TFA + VSA + BFA = display panel’s horizontal lines (480)
	; error reported in d0

	add.w d0,d3		; add tfa into d3
	add.w d1,d3		; add vsa into d3
	add.w d2,d3		; add bfa into d3
	cmp.w #480,d3	; is tfa + vsa + bfa = 480?
	bne .scroll_area_abrt	; if != 480, abort
							; else continue

	move.b SET_SCRL_AREA,__ssd_cmd

	; transfer tfa high byte
	rol.w #8,d0		;prepare high byte
	move.b d0,__ssd_data
	; transfer tfa low byte
	rol.w #8,d0		;prepare low byte
	move.b d0,__ssd_data

	; transfer vsa high byte
	rol.w #8,d1		;prepare high byte
	move.b d1,__ssd_data
	; transfer vsa low byte
	rol.w #8,d1		;prepare low byte
	move.b d1,__ssd_data

	; transfer bfa high byte
	rol.w #8,d2		;prepare high byte
	move.b d2,__ssd_data
	; transfer bfa low byte
	rol.w #8,d2		;prepare low byte
	move.b d2,__ssd_data
	rts
.scroll_area_abrt
	clr.l d0		; flush d0
	move.l #$deadbeef,d0	;load errorcode
	rts


__set_address_mode	;1 parameter
; A7 - A0	-- d0
;
; A7: Page address order   | 0 = top to bottom     1 = bottom to top
; A6: Column address order | 0 = left to right     1 = right to left
; A5: Page/cols order	   | 0 = normal            1 = reverse
; A4: Line address order   | 0 = refresh from top  1 = refresh from bottom
; A3: RGB / BRG order	   | 0 = RGB 			   1 = BRG
; A2: Dsp data latch data  | 0 = rfsh left > right 1 = rfsh right > left
; A1: Flip horizontal	   | 0 = normal			   1 = flipped
; A0: Flip vertical		   | 0 = normal			   1 = flipped
;

	move.b SET_ADDR_MODE,__ssd_cmd

	; send settings
	move.b d0,__ssd_data
	rts


__set_scroll_start	;2 parameters
; vsp15 - vsp8 -- d0 high byte
; vsp7  - vsp0 -- d0 low byte
;
; vsp15..8: framebuffer scrolling area high byte
; vsp7..0:  framebuffer scrolling area low byte
;
	move.b SET_SCRL_START,__ssd_cmd

	; transfer vsp high byte
	rol.w #8,d0		;prepare high byte
	move.b d0,__ssd_data
	; transfer vsp low byte
	rol.w #8,d0		;prepare low byte
	move.b d0,__ssd_data
	rts

__write_memory_continue	;no parameters
	move.b WR_MEM_CONTINUE,__ssd_cmd
	rts


__read_ddb		;5 parameters
; ssl15 - ssl8  -- d0 high byte
; ssl7  - ddl0  -- d0 low byte
; prod7 - prod0 -- d1 high byte
; rev2 - rev0   -- d1 low byte
; exit code = FFh
;
; ssl15..8: Supplier ID of Solomon Systech Limited high byte
; ssl7..0:  Supplier ID of Solomon Systech Limited low byte
; PROD7..0: Product ID, always 61h
; REV2..0:  Revision code, always 01h
; Exit code: ffh termination code
;
;
;
; exit code should ALWAYS be $ff
; put error code on d0
;
	move.b RD_DDB,__ssd_cmd

	; get SSL into d0
	move.b __ssd_data,d0	; get high byte
	rol.b #8,d0				; put it into the correct spot
	move.b __ssd_data,d0	; get low byte

	; get PROD byte into d1 high byte
	move.b __ssd_data,d1	; get PROD
	rol.b #8,d1				; put it into the high byte
	;get REV byte into d1 low byte
	move.b __ssd_data,d1	; get REV

	; get and check for exit code

	move.b __ssd_data,d2	; get exit code
	cmp.b #$ff,d2			; is it $ff?
	bne	.read_ddb_fail		; something went wrong!
	rts
.read_ddb_fail
	clr.l d0	; flush d0
	clr.l d1	; flush d1
	move.l #$deadbeef,d0	;load errorcode
	rts


__set_lcd_mode		;7 parameters
; A5 - A0      -- d0 high byte
; B6 - B0      -- d0 low byte
; hdp10 - hdp8 -- d1 high byte
; hdp7  - hdp0 -- d1 low byte
; vdp10 - vdp8 -- d1 high byte
; vdp7  - vdp0 -- d1 low byte
;
; A5: TFT panel data width    | 0 = 18-bit       1 = 24-bit
; A4: TFT color depth enh. en | 0 = disable FRC  1 = enable FRC enh.
; A3:
;
;
;
;
;
;
;
;
;






__set_pixel_data_interface







