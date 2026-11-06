#!/usr/bin/env python3
"""Genera ROM di test a mano (codice macchina 68k) e un'immagine disco.

Uso: mkroms.py DIR
"""
import struct
import sys
from pathlib import Path

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)

SSP = 0x028000
START = 0x000400
HANDLER = 0x000500


def rom(code, handler=b""):
    img = bytearray(b"\xff" * 0x1000)
    vec = [HANDLER] * 256
    vec[0], vec[1] = SSP, START
    for i, v in enumerate(vec[:64]):
        struct.pack_into(">I", img, i * 4, v)
    img[START:START + len(code)] = code
    img[HANDLER:HANDLER + len(handler)] = handler
    return bytes(img)


def w(*words):
    return b"".join(struct.pack(">H", x) for x in words)


# --- 1) timer: handler incrementa d0, il main lascia gli IRQ abilitati -----
t_code = w(0x46FC, 0x2000,   # move.w #$2000,sr   (maschera 0: IRQ ok)
           0x7000,           # moveq #0,d0
           0x60FE)           # bra.s *
t_handler = w(0x5280,        # addq.l #1,d0
              0x4E73)        # rte
(out / "timer.bin").write_bytes(rom(t_code, t_handler))

# --- 2) IDE: READ SECTORS (word 0/1), IDENTIFY (word 49, 60, 61) ---------
ide = w(
    0x207C, 0x00E0, 0x0100,          # movea.l #$E00100,a0
    0x117C, 0x0001, 0x0005,          # move.b #1,5(a0)    seccnt=1
    0x117C, 0x0000, 0x0007,          # move.b #0,7(a0)    LBA 0-7
    0x117C, 0x0000, 0x0009,          # move.b #0,9(a0)    LBA 8-15
    0x117C, 0x0000, 0x000B,          # move.b #0,11(a0)   LBA 16-23
    0x117C, 0x00E0, 0x000D,          # move.b #$E0,13(a0) LBA mode
    0x117C, 0x0020, 0x000F,          # move.b #$20,15(a0) READ SECTORS
    0x3210,                          # move.w (a0),d1     word 0
    0x3410,                          # move.w (a0),d2     word 1
    0x117C, 0x00EC, 0x000F,          # move.b #$EC,15(a0) IDENTIFY
    0x3E3C, 0x0030,                  # move.w #48,d7
    0x3610,                          # loop: move.w (a0),d3   (word 0..48)
    0x51CF, 0xFFFC,                  # dbra d7,loop
    0x3810,                          # move.w (a0),d4     word 49
    0x3E3C, 0x0009,                  # move.w #9,d7
    0x3610,                          # loop2: move.w (a0),d3  (word 50..59)
    0x51CF, 0xFFFC,                  # dbra d7,loop2
    0x3A10,                          # move.w (a0),d5     word 60
    0x3C10,                          # move.w (a0),d6     word 61
    0x60FE,                          # bra.s *
)
(out / "ide.bin").write_bytes(rom(ide))

# --- 3) accessi anomali ----------------------------------------------------
misc = w(
    0x1039, 0x00E0, 0x011F,          # move.b $E0011F,d0   banco alternate -> 0xFF
    0x13FC, 0x0048, 0x00E0, 0x0007,  # move.b #'H',$E00007 SCC A data -> 'H'
    0x3239, 0x00E0, 0x0006,          # move.w $E00006,d1   .W su SCC
    0x60FE,
)
(out / "misc.bin").write_bytes(rom(misc))

# --- immagine disco: 4 settori, byte = (indice mod 256) + 1 ----------------
disk = bytes(((i % 256) + 1) & 0xFF for i in range(4 * 512))
(out / "disk.img").write_bytes(disk)
print("ok")
