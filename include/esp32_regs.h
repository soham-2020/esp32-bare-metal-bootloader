/**
 * @file    esp32_regs.h
 * @brief   Bare-metal MMIO register map for ESP32-D0WD / ESP32-CAM.
 *
 * Every address in this file is sourced from:
 *   Espressif ESP32 Technical Reference Manual v5.1 (TRM)
 *   https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf
 *
 * Architecture notes — Tensilica Xtensa LX6:
 * ─────────────────────────────────────────────────────────────────────────
 *  • Harvard-modified load/store architecture. ALL peripheral access is
 *    via ordinary load/store instructions to the data bus (DBUS).
 *    There are NO dedicated I/O port instructions (unlike x86 IN/OUT).
 *
 *  • Two separate virtual bus windows:
 *      IBUS  (Instruction Bus): 0x40000000 – 0x4FFFFFFF
 *      DBUS  (Data Bus)       : 0x00000000 – 0x3FFFFFFF
 *    Peripherals are always in the DBUS window (< 0x40000000).
 *
 *  • Peripheral registers are 32-bit wide and must be accessed as 32-bit
 *    words. Sub-word (byte/halfword) accesses are NOT supported on most
 *    ESP32 peripheral registers and will produce incorrect results or faults.
 *
 *  • The `volatile` keyword is mandatory on MMIO pointers. Without it the
 *    compiler may cache the register value in a CPU register across multiple
 *    reads, breaking polling loops (e.g., SPI busy-wait).
 * ─────────────────────────────────────────────────────────────────────────
 */

#ifndef ESP32_REGS_H
#define ESP32_REGS_H

#include <stdint.h>

/* =========================================================================
 * PRIMITIVE MMIO ACCESS MACROS
 *
 * REG_READ  – dereferences a volatile uint32_t pointer at `addr`.
 *             Generates a single L32I (load 32-bit immediate-indexed)
 *             instruction on Xtensa, which is a 3-cycle operation with
 *             no speculative execution or caching.
 *
 * REG_WRITE – generates a single S32I (store 32-bit immediate-indexed).
 *             The hardware guarantees the write reaches the peripheral
 *             register file within one APB bus cycle (~12.5 ns @ 80 MHz).
 *
 * REG_SET_BIT / REG_CLR_BIT – read-modify-write. NOT atomic.
 *   If an interrupt fires between the read and write, the intermediate
 *   state could be corrupted. Always disable interrupts around RMW
 *   sequences on shared registers.
 * ========================================================================= */

/** Dereference addr as a volatile 32-bit register (read) */
#define REG_READ(addr)           (*((volatile uint32_t *)(uintptr_t)(addr)))

/** Write val to the volatile 32-bit register at addr */
#define REG_WRITE(addr, val)     (*((volatile uint32_t *)(uintptr_t)(addr)) = (uint32_t)(val))

/** Read-modify-write: set one or more bits in register at addr */
#define REG_SET_BIT(addr, bit)   REG_WRITE((addr), REG_READ(addr) | (uint32_t)(bit))

/** Read-modify-write: clear one or more bits in register at addr */
#define REG_CLR_BIT(addr, bit)   REG_WRITE((addr), REG_READ(addr) & ~(uint32_t)(bit))

/** Extract a field: shift right by `sh`, mask to `msk` bits wide */
#define REG_GET_FIELD(addr, sh, msk)  ((REG_READ(addr) >> (sh)) & (msk))

/* =========================================================================
 * DPORT — System/Peripheral Configuration Bus
 * TRM §3, Base: 0x3FF00000
 *
 * DPORT is a 128 KB register window that controls:
 *   • Clock gating (enable/disable peripheral APB clocks)
 *   • Peripheral reset
 *   • PRO-CPU and APP-CPU cache controllers
 *   • Interrupt matrix routing
 *   • CPU stall / start control for dual-core operations
 *
 * The SPI0/SPI1 flash cache is managed through this block.
 * Disabling the PRO_CACHE stops the SPI0 XIP (execute-in-place) engine,
 * which MUST happen before we drive SPI1 directly.
 * ========================================================================= */
#define DPORT_BASE                  0x3FF00000UL

