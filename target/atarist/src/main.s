; Firmware loader from cartridge
; (C) 2023-2025 by Diego Parrilla
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.
; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .	 
; bit 30: TOS application .
; bit 31: TTP

ROM4_ADDR			equ $FA0000
SCREEN_SIZE			equ (-4096)	; Use the memory before the screen memory to store the copied code

; Constants needed for the commands
RANDOM_TOKEN_ADDR:        equ (ROM4_ADDR + $F000) 	      ; Random token address at $FAF000
RANDOM_TOKEN_SEED_ADDR:   equ (RANDOM_TOKEN_ADDR + 4) 	  ; RANDOM_TOKEN_ADDR + 4 bytes
RANDOM_TOKEN_POST_WAIT:   equ $1        		      	  ; Wait this cycles after the random number generator is ready
COMMAND_TIMEOUT           equ $0000FFFF                   ; Timeout for the command

SHARED_VARIABLES:     	  equ (RANDOM_TOKEN_ADDR + $200)  ; random token + 512 bytes to the shared variables area: $FAF200

ROMCMD_START_ADDR:        equ $FB0000					  ; We are going to use ROM3 address
CMD_MAGIC_NUMBER    	  equ ($ABCD) 					  ; Magic number header to identify a command
CMD_RETRIES_COUNT	  	  equ 3							  ; Number of retries for the command
CMD_SET_SHARED_VAR		  equ 1							  ; This is a fake command to set the shared variables
														  ; Used to store the system settings

; MD/JS worker commands (must match js_worker.h CMD_JS_* values)
CMD_JS_PING    				equ $0010 ; Ping — detect worker, get version
CMD_JS_UPLOAD  				equ $0011 ; Upload JS source chunk
CMD_JS_CALL    				equ $0012 ; Call JS function with JSON args

MDJS_RESULT_ADDR              equ (ROM4_ADDR + $F100)

_dskbufp                equ $4c6                            ; Address of the disk buffer pointer    


	include inc/sidecart_macros.s
	include inc/tos.s



; Macros
; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

	section

;Rom cartridge

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
;	dc.l second
	dc.l second
	dc.l $08000000 + pre_auto		; After GEMDOS init (before booting from disks)
	dc.l 0
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_pre_auto - pre_auto
	dc.b "MDJS",0
    even

second:
	dc.l 0
	dc.l 0
	dc.l mdjsdemo_cart_run
	dc.w GEMDOS_TIME
	dc.w GEMDOS_DATE
	dc.l end_mdjsdemo - mdjsdemo_cart_run
	dc.b "MDJSDEMO.PRG",0
	even

pre_auto:
; Relocate the content of the cartridge ROM to the RAM

; Get the screen memory address to display
	get_screen_base
	move.l d0, a2

	lea SCREEN_SIZE(a2), a2		; Move to the work area just after the screen memory
	move.l a2, a3				; Save the relocation destination address in A3
	; Copy the code out of the ROM to avoid unstable behavior
    move.l #end_rom_code - start_rom_code, d6
    lea start_rom_code, a1    ; a1 points to the start of the code in ROM
    lsr.w #2, d6
    subq #1, d6
.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
	jmp (a3)

start_rom_code:
; Detect MD/JS worker (ping the RP2040 with CMD_JS_PING)
; D7 = 1 if worker is available, 0 otherwise (used by boot_gem path)
; Wait ~1 second (50 VBL ticks on PAL / 60 on NTSC) so the RP2040 has time
; to bring up JerryScript before we ping it, otherwise the first ping
; usually races the worker and reports "not detected".
	move.w #50, d6
.js_detect_delay:
	move.w #37, -(sp)			; XBIOS Vsync — wait for next vertical blank
	trap #14
	addq.l #2, sp
	dbf d6, .js_detect_delay
	send_sync CMD_JS_PING, 4	; payload = random token only (4 bytes)
	tst.w d0					; D0 = 0 on success, non-zero on timeout/error
	beq.s .js_found
	clr.l d7					; Worker not detected
	bra.s .js_detect_done
.js_found:
	move.l #1, d7				; Worker detected
.js_detect_done:
	bra boot_gem

boot_gem:
	; Print a startup message via GEMDOS Cconws, then return to GEM.
	; If the JS worker was detected (D7 = 1) show the ready message,
	; otherwise show a not-detected warning.
	tst.l d7
	beq.s .boot_gem_no_worker

	pea msg_ready
	move.w #9, -(sp)				; GEMDOS Cconws
	trap #1
	addq.l #6, sp
	bra.s .boot_gem_done

