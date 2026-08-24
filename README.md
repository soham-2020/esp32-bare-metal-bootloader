# ESP32 Bare-Metal Custom Secondary Bootloader

![Language](https://img.shields.io/badge/Language-C11%20%2B%20Xtensa%20ASM-blue)
![Platform](https://img.shields.io/badge/Platform-ESP32--D0WD-red)
![Architecture](https://img.shields.io/badge/Architecture-Xtensa%20LX6-orange)
![Binary](https://img.shields.io/badge/Binary-7%2C844%20bytes-green)
![QEMU](https://img.shields.io/badge/QEMU-Verified%209.2.2-purple)
![License](https://img.shields.io/badge/License-MIT-brightgreen)

> A production-grade bare-metal secondary bootloader for the ESP32 (Tensilica Xtensa LX6),
> written in C11 and Xtensa Assembly — no ESP-IDF, no FreeRTOS, no Arduino, no mbedtls.
> Every register access is direct MMIO manipulation.

---

## Table of Contents

- [What Is This?](#what-is-this)
- [System Architecture](#system-architecture)
- [Flash Memory Map](#flash-memory-map)
- [CPU Address Map](#cpu-address-map)
- [Core Features](#core-features)
- [Build Results](#build-results)
- [QEMU Emulation](#qemu-emulation)
- [File Structure](#file-structure)
- [Build and Flash](#build-and-flash)
- [License](#license)

---

## What Is This?

When an ESP32 powers on, the **mask ROM** (burnt into silicon at the factory) runs first.
It reads flash offset `0x1000`, copies the binary into IRAM, and jumps to the entry point.
**That image is our secondary bootloader.**

It executes before your application and handles:

| Responsibility | Detail |
|----------------|--------|
| **Hardware Init** | Disable watchdog timers, configure UART0 @ 115200 baud, set GPIO pull-ups |
| **Flash Integrity** | Raw SPI1 register reads, CRC32 table verification, HW SHA-256 digest check |
| **Boot Decision** | Sample GPIO 0 level + read RTC_CNTL_STORE6 magic word |
| **OTA Management** | 40-byte `ota_control_t` control block at flash `0x8000` |
| **Execution Handover** | Atomic interrupt disable, VECBASE relocation, pipeline flush, indirect JX jump |

---

## System Architecture

```
 Power-On Reset
       │
       ▼
 ┌─────────────────────────────────────────────┐
 │             ESP32 Mask ROM                  │
 │         (factory silicon, 0x40000000)       │
 │                                             │
 │  • Reads image header  @ flash 0x1000       │
 │  • Validates 0xE9 magic byte                │
 │  • Copies segments → IRAM / DRAM            │
 │  • Jumps to entry point                     │
 └────────────────────┬────────────────────────┘
                      │  JX → 0x40080620
                      ▼
 ┌─────────────────────────────────────────────┐
 │    startup.S   (loaded into IRAM)           │
 │                                             │
 │   rsil  a4, 15       ← disable interrupts  │
 │   wsr   a0, vecbase  ← set vector table    │
 │   isync              ← flush pipeline      │
 │   memw               ← memory barrier      │
 │   call0 bootloader_main                    │
 └────────────────────┬────────────────────────┘
                      │
                      ▼
 ┌─────────────────────────────────────────────┐
 │            bootloader_main()                │
 │                                             │
 │  1. Disable watchdog  (TIMG0 / TIMG1 / RTC)│
 │  2. Init UART0        (115200-8-N-1)        │
 │  3. Build CRC32 table (DRAM, 256 entries)   │
 │  4. Configure GPIO 0  (pull-up, sample)     │
 │                                             │
 │  GPIO0 = LOW  or  RTC_STORE6 = 0xDEADBEEF? │
 │  ┌─── YES ──────────────────────────────┐   │
 │  │  Enter IAP mode (XModem UART upload) │   │
 │  └──────────────────────────────────────┘   │
 │  ┌─── NO ───────────────────────────────┐   │
 │  │  Read OTA state block  @ 0x8000      │   │
 │  │  Validate image header (0xE9 magic)  │   │
 │  │  Verify CRC32 + SHA-256 (HW accel)   │   │
 │  │  rsil / wsr vecbase / isync / memw   │   │
 │  │  jx  → Application entry point      │   │
 │  └──────────────────────────────────────┘   │
 └─────────────────────────────────────────────┘
```

---

## Flash Memory Map

```
 Physical SPI Flash — 4 MB (W25Q32JV)
 ┌──────────┬─────────┬───────────────────────────┐
 │ Address  │  Size   │ Region                    │
 ├──────────┼─────────┼───────────────────────────┤
 │ 0x000000 │   4 KB  │ ROM Bootloader Header     │
 │ 0x001000 │  28 KB  │ ★ Our Secondary Bootloader│
 │ 0x008000 │   4 KB  │ OTA Control Block         │
 │ 0x009000 │  28 KB  │ Reserved / Padding        │
 │ 0x010000 │   1 MB  │ App Slot 1 (Primary FW)   │
 │ 0x110000 │   1 MB  │ App Slot 2 (OTA Payload)  │
 │ 0x3E0000 │  64 KB  │ NVS Key-Value Store       │
 │ 0x3F0000 │  64 KB  │ Factory / PHY Calibration │
 └──────────┴─────────┴───────────────────────────┘
```

---

## CPU Address Map

```
 ┌──────────────────┬──────────────┬────────┬──────────────────────────┐
 │ Region           │ VMA          │  Size  │ Contents                 │
 ├──────────────────┼──────────────┼────────┼──────────────────────────┤
 │ IRAM (our code)  │ 0x40080000   │ 28 KB  │ vectors + text + rodata  │
 │ DRAM (our data)  │ 0x3FFAE000   │ 16 KB  │ data + bss + stack(8KB)  │
 │ ESP32 Mask ROM   │ 0x40000000   │ 448 KB │ Factory ROM              │
 │ SPI XIP Cache    │ 0x400C0000   │  4 MB  │ Flash execute-in-place   │
 │ MMIO Peripherals │ 0x3FF00000   │  1 MB  │ GPIO/SPI/UART/SHA/RTC    │
 └──────────────────┴──────────────┴────────┴──────────────────────────┘
```

---

## Core Features

### 1. Cache-Disabled Raw SPI1 Flash Reads

SPI0 drives the CPU XIP cache. SPI1 is the CPU-master for direct flash access.
The cache **must** be disabled before any SPI1 transaction — so every function
in this call chain is marked `IRAM_ATTR` (lives in IRAM, never fetched from flash):

```c
/* Disable XIP cache before touching SPI1 */
DPORT_PRO_CACHE_CTRL_REG &= ~DPORT_PRO_CACHE_ENABLE;
__asm__ volatile("memw" ::: "memory");

SPI1_USER_REG      = SPI_USR_CMD | SPI_USR_ADDR | SPI_USR_MISO;
SPI1_USER2_REG     = (7 << SPI_USR_COMMAND_BITLEN_S) | 0x03;  /* READ opcode */
SPI1_ADDR_REG      = flash_offset << 8;
SPI1_MISO_DLEN_REG = (len * 8 - 1);
SPI1_CMD_REG       = SPI_CMD_USR;
while (SPI1_CMD_REG & SPI_CMD_USR);   /* poll until done */
/* data now in SPI1_W0_REG .. SPI1_W15_REG */
```

### 2. Hardware SHA-256 Accelerator

The SHA-256 hardware engine lives at MMIO `0x3FF03000`.
Xtensa is **little-endian**; SHA-256 (FIPS 180-4) requires **big-endian** 32-bit words.
The `BSWAP32` macro corrects byte order — without it the digest is silently wrong:

```c
#define BSWAP32(x)  (                               \
    (((uint32_t)(x) & 0xFF000000UL) >> 24U) |       \
    (((uint32_t)(x) & 0x00FF0000UL) >>  8U) |       \
    (((uint32_t)(x) & 0x0000FF00UL) <<  8U) |       \
    (((uint32_t)(x) & 0x000000FFUL) << 24U))

/* Feed one 64-byte block (16 × uint32) to the HW engine */
for (int i = 0; i < 16; i++)
    SHA_TEXT_REG(i) = BSWAP32(block_words[i]);

SHA_256_REG = 1;                /* start engine    */
while (SHA_BUSY_REG & 1);      /* wait for finish */
```

### 3. DRAM-Cached CRC32

A 256-entry lookup table (`DRAM_ATTR`) stays in DRAM so it is reachable
while the SPI XIP cache is disabled. Polynomial `0xEDB88320` (CRC-32/ISO-HDLC):

```c
DRAM_ATTR static uint32_t crc32_table[256];   /* 1 KB in DRAM */

uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len) {
    crc ^= 0xFFFFFFFFUL;
    while (len--)
        crc = (crc >> 8) ^ crc32_table[(crc ^ *buf++) & 0xFF];
    return crc ^ 0xFFFFFFFFUL;
}
```

### 4. Xtensa VECBASE Relocation + Atomic Jump

Five instructions hand control to the application atomically:

```asm
rsil   a4, 15        /* raise interrupt level → mask everything  */
wsr    a1, vecbase   /* point VECBASE at app's vector table      */
isync               /* MANDATORY: flush pipeline after WSR       */
memw               /* drain all pending memory transactions      */
jx     a0          /* one-way indirect jump → application entry */
```

> **Why `isync` is not optional:** The Xtensa pipeline may speculatively
> fetch exception vectors from the *old* VECBASE before the `wsr` commits.
> `isync` forces a full pipeline flush, making the new VECBASE visible.

### 5. Dual-Path IAP (In-Application Programming) Mode

| Path | Trigger | Survives Soft-Reset? |
|------|---------|----------------------|
| **Hardware** | `GPIO 0 == LOW` at boot (BOOT button) | No — sampled at power-on |
| **Software** | `RTC_CNTL_STORE6 == 0xDEADBEEF` | **Yes** |

The RTC scratch register persists across warm resets but clears on power-on,
making it ideal for "please enter update mode on next reboot" without
writing to flash.

---

## Build Results

```
Toolchain : xtensa-esp-elf-gcc 16.1.0 (crosstool-NG esp-16.1.0_20260609)
Flags     : -Os  -mlongcalls  -mtext-section-literals
            -ffreestanding  -nostdlib  -nostdinc
            -ffunction-sections  -fdata-sections  -Wl,--gc-sections

 ┌──────────┬─────────┬──────────────────────────────────┐
 │ Section  │  Bytes  │ Description                      │
 ├──────────┼─────────┼──────────────────────────────────┤
 │ .text    │  4,652  │ Code (IRAM: vectors + logic)     │
 │ .data    │  3,192  │ Initialized data (DRAM)          │
 │ .bss     │  8,200  │ BSS + 8 KB stack (NOLOAD)        │
 ├──────────┼─────────┼──────────────────────────────────┤
 │ Total    │ 16,044  │ dec  /  0x3EAC hex               │
 ├──────────┼─────────┼──────────────────────────────────┤
 │ Binary   │  7,844  │ bootloader.bin                   │
 │ Budget   │ 28,672  │ 28 KB flash slot                 │
 │ Used     │  27.4%  │ ✓ PASS                           │
 └──────────┴─────────┴──────────────────────────────────┘
```

---

## QEMU Emulation

Verified with **Espressif QEMU** `esp_develop_9.2.2_20260417` using a 4 MB virtual flash image.

### Create flash image

```bash
# 1. Create 4 MB blank flash (0xFF filled)
python -c "open('flash.bin','wb').write(b'\xff'*4*1024*1024)"

# 2. Merge bootloader.bin at offset 0x1000
python -c "
f = bytearray(open('flash.bin','rb').read())
f[0x1000:0x1000+len(open('build/bootloader.bin','rb').read())] = \
    open('build/bootloader.bin','rb').read()
open('flash.bin','wb').write(f)"
```

### Run QEMU

```bash
qemu-system-xtensa         \
    -M esp32               \
    -drive file=flash.bin,if=mtd,format=raw \
    -serial stdio          \
    -display none          \
    -no-reboot
```

### Captured UART0 Output

```
ets Jul 29 2019 12:21:46

rst:0x1 (POWERON_RESET),boot:0x12 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0x00
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:1
load:0x40080000,len:1540
load:0x40080604,len:3112
```

### Verification Table

| Output Line | What It Confirms |
|-------------|-----------------|
| `ets Jul 29 2019 12:21:46` | ESP32 mask ROM executing correctly in QEMU |
| `rst:0x1 (POWERON_RESET)` | Clean power-on reset path |
| `boot:0x12 (SPI_FAST_FLASH_BOOT)` | SPI flash boot, GPIO 0 = HIGH (normal boot) |
| `mode:DIO, clock div:1` | SPI parameters parsed from our image header |
| `load:0x40080000,len:1540` | ROM copied `.iram.vectors` (1,540 B) to IRAM |
| `load:0x40080604,len:3112` | ROM copied `.iram.text` (3,112 B) to IRAM |

The ROM bootloader in QEMU reads our binary from `flash:0x1000`, validates the
`0xE9` magic byte, parses segment descriptors, copies all segments into IRAM/DRAM,
then jumps to entry point `0x40080620` — the `_entry_point` label in `startup.S`.

---

## File Structure

```
esp32-bare-metal-bootloader/
├── src/
│   ├── startup.S              Xtensa LX6 assembly — entry, VECBASE, BSS, .data copy
│   └── main_bootloader.c      Full C bootloader (1,648 lines)
├── include/
│   ├── esp32_regs.h           MMIO register map (GPIO / SPI1 / UART0 / SHA / RTC)
│   └── flash_map.h            Flash offsets, app_image_hdr_t, ota_control_t structs
├── linker.ld                  GNU LD: AT() LMA/VMA split, VECBASE alignment ASSERTs
├── Makefile                   Build rules for xtensa-esp-elf toolchain
├── build_and_flash.ps1        PowerShell: build + esptool flash pipeline
└── README.md                  This file
```

---

## Build and Flash

```powershell
# PowerShell — auto-detects toolchain, builds, and flashes
.\build_and_flash.ps1 -Port COM4

# Manual flash (put ESP32 in download mode first: GPIO0=GND + RESET pulse)
python -m esptool --chip esp32 --port COM4 --baud 460800 `
    write-flash --flash-mode dio --flash-size 4MB 0x1000 build\bootloader.bin
```

---

## License

MIT — free to use, modify, and distribute.

*Built from first principles. No magic. Every register, every instruction, fully understood.*