/**
 * PRO-CPU cache control register (TRM §3.4, offset 0x040)
 *
 * Bit layout (relevant bits):
 *   [0]  PRO_CACHE_ENABLE   — 1 = XIP cache active, 0 = XIP cache disabled
 *   [1]  PRO_CACHE_FLUSH_ENA — write 1 to trigger a cache flush
 *   [8]  PRO_DRAM_HL        — DRAM high/low boundary control
 *
 * Write sequence to disable cache:
 *   1. REG_CLR_BIT(DPORT_PRO_CACHE_CTRL, BIT(0))   ← disable
 *   2. REG_SET_BIT(DPORT_PRO_CACHE_CTRL, BIT(1))   ← flush
 *   3. Poll bit[1] until hardware clears it          ← flush done
 *   4. REG_SET_BIT(DPORT_PRO_CACHE_CTRL, BIT(0))   ← re-enable
 */
#define DPORT_PRO_CACHE_CTRL        (DPORT_BASE + 0x040UL)

/** APP-CPU cache control (same layout as PRO_CACHE_CTRL, TRM §3.4) */
#define DPORT_APP_CACHE_CTRL        (DPORT_BASE + 0x058UL)

/** Peripheral clock enable register (TRM §3.2) */
#define DPORT_PERIP_CLK_EN          (DPORT_BASE + 0x1C0UL)

/** Peripheral reset enable register (TRM §3.2) */
#define DPORT_PERIP_RST_EN          (DPORT_BASE + 0x1C4UL)

/* Cache control bit fields */
#define DPORT_PRO_CACHE_ENABLE_BIT  (1UL << 0)   /**< XIP cache ON/OFF */
#define DPORT_PRO_CACHE_FLUSH_BIT   (1UL << 1)   /**< Cache flush trigger */

/* =========================================================================
 * GPIO — General Purpose Input/Output
 * TRM §4, Base: 0x3FF44000
 *
 * ESP32 has 40 GPIO pads. Routing is via a signal matrix:
 *   Physical pad ↔ IO_MUX register selects which peripheral function
 *   drives/reads the pad. Function 2 in IO_MUX routes through GPIO matrix,
 *   giving pure software GPIO control.
 *
 * GPIO registers are split into two banks:
 *   Bank 0: GPIO 0–31  (registers ending in _REG or _W1TS_REG)
 *   Bank 1: GPIO 32–39 (registers ending in 1_REG or 1_W1TS_REG)
 *
 * W1TS = Write-1-To-Set   : writing a 1-bit SETS   the corresponding bit
 * W1TC = Write-1-To-Clear : writing a 1-bit CLEARS the corresponding bit
 * W1TS/W1TC are atomic without needing read-modify-write.
 * ========================================================================= */
#define GPIO_BASE                   0x3FF44000UL

/** GPIO output register, bank 0 (GPIO 0–31). Read = current driven level. */
#define GPIO_OUT_REG                (GPIO_BASE + 0x004UL)

/** GPIO output set (W1TS), bank 0. Write bit N → drive GPIO N HIGH */
#define GPIO_OUT_W1TS_REG           (GPIO_BASE + 0x008UL)

/** GPIO output clear (W1TC), bank 0. Write bit N → drive GPIO N LOW */
#define GPIO_OUT_W1TC_REG           (GPIO_BASE + 0x00CUL)

/** GPIO input register, bank 0. Read bit N → sampled level of GPIO N */
#define GPIO_IN_REG                 (GPIO_BASE + 0x03CUL)

/** GPIO input register, bank 1 (GPIO 32–39) */
#define GPIO_IN1_REG                (GPIO_BASE + 0x040UL)

/** GPIO output enable set (W1TS), bank 0. Write bit N → enable output on GPIO N */
#define GPIO_ENABLE_W1TS_REG        (GPIO_BASE + 0x024UL)

/** GPIO output enable clear (W1TC), bank 0. Write bit N → make GPIO N input */
#define GPIO_ENABLE_W1TC_REG        (GPIO_BASE + 0x028UL)

/**
 * GPIO pin configuration registers (TRM §4.7, one per GPIO, bank 0).
 * Each register controls interrupt type, pad driver, and open-drain mode.
 * Offset from base: 0x88 + gpio_num * 4
 */
#define GPIO_PIN_REG(n)             (GPIO_BASE + 0x088UL + ((uint32_t)(n) * 4UL))

/* ── IO_MUX pad configuration (TRM §4.10) ─────────────────────────────── */

