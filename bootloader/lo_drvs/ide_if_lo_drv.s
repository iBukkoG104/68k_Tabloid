;; ide_if_lo_drv.s
;;
;; This file is a low-level driver to manage the IDE/ATA
;; interface, loaded with all the functions and control labels.
;;
;; CONTROL LABELS
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
;; MACROS & FUNCTIONS
;;


;
;; CONTROL LABELS
;

IDE_DATA_REG        equ     $e00101 ; select CF Data register
IDE_ERR_FET_REG     equ     $e00103 ; select CF error (RD) or feature (WR) register
IDE_SECTOR_REG      equ     $e00105 ; select CF sector count register
IDE_LBA_07          equ     $e00107 ; select CF LBA bits 0-7    (Sector No.)
IDE_LBA_815         equ     $e00109 ; select CF LBA bits 8-15   (Cylinder Low)
IDE_LBA_1623        equ     $e0010b ; select CF LBA bits 16-23  (Cylinder High)
IDE_LBA_2427        equ     $e0010d ; select CF LBA bits 24-27  (Card/Head)
IDE_STS_CMD_REG     equ     $e0010f ; select CF Status (RD) or Command (WR) register


;
;;   COMMANDS
;

ata_cmd_ers_sec     equ     $c0     ; erase sector command
ata_cmd_fmt_trk     equ     $50     ; format track command
ata_cmd_id_drive    equ     $ec     ; identity drive
ata_cmd_idle        equ     $e3     ; idle drive
ata_cmd_rd_sec      equ     $20     ; read sector(s) command
ata_cmd_wr_sec      equ     $30     ; write sector(s) command
ata_cmd_seek        equ     $70     ; seek command


;
;;  SUBROUTINES
;


;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE

CF_AWAIT_DRDY:                  ;;read status register for DRDY status CP/M style
                                ;;DRDY: 0 = A reg: 00h; DRDY: 1 = A reg: 0FFh
    in A, (CF_STS_CMD_REG)      ; read status register
    bit 6, A                    ; test bit 6 (DRDY)
    jr nz, CF_AWAIT_TRUE        ; if 1 (DRDY set), return 0FFh
    xor A                       ; else, return 00h
    ret
;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_AWAIT_BUSY:                  ;;read status register for BSY status CP/M style
                                ;;BSY: 0 = A reg: 00h; BSY: 1 = A reg: 0FFh
    in A, (CF_STS_CMD_REG)      ; read status register
    bit 7, A                    ; test bit 7 (BSY)
    jr nz, CF_AWAIT_TRUE        ; if 1 (BSY set), return 0FFh
    xor A                       ; else, return 00h
    ret
;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_AWAIT_DRQ:                   ;;read status register for DRQ status CP/M style
                                ;;DRQ: 0 = A reg: 00h; DRQ: 1 = A reg: 0FFh
    in A, (CF_STS_CMD_REG)      ; read status register
    bit 3, A                    ; test bit 3 (DRQ)
    jr nz, CF_AWAIT_TRUE        ; if 1 (DRQ set), return 0FFh
    xor A                       ; else, return 00h
    ret
CF_AWAIT_TRUE:
    ld A, 0FFh
    ret

;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE

CF_WRITE_REG:                   ;;write command to a CF register
								;;C reg: register; B reg: command; D reg: tries
								;;max tries before giving up: 4096 TODO TEST IRL
	push DE                     ; save DE registers
	push HL                     ; save HL registers
	ld DE, 0                    ; reset DE registers
	ld H, 10h                   ; set mask to 16 (00010000b)
CF_WRREG_LOOP:
	inc DE                      ; advance tries by 1
	ld A, D                     ; load high byte of DE into A
	cp H                        ; compare with mask
	jr z, CF_WRREG_TMOUT		; if DE = 4096, abort
								; else, continue
	call CF_AWAIT_BUSY          ; await the CF card to be ready
	cp 0FFh						; compare result with BSY: 1 signal
	jr z, CF_WRREG_LOOP         ; if equals, retry
								; else, continue
	out (C), B                  ; write command to selected register

	pop HL                      ; restore HL registers
	pop DE                      ; restore DE registers
	xor A						; set success signal
	ret
CF_WRREG_TMOUT:
	pop HL						; restore HL registers
	pop DE						; restore DE registers
	ret	                        ; abort

;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_READ_REG_STS:                ;;read status from a CF register (reg A)
								;;C reg: register; Returns to reg A
	in A, (C)                   ; read status from selected register
	ret

