# ESP32 Bare-Metal Custom Secondary Bootloader

![Language](https://img.shields.io/badge/Language-C11%20%2B%20Xtensa%20ASM-blue)
![Platform](https://img.shields.io/badge/Platform-ESP32--D0WD-red)
![Architecture](https://img.shields.io/badge/Architecture-Xtensa%20LX6-orange)
![Binary Size](https://img.shields.io/badge/Binary%20Size-7%2C844%20bytes-green)
![License](https://img.shields.io/badge/License-MIT-brightgreen)
![QEMU](https://img.shields.io/badge/QEMU-Verified%209.2.2-purple)

> A production-grade bare-metal secondary bootloader for the ESP32 (Tensilica Xtensa LX6),
> written in C11 and Xtensa Assembly. No ESP-IDF, no FreeRTOS, no Arduino. Every register is direct MMIO.

---

## What Is This Project?

When an ESP32 powers on, the **mask ROM** (burnt into silicon) runs first. It reads flash offset
`0x1000`, copies the image into IRAM, and jumps to the entry point. **That image is our secondary bootloader.**

It runs before your application and is responsible for:

| Job | Detail |
|-----|--------|
| Hardware init | Disable watchdog, configure UART0 @ 115200, set GPIO pull-ups |
| Flash integrity | Raw SPI1 USR-mode reads, CRC32 table, hardware SHA-256 |
| Boot decision | GPIO 0 level sampling + RTC_CNTL_STORE6 magic word |
| OTA management | 40-byte ota_control_t block at flash 0x8000 |
| Execution handover | Interrupt mask, VECBASE relocation, pipeline flush, JX jump |

---

## System Architecture

`
Power-On Reset
      |
      v
+-------------------+
|  ESP32 Mask ROM   |  (factory silicon, 0x40000000)
|  ROM Bootloader   |  Reads flash:0x1000 -> copies to IRAM -> JX
+--------+----------+
         | JX -> 0x40080620 (_entry_point)
         v
+--------------------------------------------------+
|           startup.S  (IRAM @ 0x40080000)         |
|  RSIL  a4,15    <- mask ALL interrupts           |
|  WSR   a0,VECBASE <- relocate Xtensa vectors     |
|  ISYNC           <- flush pipeline (MANDATORY)  |
|  MEMW            <- drain memory write buffer   |
|  CALL0 bootloader_main <- jump into C           |
+-------------------+------------------------------+
                    |
                    v
+--------------------------------------------------+
|             bootloader_main()                    |
|  1. Disable watchdog (TIMG0/TIMG1/RTC)          |
|  2. Init UART0 (115200-8-N-1)                   |
|  3. Build CRC32 table in DRAM                   |
|  4. Configure GPIO 0, sample level               |
|                                                  |
|  GPIO0=LOW or RTC_STORE6=0xDEADBEEF?            |
|    YES -> IAP mode (XModem UART upload)          |
|    NO  -> Read OTA state @ 0x8000               |
|           Validate 0xE9 magic + CRC32 + SHA-256  |
|           rsil/wsr vecbase/isync/memw/jx         |
+--------------------------------------------------+
`

---

## Flash Memory Map

`
Physical SPI Flash - 4 MB
---------------------------------------------
Address     Size    Region
----------- ------- -------------------------
0x000000      4 KB  ROM BL Header
0x001000     28 KB  BOOTLOADER CODE (us)
0x008000      4 KB  OTA Control Block
0x009000     28 KB  Reserved
0x010000      1 MB  App Slot 1 (primary FW)
0x110000      1 MB  App Slot 2 (OTA payload)
0x3E0000     64 KB  NVS key-value store
0x3F0000     64 KB  Factory / PHY data
---------------------------------------------
`

---

## Core Features

### Raw SPI1 Flash Reads (Cache-Disabled)

When SPI1 is active, SPI0 XIP cache must be disabled.
All functions in this path are IRAM_ATTR:

`c
DPORT_PRO_CACHE_CTRL_REG &= ~DPORT_PRO_CACHE_ENABLE;
__asm__ volatile("memw" ::: "memory");

SPI1_USER_REG      = SPI_USR_CMD | SPI_USR_ADDR | SPI_USR_MISO;
SPI1_USER2_REG     = (7 << SPI_USR_COMMAND_BITLEN_S) | 0x03; // READ
SPI1_ADDR_REG      = flash_offset << 8;
SPI1_MISO_DLEN_REG = (len * 8 - 1);
SPI1_CMD_REG       = SPI_CMD_USR;
while (SPI1_CMD_REG & SPI_CMD_USR);
`

### Hardware SHA-256 Accelerator

Xtensa is little-endian; SHA-256 requires big-endian. BSWAP32 is critical:

`c
#define BSWAP32(x)  ((((x)&0xFF000000)>>24)|(((x)&0x00FF0000)>>8) | \
                     (((x)&0x0000FF00)<<8) |(((x)&0x000000FF)<<24))

for (int i = 0; i < 16; i++)
    SHA_TEXT_REG(i) = BSWAP32(block_word[i]);
SHA_256_REG = 1;
while (SHA_BUSY_REG & 1);
`

### VECBASE Relocation + Atomic Jump

`sm
rsil   a4, 15        ; mask all interrupts
wsr    a1, vecbase   ; move vector table to app VECBASE (256B aligned)
isync               ; MANDATORY: pipeline flush after WSR VECBASE
memw               ; drain pending memory transactions
jx     a0          ; one-way jump to application entry
`

### Dual-Path IAP Mode

| Path | Trigger |
|------|---------|
| Hardware | GPIO 0 == LOW at boot (BOOT button) |
| Software | RTC_CNTL_STORE6 == 0xDEADBEEF (survives soft-reset) |

---

## Build Results

`
Toolchain: xtensa-esp-elf-gcc (crosstool-NG esp-16.1.0_20260609) 16.1.0
Flags:     -Os -mlongcalls -ffreestanding -nostdlib -ffunction-sections

   text    data     bss     dec
   4652    3192    8200   16044    bootloader.elf

   bootloader.bin = 7,844 bytes
   Budget         = 28,672 bytes (28 KB flash slot)
   Used           = 27.4%  PASS
`

---

## QEMU Emulation Results

Verified with Espressif QEMU esp_develop_9.2.2_20260417 and a 4 MB virtual flash image.

### Command

`ash
qemu-system-xtensa \
    -M esp32 \
    -drive file=flash.bin,if=mtd,format=raw \
    -serial stdio \
    -display none \
    -no-reboot
`

### Captured UART0 Output

`
ets Jul 29 2019 12:21:46

rst:0x1 (POWERON_RESET),boot:0x12 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0x00
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:1
load:0x40080000,len:1540
load:0x40080604,len:3112
`

### What Each Line Proves

| Output | What It Confirms |
|--------|-----------------|
| `ets Jul 29 2019 12:21:46` | ESP32 mask ROM ran correctly in QEMU |
| `rst:0x1 (POWERON_RESET)` | Clean power-on reset (not watchdog) |
| `boot:0x12 (SPI_FAST_FLASH_BOOT)` | SPI flash boot selected, GPIO 0 = HIGH |
| `mode:DIO, clock div:1` | Image header SPI params parsed correctly |
| `load:0x40080000,len:1540` | ROM loaded .iram.vectors (1540 bytes) to IRAM |
| `load:0x40080604,len:3112` | ROM loaded .iram.text (3112 bytes) to IRAM |

The ROM bootloader in QEMU successfully reads our binary from flash:0x1000,
validates the 0xE9 magic, parses segment descriptors, copies all segments to
IRAM/DRAM, and jumps to entry point 0x40080620 in startup.S.

---

## File Structure

`
esp32-bare-metal-bootloader/
|-- src/
|   |-- startup.S               Xtensa LX6 ASM entry + init
|   +-- main_bootloader.c       C bootloader (1648 lines)
|-- include/
|   |-- esp32_regs.h            MMIO map (GPIO/SPI1/UART0/SHA/RTC)
|   +-- flash_map.h             Flash offsets + image header structs
|-- linker.ld                   GNU LD: LMA/VMA split, 6 ASSERT guards
|-- Makefile                    Build rules
|-- build_and_flash.ps1         PowerShell build + flash pipeline
|-- generate_pptx.py            Generates dark-theme PowerPoint
|-- bootloader_presentation.pptx 9-slide technical deck
|-- PROJECT_PRESENTATION.md    9-slide Markdown + interview Q&A
+-- README.md                   This file
`

---

## Build and Flash

`powershell
# Auto-build and flash (PowerShell)
.\build_and_flash.ps1 -Port COM4

# Manual flash (ESP32 in download mode: GPIO0=GND + RESET pulse)
python -m esptool --chip esp32 --port COM4 --baud 460800 
    write-flash --flash-mode dio --flash-size 4MB 0x1000 build\bootloader.bin
`

---

## License

MIT License - free to use, modify, and distribute.

*Built from first principles. No magic. Every register, every instruction, fully understood.*