/**
 * IO_MUX Base: 0x3FF49000
 *
 * Each GPIO pad has one IO_MUX register that independently controls:
 *   [14:12] MCU_SEL (Function Select): selects which peripheral drives the pad
 *           0 = SDIO / special, 1 = native peripheral, 2 = GPIO matrix, ...
 *   [9]     FUN_IE  : Input buffer enable (1 = pad samples input)
 *   [8]     FUN_WPU : Internal pull-up enable  (~47 kΩ to VDD)
 *   [7]     FUN_WPD : Internal pull-down enable (~47 kΩ to GND)
 *   [11:10] FUN_DRV : Drive strength (0=5mA, 1=10mA, 2=20mA, 3=40mA)
 *   [0]     MCU_OE  : Output enable in sleep mode
 *
 * To configure GPIO 0 as input with pull-up via GPIO matrix:
 *   REG_WRITE(IO_MUX_GPIO(0),
 *             (2 << 12) |   ← MCU_SEL = GPIO matrix function
 *             (1 << 9)  |   ← FUN_IE  = input enable
 *             (1 << 8));    ← FUN_WPU = pull-up enable
 */
#define IO_MUX_BASE                 0x3FF49000UL
#define IO_MUX_GPIO(n)              (IO_MUX_BASE + 0x044UL + ((uint32_t)(n) * 4UL))

/* IO_MUX field definitions */
#define IO_MUX_MCU_SEL_SHIFT        12             /**< Function select field bit offset */
#define IO_MUX_GPIO_FUNC            (2UL << 12)    /**< Select GPIO matrix (function 2) */
#define IO_MUX_FUN_IE               (1UL << 9)     /**< Input buffer enable */
#define IO_MUX_FUN_WPU              (1UL << 8)     /**< Internal pull-up enable */
#define IO_MUX_FUN_WPD              (1UL << 7)     /**< Internal pull-down enable */
#define IO_MUX_FUN_DRV_SHIFT        10             /**< Drive strength field bit offset */

/* =========================================================================
 * SPI1 — Software-Controlled SPI Flash Controller
 * TRM §7, Base: 0x3FF42000
 *
 * CRITICAL ARCHITECTURE NOTE:
 * ─────────────────────────────────────────────────────────────────────────
 *  The ESP32 flash interface has TWO SPI controller instances:
 *
 *   SPI0 (base 0x3FF43000): The "cache master". Automatically driven by
 *     the PRO/APP CPU XIP (execute-in-place) cache engine. NEVER write to
 *     SPI0 registers from firmware — the cache controller owns this bus.
 *     Its registers are listed here for reference only.
 *
 *   SPI1 (base 0x3FF42000): The "CPU master". Firmware-controlled.
 *     Shares the same 4-wire QSPI pins as SPI0 via an internal arbiter.
 *     To use SPI1 without corrupting cache state:
 *       1. Disable SPI0 cache (DPORT_PRO_CACHE_CTRL bit 0 → 0)
 *       2. Issue all SPI1 transactions
 *       3. Re-enable SPI0 cache
 *     All code executing during this window MUST be in IRAM.
 *
 * SPI1 USER-mode transaction phases (controlled by SPI_USER_REG):
 *   Phase order: [CMD] → [ADDR] → [DUMMY] → [MOSI/MISO]
 *   Each phase is independently enabled via SPI_USER_REG bits.
 *   Phase lengths are set in SPI_USER1_REG and SPI_USER2_REG.
 *   Data is transferred via SPI_W0..W15 registers (64-byte FIFO).
 * ========================================================================= */
#define SPI1_BASE                   0x3FF42000UL

/* ── SPI1 Register Map ──────────────────────────────────────────────────── */

/**
 * SPI_CMD_REG (offset 0x000) — Command register / transaction control
 *
 * Bit layout (write-1-to-trigger, hardware auto-clears):
 *   [18] USR  — Start a USR-mode transaction (main trigger bit)
 *   Other bits trigger specific built-in commands (READ, WRITE, etc.)
 *   We only use bit 18 (USR mode) for full manual control.
 */
#define SPI1_CMD_REG                (SPI1_BASE + 0x000UL)
#define SPI1_CMD_USR                (1UL << 18)  /**< Start USR-mode transaction */

/**
 * SPI_ADDR_REG (offset 0x004) — Flash address for the transaction
 *
 * For 24-bit address SPI commands:
 *   The address is placed in bits [31:8] (left-justified).
 *   Hardware extracts ADDR_BITLEN+1 bits from the MSB side and shifts
 *   them out on MOSI MSB-first.
 *
 * So to send 24-bit address 0xABCDEF:
 *   REG_WRITE(SPI1_ADDR_REG, 0xABCDEF << 8)
 */
#define SPI1_ADDR_REG               (SPI1_BASE + 0x004UL)

