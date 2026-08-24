# ESP32 Bare-Metal Custom Secondary Bootloader Presentation

## Slide 1 — Title & Architectural Summary
**Title:** ESP32 Bare-Metal Custom Secondary Bootloader — Xtensa LX6 Silicon-Level Firmware

*   **Overview:** A custom bare-metal secondary bootloader for ESP32, bypassing standard frameworks to achieve minimal footprint, absolute hardware control, and custom secure boot.
*   **Architecture:** ROM Bootloader → `startup.S` (IRAM@0x40080000) → `bootloader_main()` → Context Handoff (JX) → User Application.
*   **Stats:** Extremely lightweight 7,844 bytes binary footprint.
*   **Implementation:** Written in pure C11 + Xtensa Assembly. Zero external framework dependencies (no ESP-IDF or Arduino core).

**Speaker Notes:** Welcome everyone. Today I'm presenting a bare-metal secondary bootloader for the ESP32. We bypass the ESP-IDF completely to gain full control over the boot process, resulting in a tiny 7.8KB binary that initializes the hardware and jumps to the application.

---

## Slide 2 — Problem Statement
**Title:** Problem Statement & Motivation

*   **Bypassing Frameworks:** High-level frameworks (Arduino/ESP-IDF) hide critical hardware details like VECBASE management, SPI bus arbitration, and window register initialization.
*   **Security Focus:** Need absolute guarantee of firmware integrity (SHA-256 + CRC32) before jumping to the application code, without relying on black-box framework implementations.
*   **Full Control:** Achieving deterministic boot times and predictable execution without RTOS scheduler overhead during the critical boot phase.

**Speaker Notes:** Why build this? Because frameworks hide the real hardware. By writing this bare-metal, we take ownership of the vector table, SPI flash controller, and security engine. This guarantees deterministic boot times and allows us to implement our own firmware verification without RTOS interference.

---

## Slide 3 — Memory Architecture & Linker Mechanics
**Title:** Memory Architecture & Linker Mechanics

*   **Flash Map:** 0x1000 (BL 28KB), 0x8000 (PartTable), 0x010000 (App1 1MB), 0x110000 (App2 1MB).
*   **SRAM Layout:** IRAM: `0x40080000` (code+vectors), DRAM: `0x3FFAE000` (data+bss+stack 8KB).
*   **LMA vs VMA:** Handled using `AT()` directive in `linker.ld` and `LOADADDR()` for `_data_lma` to load data from flash to RAM.
*   **VECBASE Alignment:** 256-byte alignment enforced: `ASSERT((_bl_vecbase & 0xFF) == 0)`
*   **Section Placement:** `.iram.vectors` first (256-byte aligned), then `.iram.text`, `.iram.rodata`, `.dram.data` (NOLOAD: `.bss`, `.stack`).

```ld
/* Linker Snippet */
.iram.vectors : ALIGN(256) {
    _bl_vecbase = .;
    *(.iram.vectors)
} > IRAM_SEG
```

**Speaker Notes:** Memory layout is critical. The linker script defines the LMA in flash and VMA in SRAM. We must ensure the Xtensa vector base is 256-byte aligned. The bootloader places its vectors first in IRAM, followed by its code, while data lives in DRAM.

---

## Slide 4 — Raw SPI1 Silicon Control
**Title:** Raw SPI1 Silicon Control

*   **SPI0 vs SPI1:** SPI0 is the cache master (XIP), while SPI1 is the CPU master (raw firmware control).
*   **Cache Disable:** `DPORT_PRO_CACHE_CTRL` bit[0]=0 followed by a `MEMW` barrier to safely access flash directly.
*   **SPI1 USR-Mode:** Programming registers directly:
    *   `SPI1_USER_REG`: CMD|ADDR|MISO phases
    *   `SPI1_USER1_REG`: `23<<26` (24-bit address)
    *   `SPI1_USER2_REG`: `(7<<28) | 0x03` (Read Opcode)
    *   `SPI1_ADDR_REG`: Address shifted left by 8
*   **IRAM_ATTR Enforcement:** ALL functions in the call chain must reside in IRAM during the cache-off window to prevent fatal fetch errors.
*   **Data Extraction:** Reading `SPI1_W0..W15` with little-endian extraction.

**Speaker Notes:** To read flash directly, we must disable the cache and take over SPI1. Because the cache is off, any code executed must be in IRAM. We manually construct the SPI transactions using the USR-mode registers to read our application image.

---

## Slide 5 — Hardware Security Engine
**Title:** Hardware Security Engine

*   **SHA-256:** Hardware accelerator at `0x3FF03000`, processing 64-byte blocks.
*   **Endianness Bug:** Xtensa is Little-Endian, SHA engine expects Big-Endian. Requires `BSWAP32()` macro: `(x>>24)|((x>>8)&0xFF00)|((x<<8)&0xFF0000)|(x<<24)`.
*   **FIPS 180-4 Padding:** `0x80` + zeros + 64-bit big-endian bit count. Requires a two-block case when remainder >= 56 bytes.
*   **Chaining:** Utilizing `SHA_256_START` vs `SHA_256_CONTINUE` for Merkle-Damgård chaining.
*   **CRC32:** Slice-by-1 algorithm with a 256-entry `DRAM_ATTR` table (Polynomial `0xEDB88320`).
*   **Constant-Time Compare:** XOR all 32 digest bytes, check `diff==0` to prevent timing attacks.

**Speaker Notes:** We use the ESP32's hardware SHA-256 for fast image verification. A key gotcha is the endianness mismatch between the Xtensa core and the SHA engine, requiring explicit byte-swapping. We also implement a constant-time comparison to thwart timing attacks.

---

## Slide 6 — Execution Context Handover
**Title:** Execution Context Handover

