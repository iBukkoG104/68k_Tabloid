# 68k_Tabloid
A 7" PDA powered by a MC68010 and electronic shenanigans. It features a custom POSIX-compliant OS, m68k_OS written specifically for this machine

## What is this all about?
This is a personal project I started to learn more about how systems and computers work by designing, refining, and building my own computer system, and OSDEV by making my own custom Operating System.

This "tabloid", a PDA a bit larger than usual, is powered by a Motorola 68010 CPU clocked at 10MHz, with 4MB of DRAM SIMM 30-pin on a custom socket, an 7" 800x480 65K color display and even an ESP-32 running ZImodem!

The software that powers this contraption is a FORTH-based interactive shell, like OpenFirmware on old PPC Macs, equipped with custom words and a bootloader capable of loading the first sector of a MBR/FAT32-formatted IDE/ATA drive and execute the code contained in it (if bootable).