/**
 * SPI_CTRL_REG (offset 0x008) — SPI mode and data format control
 *
 * Key bits:
 *   [25] WR_BIT_ORDER : 0 = MSB first (standard), 1 = LSB first
 *   [24] RD_BIT_ORDER : 0 = MSB first (standard), 1 = LSB first
 *   [15] QIO_MODE     : 1 = QSPI I/O mode (4 data lines)
 *   [14] DIO_MODE     : 1 = Dual I/O mode  (2 data lines)
 *   [13] QOUT_MODE    : 1 = Quad output mode
 *   [12] DOUT_MODE    : 1 = Dual output mode
 *   [10] FASTRD_MODE  : 1 = Fast read (with dummy cycles)
 */
#define SPI1_CTRL_REG               (SPI1_BASE + 0x008UL)

/**
 * SPI_USER_REG (offset 0x01C) — USR-mode phase enable/disable register
 *
 * This is the master switch for each transaction phase.
 * Set the corresponding bit to INCLUDE that phase in the transaction.
 * Clear the bit to SKIP that phase.
 *
 * Bit layout (relevant bits):
 *   [31] SPI_USR_COMMAND  — Include 8-bit command phase (sends opcode byte)
 *   [30] SPI_USR_ADDR     — Include address phase
 *   [29] SPI_USR_DUMMY    — Include dummy clock cycles (for FAST_READ)
 *   [28] SPI_USR_MISO     — Include MISO phase (receive data from flash)
 *   [27] SPI_USR_MOSI     — Include MOSI phase (send data to flash)
 *   [26] SPI_USR_MISO_HIGHPART — Use high half of data buffer (W8..W15)
 *   [9]  SPI_CK_OUT_EDGE  — clock edge polarity for output
 *   [7]  SPI_CS_HOLD      — keep CS asserted after last byte (for multi-tx)
 *   [6]  SPI_CS_SETUP     — extra CS setup time before clock
 */
#define SPI1_USER_REG               (SPI1_BASE + 0x01CUL)
#define SPI1_USR_COMMAND            (1UL << 31)  /**< Enable command phase */
#define SPI1_USR_ADDR               (1UL << 30)  /**< Enable address phase */
#define SPI1_USR_DUMMY              (1UL << 29)  /**< Enable dummy cycles */
#define SPI1_USR_MISO               (1UL << 28)  /**< Enable MISO (read) phase */
#define SPI1_USR_MOSI               (1UL << 27)  /**< Enable MOSI (write) phase */

/**
 * SPI_USER1_REG (offset 0x020) — USR-mode bit-length register for ADDR/DUMMY
 *
 * Field layout:
 *   [31:26] SPI_USR_ADDR_BITLEN  — Address phase length - 1 (e.g., 23 for 24 bits)
 *   [25:0]  SPI_USR_DUMMY_CYCLELEN — Dummy clock cycles - 1 (e.g., 7 for 8 cycles)
 *
 * For standard READ (0x03) with 24-bit address, no dummy cycles:
 *   SPI_USR_ADDR_BITLEN  = 23  → (23 << 26)
 *   SPI_USR_DUMMY_CYCLELEN = 0 (bit 29 of USER_REG is also cleared)
 */
#define SPI1_USER1_REG              (SPI1_BASE + 0x020UL)
#define SPI1_USER1_ADDR_BITLEN_SHIFT 26          /**< Address length field shift */

/**
 * SPI_USER2_REG (offset 0x024) — USR-mode command byte and bit length
 *
 * Field layout:
 *   [31:28] SPI_USR_COMMAND_BITLEN — Command phase length - 1 (7 for 8-bit cmd)
 *   [15:0]  SPI_USR_COMMAND_VALUE  — Actual command opcode byte
 *
 * Example for READ (0x03):
 *   REG_WRITE(SPI1_USER2_REG, (7UL << 28) | 0x03UL)
 */
#define SPI1_USER2_REG              (SPI1_BASE + 0x024UL)
#define SPI1_USER2_CMD_BITLEN_SHIFT 28           /**< Command length field shift */

/**
 * SPI_MOSI_DLEN_REG (offset 0x028) — MOSI data phase length (bits - 1)
 * Set to 0 when no MOSI data is sent (read-only transactions).
 */
#define SPI1_MOSI_DLEN_REG          (SPI1_BASE + 0x028UL)

/**
 * SPI_MISO_DLEN_REG (offset 0x02C) — MISO data phase length (bits - 1)
 *
 * For reading N bytes from flash:
 *   REG_WRITE(SPI1_MISO_DLEN_REG, (N * 8) - 1)
 *
 * Maximum per transaction: 64 bytes → (64*8)-1 = 511 bits
 */