.boot_gem_no_worker:
	pea msg_not_detected
	move.w #9, -(sp)				; GEMDOS Cconws
	trap #1
	addq.l #6, sp

.boot_gem_done:
	rts

; Cartridge-resident MD/JS demo app.
; Registers as a GEM app (AES), runs ping/upload/call, then shows result.
mdjsdemo_cart_run:
	movem.l d2-d7/a2-a6,-(sp)

	bsr mdjsdemo_aes_init
	tst.w d0
	bmi .mdjsdemo_no_aes

	send_sync CMD_JS_PING, 4
	tst.w d0
	beq .mdjsdemo_ping_ok
	lea mdjsdemo_alert_worker_missing, a0
	bsr mdjsdemo_form_alert
	bra .mdjsdemo_exit_aes
.mdjsdemo_ping_ok:

	lea mdjsdemo_upload_source, a4
	moveq #0, d3
	moveq #1, d4
	move.l #MDJS_UPLOAD_SOURCE_LEN, d5
	send_write_sync CMD_JS_UPLOAD, MDJS_UPLOAD_SOURCE_LEN
	tst.w d0
	beq .mdjsdemo_upload_ok
	lea mdjsdemo_alert_upload_failed, a0
	bsr mdjsdemo_form_alert
	bra .mdjsdemo_exit_aes
.mdjsdemo_upload_ok:

	lea mdjsdemo_call_payload, a4
	moveq #0, d3
	moveq #1, d4
	move.l #MDJS_CALL_PAYLOAD_LEN, d5
	send_write_sync CMD_JS_CALL, MDJS_CALL_PAYLOAD_LEN
	tst.w d0
	beq .mdjsdemo_call_ok
	lea mdjsdemo_alert_call_failed, a0
	bsr mdjsdemo_form_alert
	bra .mdjsdemo_exit_aes
.mdjsdemo_call_ok:

	bsr mdjsdemo_build_success_alert
	lea mdjsdemo_alert_buffer, a0
	bsr mdjsdemo_form_alert
	bra .mdjsdemo_exit_aes

.mdjsdemo_no_aes:
	pea mdjsdemo_noaes_msg
	move.w #Cconws, -(sp)
	trap #1
	addq.l #6, sp
	bra .mdjsdemo_done

.mdjsdemo_exit_aes:
	bsr mdjsdemo_aes_exit

.mdjsdemo_done:
	movem.l (sp)+,d2-d7/a2-a6
	rts

mdjsdemo_aes:
	move.w #$C8, d0
	lea mdjsdemo_aespb, a0
	move.l a0, d1
	trap #2
	move.w mdjsdemo_aes_intout, d0
	rts

mdjsdemo_aes_init:
	move.l #mdjsdemo_aes_ctrl_appl_init, mdjsdemo_aespb
	bsr mdjsdemo_aes
	rts

mdjsdemo_aes_exit:
	move.l #mdjsdemo_aes_ctrl_appl_exit, mdjsdemo_aespb
	bsr mdjsdemo_aes
	rts

; a0 = pointer to alert string "[#][lines][buttons]"
mdjsdemo_form_alert:
	move.w #1, mdjsdemo_aes_intin
	move.l a0, mdjsdemo_aes_addrin
	move.l #mdjsdemo_aes_ctrl_form_alert, mdjsdemo_aespb
	bsr mdjsdemo_aes
	rts

mdjsdemo_build_success_alert:
	lea mdjsdemo_alert_buffer, a0
	lea mdjsdemo_alert_prefix, a1
	bsr mdjsdemo_append_cstr
	bsr mdjsdemo_append_result
	tst.w d6
	beq .build_success_no_result
	tst.w d5
	beq .build_success_no_result
	lea mdjsdemo_alert_suffix, a1
	bsr mdjsdemo_append_cstr
	clr.b (a0)
	rts
.build_success_no_result:
	lea mdjsdemo_alert_buffer, a0
	lea mdjsdemo_alert_no_result, a1
	bsr mdjsdemo_append_cstr
	clr.b (a0)
	rts