*   **Xtensa Vectors:** Unlike ARM (table of addresses), Xtensa uses executable code at fixed offsets from `VECBASE`.
*   **Handoff Sequence:**
    *   `rsil a4,15`: Atomic interrupt mask (`PS.INTLEVEL=15`)
    *   `wsr %1,vecbase`: Relocate VECBASE to app's vector table
    *   `isync`: MANDATORY pipeline flush
    *   `memw`: Memory barrier (drain AHB/APB pending stores)
    *   `jx %0`: Register-indirect jump (one-way, non-returning)
*   **Register Window Clean Init:** `WINDOWBASE=0`, `WINDOWSTART=0x1`, `PS.WOE=1`.
*   **Startup Sequence:** `RSIL` → `VECBASE` → `ISYNC` → Window Init → SP Init → BSS Zero → `.data` Copy → `CALL0`.

```assembly
/* Context Handover */
rsil a4, 15
wsr  a2, vecbase
isync
memw
jx   a3
```

**Speaker Notes:** Jumping to the application is a delicate dance. We must mask interrupts, set the new vector base, and most importantly, issue an `isync` to flush the pipeline. We use a `jx` instruction for a clean, non-returning jump to the application's entry point.

---

## Slide 7 — Dual-Path IAP Mode
**Title:** Dual-Path IAP (In-Application Programming) Mode

*   **Path A (Hardware):** GPIO0 hardware trigger. Configures `IO_MUX_GPIO` pull-up, 10ms debounce. Sample LOW = Enter IAP.
*   **Path B (Software):** `RTC_CNTL_STORE6` (`0x3FF48000`) magic = `0xDEADBEEF`. Survives soft reset. Cleared immediately after read to prevent infinite IAP loops.
*   **IAP Mode Execution:** Sends 'C' (XModem-CRC ready signal) on UART0, loops waiting for new firmware upload.
*   **Fallback Chain:** Slot2 fail → Slot1 → IAP mode.
*   **OTA State:** 40-byte `ota_control_t` with magic (`0xCAFEBABE`), active_slot, state machine, and CRC32.

**Speaker Notes:** We support two ways to enter firmware update mode: holding GPIO0 low during boot, or a software trigger via RTC memory. We clear the RTC magic word immediately to ensure we don't get stuck in an update loop if a crash occurs.

---

## Slide 8 — QEMU Emulation & Verification
**Title:** QEMU Emulation & Verification

*   **Command:** `qemu-system-xtensa -M esp32 -m 4M -drive file=flash.bin,if=mtd,format=raw -serial stdio -display none`
*   **Flash Image:** Created 4MB zeroed file + bootloader.bin merged at offset `0x1000`.
*   **Expected UART Sequence:**
    *   `[BL] Secondary Bootloader v1.0.0 banner`
    *   `[BL] CRC32 table ready`
    *   `[BL] Normal boot mode`
    *   `[BL] OTA state absent — defaulting to Slot 1`
    *   `[BL-ERR] Bad image magic` (Expected in emulation without app)
    *   `[BL] Entering IAP mode`
*   **Verification:** QEMU proves `startup.S` execution, VECBASE relocation, UART output, and IRAM/DRAM mapping correctness.

**Speaker Notes:** We validate the bootloader using QEMU. This allows us to simulate the memory map and test the bootloader's fallback logic—like successfully entering IAP mode when the flash is empty—without touching physical hardware.

---

## Slide 9 — Interview Q&A
**Title:** Interview Q&A

1.  **Why must `IRAM_ATTR` be used for functions called during cache disable?**
    If code is in flash, a cache miss while the cache is disabled causes a fatal exception. Code must be pre-loaded in IRAM.
2.  **Why is `ISYNC` mandatory after `WSR VECBASE`?**
    It flushes the processor pipeline. Without it, the CPU might speculatively fetch interrupts using the old `VECBASE`.
3.  **How does Xtensa's vector table differ from ARM Cortex-M?**
    ARM uses a table of addresses (pointers). Xtensa places actual executable instructions at fixed offsets from `VECBASE`.
4.  **Why does SHA-256 hardware require byte-swapping on Xtensa?**
    The Xtensa LX6 core is Little-Endian, but the ESP32 SHA peripheral expects Big-Endian data words.
5.  **What happens if `VECBASE` is not 256-byte aligned?**
    The CPU ignores the lower 8 bits. If unaligned, the CPU calculates incorrect offsets, leading to immediate crashes on exceptions.
6.  **Why use `CALL0` in `startup.S` instead of `CALL4`/`CALL8`?**
    Register windowing (`PS.WOE`) isn't set up yet. `CALL0` uses standard link-register (`a0`) subroutine calls without window rotation.
7.  **How does the `AT()` directive in the linker script work for LMA/VMA splits?**
    It specifies the Load Memory Address (Flash) vs Virtual Memory Address (SRAM). The startup code copies data from LMA to VMA.
8.  **Why is `MEMW` needed before the `JX` instruction?**
    It forces the CPU to wait until all pending memory writes (like the `.data` segment copy or peripheral writes) are flushed to the bus.
9.  **How does `RTC_CNTL_STORE6` survive a software reset but not a power-on reset?**
    It's powered by the RTC domain (Vdd_RTC), which remains powered during a CPU core reset but loses state on full power loss.
10. **What is the difference between `RSIL` and `WAITI` on Xtensa?**
    `RSIL` reads the current interrupt level and sets a new one (masking interrupts). `WAITI` puts the CPU to sleep until an interrupt occurs.

**Speaker Notes:** These are the technical details that separate basic developers from advanced firmware engineers. Understanding the silicon pipeline, memory bus behavior, and Xtensa specific architecture is crucial for writing a stable bare-metal bootloader.
