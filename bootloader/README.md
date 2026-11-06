# Bootloader & Firmware
One of the most important piece of software of the project -- allows the 68k_tabloid to boot and even work :O

## How does it work?
The Firmware takes care to initialise the system and all devices, and to boot from the IDE/ATA interface. It features a full fledged ANS FORTH interactive shell based off f68kans implementation of FORTH + some custom words and functions. After initialisation, it will quietly load the first sector of a MBR/FAT32 IDE device into DRAM $201000 and attempt to boot from it if bootable; else it will quit and show the FORTH shell on the build-in screen. The FORTH shell is always accessible through the serial monitor (PortA on the Z8530 SCC).


## Roadmap of development

 - Init the SCC                         [DONE]
 - Init the built-in display
 - Init ZImodem
 - Init IDE/ATA device
 - Load FORTH shell
 - Load and attempt booting the first sector silently
 - Boot example code
 - Boot m68K_OS
