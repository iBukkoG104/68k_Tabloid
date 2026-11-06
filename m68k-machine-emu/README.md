# AI WARNING! AI WARNING! AI WARNING! AI WARNING!

### to speed up software and OS development, this harness was developed with Claude Code.

# m68k-machine-emu

## what's this?

This is a Musashi harness written in C to emulate a Motorola 68k CPU and it's
used as the core of the emulator of my machine.



## what does this offer?

This emulator offers the possibility to boot a .bin ROM file and to test
firmware, debug code, and parts of an operating system.

Up to this date, it has:
- Full memory map implemented with the correct addressess;
- serial monitor emulation on terminal;
- PortB of the emulator is connected to a virtual pts to hook up to `pppd` and emulate ZImodem's `AT+PPP` function;
- Display and resistive touch emulation through separate window.