#define SPI1_MISO_DLEN_REG          (SPI1_BASE + 0x02CUL)

/**
 * SPI_W0_REG .. SPI_W15_REG (offsets 0x080..0x0BC) — 64-byte data FIFO
 *
 * This is a 16-word (64-byte) data buffer shared between MOSI and MISO.
 * After a MISO transaction completes, the received bytes are in W0..W15.
 *
 * Byte ordering within each word:
 *   The FIRST received byte lands in bits [7:0] of SPI_W0 (LSB),
 *   the SECOND in bits [15:8], the THIRD in bits [23:16], etc.
 *   This is LITTLE-ENDIAN byte order within each 32-bit register word.
 *
 * Example: reading 4 bytes [0xAA, 0xBB, 0xCC, 0xDD] gives:
 *   SPI_W0 = 0xDDCCBBAA
 */
#define SPI1_W0_REG                 (SPI1_BASE + 0x080UL)
/** Get address of SPI1 data word register N (0–15) */
#define SPI1_Wn_REG(n)              (SPI1_BASE + 0x080UL + ((uint32_t)(n) * 4UL))

/**
 * SPI_EXT2_REG (offset 0x0F8) — SPI state machine status register
 *
 * Bits [2:0] = ST: State machine state
 *   0x0 = IDLE (transaction complete)
 *   Non-zero = transaction in progress
 *
 * Poll this (or bit 18 of CMD_REG) to detect transaction completion.
 */
#define SPI1_EXT2_REG               (SPI1_BASE + 0x0F8UL)
#define SPI1_EXT2_ST_MASK           0x7UL        /**< State machine state field */

/* Standard SPI NOR flash opcodes */
#define FLASH_OP_READ               0x03U  /**< Slow read (no dummy cycles) */
#define FLASH_OP_FAST_READ          0x0BU  /**< Fast read (8 dummy cycles) */
#define FLASH_OP_RDSR               0x05U  /**< Read status register */
#define FLASH_OP_WREN               0x06U  /**< Write enable */
#define FLASH_OP_SECTOR_ERASE       0x20U  /**< Erase 4KB sector */
#define FLASH_OP_PAGE_PROG          0x02U  /**< Program 256-byte page */
#define FLASH_OP_JEDEC_ID           0x9FU  /**< Read JEDEC manufacturer ID */

/** Maximum bytes per SPI1 transaction (64-byte FIFO hardware limit) */
#define SPI1_MAX_TRANSFER_BYTES     64U

/* =========================================================================
 * UART0 — Universal Asynchronous Receiver/Transmitter 0
 * TRM §13, Base: 0x3FF40000
 *
 * Used exclusively for bootloader debug output (blocking, polled TX).
 * No RX, interrupts, or DMA are used in this implementation.
 *
 * Clock source: APB clock (typically 80 MHz after PLL lock by ROM BL).
 * Baud rate = APB_CLK / UART_CLKDIV[19:4]  (integer divider, 16-bit)
 *             + fractional part in UART_CLKDIV[3:0] (4-bit fraction)
 *
 * For 115200 baud @ 80 MHz APB:
 *   divider = 80,000,000 / 115,200 = 694.44...
 *   Integer part = 694 (0x2B6)
 *   Fractional part ≈ 0 (ignored for this accuracy level)
 *   Register value: (694 << 4) | 0 = 0x2B60
 * ========================================================================= */
#define UART0_BASE                  0x3FF40000UL

/**
 * UART_FIFO_REG (offset 0x000) — TX/RX FIFO access register
 *
 * Write bits [7:0] → byte pushed into TX FIFO.
 * Read  bits [7:0] → byte popped from RX FIFO.
 * TX FIFO depth: 128 bytes. RX FIFO depth: 128 bytes.
 */
#define UART0_FIFO_REG              (UART0_BASE + 0x000UL)

/**
 * UART_STATUS_REG (offset 0x01C) — FIFO fill levels and error flags
 *
 * Field layout:
 *   [30:23] TXFIFO_CNT — Number of bytes currently in TX FIFO (0–127)
 *   [19:12] RXFIFO_CNT — Number of bytes currently in RX FIFO (0–127)
 *   [7]     TXD        — Level on the TX pin (after inversion if enabled)
 *   [3]     BRK_DET    — Break condition detected
 *   [2]     FRM_ERR    — Framing error
 *   [1]     OVF        — RX FIFO overflow
 *   [0]     RXFIFO_FULL— RX FIFO full
 */