;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_WRITE_DATA:                  ;;write data from buffer (0FDFFh) to CF
								;;B reg: LOW LBA; C reg: MID LBA; D reg: HIGH LBA
	call CF_AWAIT_BUSY          ; await BUSY
	cp 0FFh                     ; compare with busy signal
	jr z, CF_WRITE_DATA         ; if 0 (drive not ready), wait

	push BC                     ; save LOW and MID LBA address
	ld C, CF_SECTOR_REG         ; select Sector Count register
	ld B, 1                     ; write 1 sector
	call CF_WRITE_REG

	pop BC                      ; restore BC
	call  CF_LBA_LOADER			; load B, C and D regs into LBA


	ld C, CF_STS_CMD_REG        ; select command register
	ld B, ATA_CMD_WR_SEC        ; WRITE SECTOR ata command
	call CF_WRITE_REG

;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_WRDAT_WAIT1:
	call CF_AWAIT_DRQ           ; await Data request from CF Flash
	cp 0FFh                     ; check if DRQ is set
	jr nz, CF_WRDAT_WAIT1       ; if not, wait
								; else, continue
	xor A                       ; reset A
	ld HL, CF_BUFFER			; load buffer location into HL
CF_WRDAT_COPY:
	add A, 7Fh                  ; load differentiator between 1st and 2nd copy
	ld C, CF_DATA_REG
	ld B, 0                     ; loads 0 into reg B, will copy 256 to CF buffer
	otir                        ; outs data from buffer (0FDFFh) and onwards to
								;   the CF card.
	cp 0FEh                     ; check if loop has executed 1 or 2 times
	jr nz, CF_WRDAT_COPY        ; if executed once, loop again
;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_WRDAT_WAITEND:
	call CF_AWAIT_BUSY          ; await BUSY flag
	cp 0FFh                     ; is BUSY cleared?
	jr z, CF_WRDAT_WAITEND      ; if not, wait
	ret                         ; else, return
;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_LBA_LOADER:
	;
	;   LBA address selection
	;   B reg = LOW, C reg = MID, D reg = HIGH, E reg = HEAD
	;
	ld A, B
	out (CF_LBA_07), A          ; write LOW LBA
	ld A, C
	out (CF_LBA_815), A         ; write MID LBA
	ld A, D
	out (CF_LBA_1623), A        ; write HIGH LBA
	xor A	                    ; since I have a 512MB CF card, HEAD will be 0000
	;and 0Fh COMMENTED FOR FUTURE; keep only the low nibble
	or 0E0h                     ; merge with LBA and MASTER controls
	out (CF_LBA_2427), A        ;write HEAD LBA

	ret


;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_READ_DATA:                   ;;read data from CF to buffer (0FDFFh)
;;;NOTICE could become interrupt-driven one day ###
	call CF_AWAIT_BUSY          ; await BUSY
	cp 0FFh                     ; compare with busy signal
	jr z, CF_READ_DATA          ; if 0 (drive not ready), wait

	push BC                     ; save LOW and MID LBA address
	ld C, CF_SECTOR_REG         ; select Sector Count register
	ld B, 1                     ; write 1 sector
	call CF_WRITE_REG

	pop BC                      ; restore BC
	call CF_LBA_LOADER			; load B, C and D regs into LBA

	ld C, CF_STS_CMD_REG        ; select command register
	ld B, ATA_CMD_RD_SEC        ; READ SECTOR ata command
	call CF_WRITE_REG
;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE
CF_RDDAT_WAIT1:
	call CF_AWAIT_DRQ           ; await Data request from CF Flash
	cp 0FFh                     ; check if DRQ is set
	jr nz, CF_RDDAT_WAIT1       ; if not, wait
								; else, continue
	xor A                       ; reset A
	ld HL, CF_BUFFER			; load buffer location into HL
CF_RDDAT_COPY:
	add A, 7Fh                  ; load differentiator between 1st and 2nd copy
	ld C, CF_DATA_REG
	ld B, 0                     ; loads 0 into reg B, will copy 256 to CF buffer
	inir                        ; ins data from CF card to the buffer in RAM
	cp 0FEh                     ; check if loop has executed 1 or 2 times
	jr nz, CF_RDDAT_COPY        ; if executed once, loop again
	ret 						; else, returns
;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE

CF_DETECT:
    ld A, 0AAh
    out (CF_LBA_815), A     	; scrivi pattern su Cylinder Low
    in A, (CF_LBA_815)      	; rileggi
    cp 0AAh                  	; corrisponde?
    ret                      	; Z = CF presente, NZ = assente

;;FIXME ATTENTION ATTENTION  MUST BE PORTED TO 68K ASSEMBLY BEFORE USE



;
;;  MACROS & FUNCTIONS
;