; Append zero-terminated src (a1) to dst (a0). Returns updated a0.
mdjsdemo_append_cstr:
.append_cstr_loop:
	move.b (a1)+, d0
	beq .append_cstr_done
	move.b d0, (a0)+
	bra .append_cstr_loop
.append_cstr_done:
	rts

; Append unswapped result bytes from MDJS_RESULT_ADDR to dst (a0).
mdjsdemo_append_result:
	move.l #MDJS_RESULT_ADDR, a1
	move.w #63, d7
	clr.w d6				; number of chars appended
	clr.w d5				; 1 if any meaningful char was appended
.append_result_loop:
	move.w (a1)+, d0
	move.b d0, d1			; low byte
	lsr.w #8, d0			; high byte
	beq .append_result_done
	cmpi.b #$20, d0
	bcs .append_result_done
	cmpi.b #$7E, d0
	bhi .append_result_done
	cmpi.b #'=', d0
	beq .append_result_store_hi
	cmpi.b #' ', d0
	beq .append_result_store_hi
	moveq #1, d5
.append_result_store_hi:
	move.b d0, (a0)+
	addq.w #1, d6
	dbf d7, .append_result_low
	bra .append_result_done
.append_result_low:
	tst.b d1
	beq .append_result_done
	cmpi.b #$20, d1
	bcs .append_result_done
	cmpi.b #$7E, d1
	bhi .append_result_done
	cmpi.b #'=', d1
	beq .append_result_store_lo
	cmpi.b #' ', d1
	beq .append_result_store_lo
	moveq #1, d5
.append_result_store_lo:
	move.b d1, (a0)+
	addq.w #1, d6
	dbf d7, .append_result_loop
.append_result_done:
	rts

msg_ready:
	dc.b "MD/JS: JavaScript Worker is ready",$d,$a,0
	even

msg_not_detected:
	dc.b "MD/JS: JavaScript Worker not detected",$d,$a,0
	even

mdjsdemo_aes_ctrl_appl_init:
	dc.w 10,0,1,0,0
mdjsdemo_aes_ctrl_appl_exit:
	dc.w 19,0,1,0,0
mdjsdemo_aes_ctrl_form_alert:
	dc.w 52,1,1,1,0

mdjsdemo_aespb:
	dc.l mdjsdemo_aes_ctrl_appl_init
	dc.l mdjsdemo_aes_global
	dc.l mdjsdemo_aes_intin
	dc.l mdjsdemo_aes_intout
	dc.l mdjsdemo_aes_addrin
	dc.l mdjsdemo_aes_addrout

mdjsdemo_aes_global:
	ds.w 16
mdjsdemo_aes_intin:
	ds.w 16
mdjsdemo_aes_intout:
	ds.w 16
mdjsdemo_aes_addrin:
	ds.l 8
mdjsdemo_aes_addrout:
	ds.l 2

mdjsdemo_upload_source:
	dc.b "function add(a,b){ return a+b; }"
mdjsdemo_upload_source_end:
	even
MDJS_UPLOAD_SOURCE_LEN	equ (mdjsdemo_upload_source_end - mdjsdemo_upload_source)

mdjsdemo_call_payload:
	dc.b "add",0,"[5,7]",0
mdjsdemo_call_payload_end:
	even
MDJS_CALL_PAYLOAD_LEN	equ (mdjsdemo_call_payload_end - mdjsdemo_call_payload)

mdjsdemo_alert_prefix:
	dc.b "[1][MD/JS Demo|add(5,7) = ",0
mdjsdemo_alert_suffix:
	dc.b "][OK]",0

mdjsdemo_alert_worker_missing:
	dc.b "[1][MD/JS worker not detected.][OK]",0
	even
mdjsdemo_alert_upload_failed:
	dc.b "[1][MD/JS upload failed.][OK]",0
	even
mdjsdemo_alert_call_failed:
	dc.b "[1][MD/JS call failed.][OK]",0
	even
mdjsdemo_alert_no_result:
	dc.b "[1][MD/JS did not return a valid result.][OK]",0
	even

mdjsdemo_noaes_msg:
	dc.b "MD/JS demo requires GEM AES.",$d,$a,0
	even

mdjsdemo_alert_buffer:
	ds.b 128
	even

end_mdjsdemo:

; Shared functions included at the end of the file
; Don't forget to include the macros for the shared functions at the top of file
    include "inc/sidecart_functions.s"


end_rom_code:
end_pre_auto:
	even
	dc.l 0