#define UART0_STATUS_REG            (UART0_BASE + 0x01CUL)
#define UART0_TXFIFO_CNT_SHIFT      16            /**< TX FIFO count field shift */
#define UART0_TXFIFO_CNT_MASK       0x7FU         /**< TX FIFO count mask (7 bits) */
#define UART0_TXFIFO_FULL_LEVEL     127U          /**< Stall TX if count >= this */

/**
 * UART_CLKDIV_REG (offset 0x014) — Baud rate divisor register
 *
 * Bits [19:4] = integer divisor (16 bits)
 * Bits [3:0]  = fractional divisor (4 bits, in 1/16th increments)
 *
 * Baud rate = APB_CLK_FREQ / (CLKDIV[19:4] + CLKDIV[3:0]/16.0)
 */
#define UART0_CLKDIV_REG            (UART0_BASE + 0x014UL)

/**
 * UART_CONF0_REG (offset 0x020) — Frame format and loopback control
 *
 * Key bits (ESP32 reset values already set 8N1):
 *   [5:4] STOP_BIT_NUM — 0x1 = 1 stop bit, 0x2 = 1.5, 0x3 = 2
 *   [3:2] BIT_NUM      — 0x0 = 5-bit, 0x3 = 8-bit data
 *   [0]   PARITY_EN    — 0 = no parity (default)
 *
 * Reset default = 8N1 → no explicit write needed for basic operation.
 */
#define UART0_CONF0_REG             (UART0_BASE + 0x020UL)

/**
 * Baud rate divisor for 115200 @ APB=80 MHz.
 * Integer = 694, fractional = 0.
 * CLKDIV register = (694 << 4) | 0 = 0x2B60
 */
#define UART0_BAUD_115200_DIV       ((694U << 4) | 0U)

/* =========================================================================
 * SHA-256 HARDWARE ACCELERATOR
 * TRM §24.3, Base: 0x3FF03000
 *
 * The ESP32 silicon includes a dedicated SHA accelerator connected to the
 * crypto DMA bus. It computes SHA-256 compliant with FIPS 180-4.
 *
 * Operation sequence:
 * ─────────────────────────────────────────────────────────────────────────
 *  1. Write 64 bytes (512 bits = 1 SHA-256 block) to SHA_TEXT_BASE.
 *     Each 32-bit write covers 4 bytes. Data is treated BIG-ENDIAN by the
 *     hardware. Since the Xtensa LX6 is LITTLE-ENDIAN, you MUST byte-swap
 *     each 32-bit word before writing using __builtin_bswap32().
 *
 *  2. Trigger the engine:
 *     FIRST block:       Write 1 to SHA_256_START     (initialises IV)
 *     SUBSEQUENT blocks: Write 1 to SHA_256_CONTINUE  (chains the state)
 *
 *  3. Poll SHA_256_BUSY until it reads 0x0 (engine is idle).
 *
 *  4. After ALL blocks have been processed, write 1 to SHA_256_LOAD
 *     and poll SHA_256_BUSY again. This causes the engine to copy
 *     the final hash state into the result registers.
 *
 *  5. Read 32 bytes (8 × 32-bit words) from SHA_256_RESULT_BASE.
 *     These words are in BIG-ENDIAN order. Byte-swap each word back
 *     to little-endian if you need the hash in LSB-first form.
 *
 * SHA-256 padding (FIPS 180-4 §5.1.1) MUST be applied by the caller:
 *   • Append 0x80 byte after the last data byte
 *   • Zero-pad until 56 bytes (8 bytes before end of 64-byte block)
 *   • Append 64-bit big-endian message length IN BITS (not bytes)
 *   If the padding doesn't fit in the last data block, an extra all-
 *   padding block must be sent.
 * ========================================================================= */
#define SHA_BASE                    0x3FF03000UL

/**
 * SHA_TEXT_BASE (offset 0x000) — 64-byte message block input window.
 * 16 consecutive 32-bit registers. Write the 64-byte padded message block here
 * BEFORE triggering START or CONTINUE.
 */
#define SHA_TEXT_BASE               (SHA_BASE + 0x000UL)

/**
 * SHA_256_START (offset 0x180) — Trigger start of FIRST SHA-256 block.
 * Write 0x1 to this register to load the SHA-256 initial values (IV)
 * and begin processing the block in SHA_TEXT_BASE.
 */
#define SHA_256_START               (SHA_BASE + 0x180UL)

/**
 * SHA_256_CONTINUE (offset 0x184) — Trigger processing of SUBSEQUENT blocks.
 * Write 0x1 to chain the current hash state with the next message block.
 * The engine uses the previous round's output as the next round's input.
 */
#define SHA_256_CONTINUE            (SHA_BASE + 0x184UL)

/**
 * SHA_256_LOAD (offset 0x188) — Request result copy to result registers.
 * Write 0x1 after all blocks are processed. Engine copies the 256-bit
 * hash output to SHA_256_RESULT_BASE.
 */
#define SHA_256_LOAD                (SHA_BASE + 0x188UL)

/**
 * SHA_256_BUSY (offset 0x18C) — Hardware engine busy indicator.
 * Read 0x1 = engine is computing; Read 0x0 = engine is idle.
 * ALWAYS poll this to 0 before issuing next START/CONTINUE/LOAD command
 * or reading result registers. Writing new data while busy causes corruption.
 */
#define SHA_256_BUSY                (SHA_BASE + 0x18CUL)

/**
 * SHA_256_RESULT_BASE (offset 0x200) — 32-byte (256-bit) hash result.
 * 8 consecutive 32-bit registers. Valid only after LOAD completes.
 * Words are in BIG-ENDIAN byte order (most significant bytes first).
 */
#define SHA_256_RESULT_BASE         (SHA_BASE + 0x200UL)

/** SHA-256 block size in bytes (512 bits = one Merkle-Damgård round) */
#define SHA256_BLOCK_BYTES          64U
/** SHA-256 digest output size in bytes */
#define SHA256_DIGEST_BYTES         32U

/* =========================================================================
 * RTC_CNTL — Real-Time Clock Controller / Power Management Unit
 * TRM §29, Base: 0x3FF48000
 *
 * The RTC subsystem runs from a separate 32 kHz slow clock and maintains
 * state across software resets and deep-sleep wakeup cycles.
 *
 * STORE registers (STORE0..STORE7) are 32-bit general-purpose scratch
 * registers within the RTC domain. They retain their values across:
 *   • Software reset (esp_restart / software_reset opcode)
 *   • CPU stall
 *   • Deep sleep (if powered)
 *
 * They are CLEARED by:
 *   • Power-on reset (POR)
 *   • Brownout reset (when supply drops below threshold)
 *   • RTC watchdog reset
 *
 * STORE6 is used by our bootloader as a software IAP trigger flag.
 * The application writes 0xDEADBEEF here and then performs a software
 * reset. The bootloader reads this register and enters IAP if it matches.
 * ========================================================================= */
#define RTC_CNTL_BASE               0x3FF48000UL

#define RTC_CNTL_STORE0_REG         (RTC_CNTL_BASE + 0x030UL)  /**< RTC scratch 0 */
#define RTC_CNTL_STORE1_REG         (RTC_CNTL_BASE + 0x034UL)  /**< RTC scratch 1 */
#define RTC_CNTL_STORE2_REG         (RTC_CNTL_BASE + 0x038UL)  /**< RTC scratch 2 */
#define RTC_CNTL_STORE3_REG         (RTC_CNTL_BASE + 0x03CUL)  /**< RTC scratch 3 */
#define RTC_CNTL_STORE4_REG         (RTC_CNTL_BASE + 0x098UL)  /**< RTC scratch 4 */
#define RTC_CNTL_STORE5_REG         (RTC_CNTL_BASE + 0x09CUL)  /**< RTC scratch 5 */

/**
 * RTC_CNTL_STORE6_REG (offset 0x0A0) — Our bootloader IAP flag register.
 *
 * Dual-path IAP trigger mechanism:
 *   PATH A (hardware): GPIO 0 sampled LOW at boot → IAP mode
 *   PATH B (software): Application writes 0xDEADBEEF to this register,
 *                      then calls esp_restart(). Bootloader reads it and
 *                      enters IAP. This survives the soft reset.
 *
 * After entering IAP (either path), bootloader clears this register
 * to prevent stuck-in-IAP loops.
 */
#define RTC_CNTL_STORE6_REG         (RTC_CNTL_BASE + 0x0A0UL)
#define RTC_CNTL_STORE7_REG         (RTC_CNTL_BASE + 0x0A4UL)  /**< RTC scratch 7 */

/** Magic value written to STORE6 by application to request IAP on next reset */
#define IAP_MAGIC_WORD              0xDEADBEEFUL

/** Value written by bootloader to STORE6 after consuming the IAP request */
#define IAP_MAGIC_CLEAR             0x00000000UL

/* =========================================================================
 * WATCHDOG TIMERS
 * TRM §26 (TIMG0 MWDT), TRM §29 (RTC RWDT)
 *
 * Both watchdogs are armed by the ROM bootloader and may fire during
 * our long CRC/SHA operations if not disabled first.
 *
 * Protection mechanism:
 *   The WDT configuration registers are write-protected. Before writing
 *   any WDT configuration register, write the unlock key (0x50D83AA1)
 *   to the WDTPROTECT register. The window closes automatically after
 *   any write to a protected register.
 * ========================================================================= */
#define TIMG0_BASE                  0x3FF5F000UL
#define TIMG0_WDTCONFIG0_REG        (TIMG0_BASE + 0x048UL)  /**< MWDT config */
#define TIMG0_WDTFEED_REG           (TIMG0_BASE + 0x060UL)  /**< Write any val to feed */
#define TIMG0_WDTPROTECT_REG        (TIMG0_BASE + 0x064UL)  /**< Write key to unlock */

#define RTC_CNTL_WDTCONFIG0_REG    (RTC_CNTL_BASE + 0x08CUL)  /**< RTC WDT config */
#define RTC_CNTL_WDTFEED_REG       (RTC_CNTL_BASE + 0x09CUL)  /**< RTC WDT feed */
#define RTC_CNTL_WDTPROTECT_REG    (RTC_CNTL_BASE + 0x0A0UL)  /**< RTC WDT protect */

#define WDT_PROTECT_KEY             0x50D83AA1UL  /**< Magic key to unlock WDT regs */
#define WDT_ENABLE_BIT              (1UL << 31)   /**< WDT enable/disable bit */

/* =========================================================================
 * TIMER GROUP 0 — MWDT Timer (for optional delay functions)
 * ========================================================================= */
#define TIMG0_T0CONFIG_REG          (TIMG0_BASE + 0x000UL)
#define TIMG0_T0LO_REG              (TIMG0_BASE + 0x004UL)
#define TIMG0_T0HI_REG              (TIMG0_BASE + 0x008UL)
#define TIMG0_T0UPDATE_REG          (TIMG0_BASE + 0x00CUL)

/* =========================================================================
 * Xtensa LX6 Special Register Numbers
 * Accessed via RSR (Read Special Register) and WSR (Write Special Register)
 * instructions. Cannot be accessed as MMIO — require inline assembly.
 * ========================================================================= */

/**
 * VECBASE Special Register (SR number 231 = 0xE7)
 *
 * Holds the 256-byte-aligned base address of the exception vector table.
 * Written with: `WSR  Rn, vecbase`
 * Read with:    `RSR  Rn, vecbase`
 *
 * Xtensa vectors are EXECUTABLE CODE at fixed offsets from VECBASE.
 * This differs fundamentally from ARM's VTOR which holds pointer addresses.
 * Each exception level n dispatches to: VECBASE + fixed_level_offset
 *
 * After writing VECBASE, ISYNC must be executed to flush the instruction
 * prefetch pipeline. Failure to ISYNC causes the CPU to fetch instructions
 * using the old VECBASE value for several more cycles.
 */
#define XTENSA_SR_VECBASE            231   /* 0xE7 — used symbolically in asm */

/**
 * PS (Processor State) Special Register (SR number 230 = 0xE6)
 *
 * Bit layout (relevant fields):
 *   [3:0]   INTLEVEL — Current interrupt masking level (0=all enabled, 15=all masked)
 *   [4]     EXCM     — Exception mode (1 during exception handling)
 *   [5]     UM       — User mode (0=kernel, 1=user)
 *   [7:6]   RING     — MMU protection ring (always 0b00 on bare ESP32)
 *   [18]    WOE      — Window Overflow Enable (must be 1 for CALL/RETW to work)
 *   [20:16] CALLINC  — Register window call increment (set by ENTRY/RETW)
 *   [24:22] OWB      — Old Window Base (saved on ENTRY)
 *
 * Setting INTLEVEL to 15 via RSIL masks ALL hardware interrupts.
 * RSIL Rn, 15  atomically reads old PS into Rn and sets INTLEVEL=15.
 */
#define XTENSA_SR_PS                 230   /* 0xE6 — used symbolically in asm */

/* =========================================================================
 * APB Clock Frequency
 * The ESP32 APB (Advanced Peripheral Bus) runs at 80 MHz after the ROM
 * bootloader configures the PLL. This is used for baud rate calculations.
 * ========================================================================= */
#define APB_CLK_FREQ_HZ             80000000UL   /**< APB clock = 80 MHz */

#endif /* ESP32_REGS_H */
