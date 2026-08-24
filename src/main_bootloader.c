/**
 * @file    main_bootloader.c
 * @brief   ESP32-D0WD / ESP32-CAM bare-metal secondary bootloader.
 *
 * ==========================================================================
 * SYSTEM ARCHITECTURE OVERVIEW
 * ==========================================================================
 *
 * This file implements the complete secondary bootloader logic:
 *
 *   ┌─────────────┐  power-on  ┌───────────────────┐
 *   │  ROM BL     │  ────────► │  startup.S         │
 *   │  (0x40000000│            │  • RSIL / VECBASE  │
 *   │  mask ROM)  │            │  • Stack / BSS     │
 *   └─────────────┘            │  • .data copy      │
 *                              └────────┬──────────┘
 *                                       │ call0 bootloader_main
 *                                       ▼
 *                              ┌───────────────────────────────────────┐
 *                              │  bootloader_main()                    │
 *                              │                                       │
 *                              │  1. bl_wdt_disable()                  │
 *                              │  2. bl_uart_init()                    │
 *                              │  3. bl_crc32_init_table()             │
 *                              │  4. bl_gpio_configure_iap_pin()       │
 *                              │  5. bl_check_iap_trigger() ──┐        │
 *                              │                               │ YES    │
 *                              │                               └──► bl_enter_iap_mode() │
 *                              │  6. bl_read_ota_state()               │
 *                              │  7. bl_validate_image_header()        │
 *                              │  8. bl_verify_crc32()                 │
 *                              │  9. bl_verify_sha256_hw()             │
 *                              │  10. bl_load_app_segments()           │
 *                              │  11. bl_jump_to_app() ─────────────── ┤
 *                              │                           JX → App    │
 *                              └───────────────────────────────────────┘
 *
 * ==========================================================================
 * IRAM_ATTR / DRAM_ATTR PLACEMENT RATIONALE
 * ==========================================================================
 *
 * ESP32 SPI flash access uses a shared bus between two masters:
 *   SPI0 (cache master): drives XIP/execute-in-place for the CPU
 *   SPI1 (cpu master):   we control this for direct flash reads
 *
 * When we perform raw SPI1 transactions, we MUST disable SPI0's cache
 * first. While the cache is disabled, the CPU cannot fetch instructions
 * from flash. ANY instruction executed during this window must be in IRAM.
 *
 * Therefore:
 *   • All functions that call spi1_read_raw() MUST be IRAM_ATTR
 *   • The CRC table MUST be DRAM_ATTR (accessible when cache is off)
 *   • All read buffers MUST be DRAM_ATTR (cache-off reachable)
 *   • bl_jump_to_app MUST be IRAM_ATTR (we disable cache inside it)
 *
 * GCC attribute section placement:
 *   IRAM_ATTR → __attribute__((section(".iram.text")))
 *   DRAM_ATTR → __attribute__((section(".dram.data")))
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp32_regs.h"
#include "flash_map.h"

/*
 * BSWAP32 — Portable inline byte-swap for 32-bit words.
 *
 * BSWAP32() calls __bswapsi2 from libgcc when -nostdlib is used.
 * Since we link without libgcc (-nostdlib, no -lgcc), we define our own.
 * Pure bit-shift: GCC on Xtensa compiles this to SSAI + SRC (2 cycles).
 *
 * Used to convert between Xtensa little-endian and SHA-256 big-endian words.
 */
#define BSWAP32(x)  (                               \
    (((uint32_t)(x) & 0xFF000000UL) >> 24U) |       \
    (((uint32_t)(x) & 0x00FF0000UL) >>  8U) |       \
    (((uint32_t)(x) & 0x0000FF00UL) <<  8U) |       \
    (((uint32_t)(x) & 0x000000FFUL) << 24U)         \
)


/* =========================================================================
 * SECTION PLACEMENT ATTRIBUTES
 * ========================================================================= */

/**
 * IRAM_ATTR: Forces a function into the .iram.text section.
 * This section is mapped to IRAM (0x40080000) by the linker script.
 * Functions here execute directly from internal SRAM — no flash cache needed.
 * ESSENTIAL for any code that runs while SPI0 cache is disabled.
 */
#define IRAM_ATTR   __attribute__((section(".iram.text"))) \
                    __attribute__((noinline))

/**
 * DRAM_ATTR: Forces a variable into the .dram.data section.
 * This section resides in DRAM (0x3FFAE000+) — accessible via the data bus
 * even when the instruction-bus flash cache is disabled.
 * ESSENTIAL for read buffers, the CRC table, and any data accessed during
 * SPI1 raw transactions.
 */
#define DRAM_ATTR   __attribute__((section(".dram.data")))

/**
 * MEMORY_BARRIER: Full memory barrier via Xtensa MEMW instruction.
 *
 * MEMW (Memory Barrier Write) stalls the processor until all outstanding
 * memory transactions on the AHB/APB buses are complete.
 * Use this before disabling cache (to drain any pending cache writes)
 * and after re-enabling cache (to ensure new mappings are visible).
 *
 * The "memory" clobber prevents the compiler from reordering memory
 * accesses across this barrier in the compiled output.
 */
#define MEMORY_BARRIER()  __asm__ volatile("memw" ::: "memory")

/* =========================================================================
 * UART DEBUG HELPER — BLOCKING POLLED OUTPUT
 *
 * No interrupts. No DMA. No FIFOs beyond the hardware TX FIFO.
 * Write one character at a time, wait for the TX FIFO to have room.
 * This is intentionally simple — bootloader doesn't need throughput.
 * ========================================================================= */

/**
 * @brief Initialize UART0 for 115200 baud, 8N1, no flow control.
 *
 * Called once during bootloader_main() before any debug output.
 * The ROM bootloader already uses UART0 at 115200, so this is mostly
 * a formality — but we re-initialize to guarantee known state.
 *
 * Baud divisor calculation:
 *   APB_CLK = 80,000,000 Hz
 *   Target  = 115,200 baud
 *   Divisor = APB_CLK / baud = 80,000,000 / 115,200 = 694.44...
 *   Integer part: 694 → stored in CLKDIV[19:4] = (694 << 4)
 *   Fractional:   0   → stored in CLKDIV[3:0]  = 0
 *   Register:     0x2B60
 */
IRAM_ATTR void bl_uart_init(void)
{
    REG_WRITE(UART0_CLKDIV_REG, UART0_BAUD_115200_DIV);

    /*
     * UART_CONF0 reset value = 0x0000001C which is:
     *   BIT_NUM=3 (8 data bits), STOP_BIT_NUM=1 (1 stop bit), PARITY=0 (none)
     * This is already 8N1. No write needed.
     * We still write explicitly to be self-documenting and defensive.
     *
     * Bit layout written:
     *   [5:4] = 0b01 = STOP_BIT_NUM = 1 stop bit
     *   [3:2] = 0b11 = BIT_NUM = 8 data bits
     *   [0]   = 0    = PARITY_EN = no parity
     */
    REG_WRITE(UART0_CONF0_REG, (0x1U << 4) | (0x3U << 2));
}

/**
 * @brief Write a single byte to UART0 TX FIFO (blocking).
 *
 * Poll TXFIFO_CNT until space is available, then write to the FIFO register.
 * At 115200 baud, each byte takes ~87 µs to transmit. This blocking is
 * acceptable in a bootloader context.
 *
 * @param c  Byte to transmit
 */
IRAM_ATTR void bl_uart_putc(char c)
{
    /*
     * Spin until TX FIFO has fewer than 127 bytes (FIFO depth is 128).
     * TXFIFO_CNT is at STATUS[22:16] — 7-bit count field.
     *
     * If \n, also send \r for CRLF line endings (most terminal emulators expect this).
     */
    if (c == '\n') {
        while (((REG_READ(UART0_STATUS_REG) >> UART0_TXFIFO_CNT_SHIFT)
                & UART0_TXFIFO_CNT_MASK) >= UART0_TXFIFO_FULL_LEVEL) { /* spin */ }
        REG_WRITE(UART0_FIFO_REG, '\r');
    }
    while (((REG_READ(UART0_STATUS_REG) >> UART0_TXFIFO_CNT_SHIFT)
            & UART0_TXFIFO_CNT_MASK) >= UART0_TXFIFO_FULL_LEVEL) { /* spin */ }
    REG_WRITE(UART0_FIFO_REG, (uint32_t)(uint8_t)c);
}

/**
 * @brief Write a null-terminated C string to UART0.
 * @param s  Pointer to string (must be in DRAM or IRAM — not raw XIP flash)
 */
IRAM_ATTR void bl_uart_puts(const char *s)
{
    while (*s) {
        bl_uart_putc(*s++);
    }
}

/**
 * @brief Print a 32-bit hex value with a label prefix.
 * Format: "  label: 0xXXXXXXXX\n"
 * @param label  String label (must be in DRAM/IRAM)
 * @param val    32-bit value to print
 */
IRAM_ATTR void bl_uart_print_hex32(const char *label, uint32_t val)
{
    static char DRAM_ATTR hex_chars[] = "0123456789ABCDEF";
    bl_uart_puts("  ");
    bl_uart_puts(label);
    bl_uart_puts(": 0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        bl_uart_putc(hex_chars[(val >> shift) & 0xFU]);
    }
    bl_uart_putc('\n');
}

/* =========================================================================
 * WATCHDOG TIMER DISABLE
 * ========================================================================= */

/**
 * @brief Disable both the Main Watchdog Timer (MWDT0) and the RTC Watchdog.
 *
 * WDT registers are WRITE-PROTECTED. The protection mechanism:
 *   1. Write the unlock key (0x50D83AA1) to the WDTPROTECT register.
 *   2. The protection window opens for ONE write to protected registers.
 *   3. The window automatically closes again after the write.
 *
 * MWDT0 (Timer Group 0 Main WDT):
 *   • WDT_ENABLE bit is bit [31] of TIMG0_WDTCONFIG0
 *   • Clear bit 31 → WDT disabled
 *
 * RWDT (RTC Watchdog):
 *   • Same protocol with RTC_CNTL_WDTPROTECT and RTC_CNTL_WDTCONFIG0
 *
 * Why disable: Our CRC32 + SHA-256 loop over a 1 MB image takes several
 * seconds. The WDT default timeout (typically 8–30 seconds) may fire.
 * The application will re-enable its own WDT after startup.
 */
IRAM_ATTR void bl_wdt_disable(void)
{
    /* ── Disable MWDT0 (Timer Group 0 Main Watchdog) ────────────────── */
    REG_WRITE(TIMG0_WDTPROTECT_REG, WDT_PROTECT_KEY); /* Unlock */
    REG_CLR_BIT(TIMG0_WDTCONFIG0_REG, WDT_ENABLE_BIT); /* Disable WDT */
    REG_WRITE(TIMG0_WDTPROTECT_REG, 0UL);              /* Re-lock */

    /* ── Disable RWDT (RTC Watchdog) ────────────────────────────────── */
    REG_WRITE(RTC_CNTL_WDTPROTECT_REG, WDT_PROTECT_KEY); /* Unlock */
    REG_CLR_BIT(RTC_CNTL_WDTCONFIG0_REG, WDT_ENABLE_BIT); /* Disable */
    REG_WRITE(RTC_CNTL_WDTPROTECT_REG, 0UL);              /* Re-lock */
}

/* =========================================================================
 * BUSY-WAIT DELAY
 * ========================================================================= */

/**
 * @brief Approximate millisecond delay using CPU busy-wait.
 *
 * NOT suitable for precision timing. Used only for GPIO debounce (10 ms).
 *
 * Calibration:
 *   At 240 MHz CPU clock, each NOP takes ~1/(240e6) ≈ 4.2 ns.
 *   ~240,000 NOPs ≈ 1 ms. Xtensa NOP executes in 1 pipeline slot (1 cycle).
 *   CPU clock at boot may be 80 MHz (ROM BL sets 80 MHz initially).
 *   At 80 MHz: 80,000 iterations ≈ 1 ms.
 *   Conservative: 100,000 iterations per ms (safe for 80–240 MHz range).
 *
 * @param ms   Milliseconds to wait (approximate)
 */
IRAM_ATTR static void bl_delay_ms(uint32_t ms)
{
    /* Prevent compiler from optimizing the loop away (-O2 might remove it) */
    for (volatile uint32_t i = 0; i < ms * 100000UL; i++) {
        __asm__ volatile("nop");  /* Single-cycle no-operation */
    }
}

/* =========================================================================
 * GPIO CONFIGURATION
 * ========================================================================= */

/**
 * @brief Configure a GPIO pin as input with internal pull-up enabled.
 *
 * GPIO pad configuration requires writing two register sets:
 *
 * 1. IO_MUX register for the physical pad:
 *    Sets the electrical characteristics and selects the signal function.
 *    For GPIO matrix routing (function 2), this is required first.
 *    Bits we write:
 *      [14:12] MCU_SEL = 2   → Route through GPIO matrix (gives SW control)
 *      [9]     FUN_IE  = 1   → Enable input buffer (read pin level)
 *      [8]     FUN_WPU = 1   → Enable internal pull-up (~47 kΩ to VDD33)
 *
 * 2. GPIO_ENABLE_W1TC register:
 *    Clear the OUTPUT ENABLE bit so the pad is configured as INPUT.
 *    W1TC = Write-1-To-Clear: writing bit N clears output enable for GPIO N.
 *    This is atomic and requires no read-modify-write.
 *
 * @param gpio_num   GPIO number (0–39)
 */
IRAM_ATTR void bl_gpio_set_input_pullup(uint8_t gpio_num)
{
    /*
     * Write IO_MUX register for this pad.
     * We write the complete register (not read-modify-write) to ensure
     * we're in a known state regardless of what the ROM BL left.
     */
    REG_WRITE(IO_MUX_GPIO(gpio_num),
              IO_MUX_GPIO_FUNC  |   /* bits [14:12] = 010 → GPIO matrix */
              IO_MUX_FUN_IE     |   /* bit  [9]     = 1   → input enable */
              IO_MUX_FUN_WPU);      /* bit  [8]     = 1   → pull-up enable */

    /* Disable output driver → pad becomes a pure input */
    if (gpio_num < 32U) {
        /* Bank 0: GPIO 0–31 */
        REG_WRITE(GPIO_ENABLE_W1TC_REG, 1UL << gpio_num);
    }
    /* GPIO 32–39 use separate W1TC register — not shown (GPIO 0 used for IAP) */
}

/**
 * @brief Read the digital level of a GPIO pin.
 *
 * @param gpio_num   GPIO number (0–39)
 * @return           0 if the pad is LOW, 1 if the pad is HIGH
 */
IRAM_ATTR uint8_t bl_gpio_read(uint8_t gpio_num)
{
    if (gpio_num < 32U) {
        /* GPIO_IN_REG: each bit corresponds to the sampled level of GPIO N */
        return (uint8_t)((REG_READ(GPIO_IN_REG) >> gpio_num) & 0x1U);
    } else {
        /* GPIO_IN1_REG covers GPIO 32–39 */
        return (uint8_t)((REG_READ(GPIO_IN1_REG) >> (gpio_num - 32U)) & 0x1U);
    }
}

/* =========================================================================
 * RAW SPI1 FLASH READ
 *
 * This is the core flash read engine. It bypasses the cache entirely
 * and communicates with the flash chip directly via SPI1 registers.
 *
 * CRITICAL RULES (violations cause instant crash):
 * ─────────────────────────────────────────────────────────────────────────
 *  1. flash_cache_disable() MUST be called BEFORE spi1_read_raw()
 *     After cache is disabled, the CPU executes from IRAM only.
 *
 *  2. All functions in the call chain from cache-disable to cache-enable
 *     MUST be IRAM_ATTR. Even one non-IRAM function called during this
 *     window = instant prefetch fault = immediate CPU lock-up.
 *
 *  3. All data buffers written/read during this window MUST be in DRAM
 *     (accessible via data bus). DRAM_ATTR variables satisfy this.
 *
 *  4. flash_cache_enable() MUST be called after all SPI1 transactions.
 *     Failure to re-enable leaves the system unable to execute from flash.
 *
 * Transaction phases for standard READ (opcode 0x03):
 *   [CMD: 8 bits, 0x03] → [ADDR: 24 bits] → [MISO: N bytes]
 *   No dummy cycles needed for 0x03 READ (device reads at DC clock speed).
 *   For FAST_READ (0x0B): CMD(8) + ADDR(24) + DUMMY(8) + MISO(N)
 * ========================================================================= */

/**
 * @brief Disable SPI0 flash cache (stop XIP execution from flash).
 *
 * Mechanism:
 *   DPORT_PRO_CACHE_CTRL bit[0] = PRO_CACHE_ENABLE
 *   Writing 0 to this bit stops the SPI0 cache controller from issuing
 *   transactions on the shared QSPI bus. SPI1 now has exclusive bus access.
 *
 *   The MEMORY_BARRIER() before the register write ensures all CPU-side
 *   pending loads from flash (that may be in the AHB pipeline) complete
 *   before the cache is cut off.
 *
 * POST-CONDITION: Only IRAM code may execute after this call.
 */
IRAM_ATTR static void flash_cache_disable(void)
{
    MEMORY_BARRIER();  /* Drain all pending AHB/APB memory transactions */
    /* Clear bit 0 (PRO_CACHE_ENABLE): stops SPI0 from driving the flash bus */
    REG_CLR_BIT(DPORT_PRO_CACHE_CTRL, DPORT_PRO_CACHE_ENABLE_BIT);
    MEMORY_BARRIER();  /* Ensure cache-disable write is committed to DPORT */
}

/**
 * @brief Re-enable SPI0 flash cache (restore XIP execution from flash).
 *
 * Sets bit 0 (PRO_CACHE_ENABLE) to restart the cache engine.
 * After this call, the CPU can execute from flash-mapped virtual addresses again.
 *
 * PRE-CONDITION: All SPI1 transactions must be fully complete before calling.
 */
IRAM_ATTR static void flash_cache_enable(void)
{
    MEMORY_BARRIER();
    REG_SET_BIT(DPORT_PRO_CACHE_CTRL, DPORT_PRO_CACHE_ENABLE_BIT);
    MEMORY_BARRIER();  /* Ensure cache-enable write propagates before next fetch */
}

/**
 * @brief Wait for SPI1 state machine to return to IDLE.
 *
 * After writing SPI1_CMD_USR to trigger a transaction, poll the CMD register
 * until the USR bit clears (hardware auto-clears it on completion).
 * Alternatively, poll SPI1_EXT2 ST field for 0.
 *
 * We poll both for robustness.
 */
IRAM_ATTR static void spi1_wait_idle(void)
{
    /* SPI1_CMD[18] = USR: hardware sets this on transaction start, clears on done */
    while (REG_READ(SPI1_CMD_REG) & SPI1_CMD_USR) {
        /* Spin — typical flash READ transaction at 40 MHz: ~2 µs per 64 bytes */
    }
    /* Also verify state machine is in IDLE state (ST field = 0) */
    while ((REG_READ(SPI1_EXT2_REG) & SPI1_EXT2_ST_MASK) != 0U) {
        /* Spin */
    }
}

/**
 * @brief Read exactly `len` bytes (1–64) from physical flash address `addr`.
 *
 * This function directly programs SPI1 registers to issue a standard
 * READ (0x03) transaction and collects the received data from W0..W15.
 *
 * Transaction configuration:
 *   CMD    phase: 8 bits,  value = 0x03 (READ opcode)
 *   ADDR   phase: 24 bits, value = flash physical byte address
 *   MISO   phase: len*8 bits, received into SPI1_W0..W15 registers
 *   MOSI   phase: disabled (read-only transaction)
 *   DUMMY  phase: disabled (not needed for standard 0x03 READ)
 *
 * @param addr   24-bit physical flash byte address (e.g., 0x010000)
 * @param buf    Destination buffer in DRAM (DRAM_ATTR or stack)
 * @param len    Number of bytes to read (1 to SPI1_MAX_TRANSFER_BYTES=64)
 *
 * @pre  flash_cache_disable() must be called before this function.
 * @pre  len must be 1..64 (hardware FIFO limit).
 * @pre  buf must point to DRAM (accessible with cache disabled).
 */
IRAM_ATTR static void spi1_read_raw(uint32_t addr, uint8_t *buf, size_t len)
{
    /* Validate len: clamp to 64 bytes (SPI1 FIFO hardware limit) */
    if (len == 0U || len > SPI1_MAX_TRANSFER_BYTES) return;

    /* Ensure SPI1 is idle before reconfiguring */
    spi1_wait_idle();

    /* ── Configure USER register: enable CMD + ADDR + MISO phases ─────
     *
     * SPI1_USR_COMMAND (bit 31): Include 8-bit command phase.
     *   The command opcode (0x03) is transmitted MSB-first on MOSI.
     *
     * SPI1_USR_ADDR (bit 30): Include 24-bit address phase.
     *   The 24-bit address is transmitted MSB-first on MOSI.
     *   Address value is written to SPI1_ADDR_REG with 8-bit left shift.
     *
     * SPI1_USR_MISO (bit 28): Include MISO data receive phase.
     *   `len` bytes are clocked in from the flash chip on the MISO line.
     *   Each byte lands in the SPI1_Wn registers, LSB-first within each word.
     *
     * We explicitly clear MOSI (bit 27) and DUMMY (bit 29) to exclude
     * those phases. Writing the complete USER register (not RMW) ensures
     * we don't accidentally inherit stale phase configurations from the
     * ROM bootloader's last SPI transaction.
     */
    REG_WRITE(SPI1_USER_REG,
              SPI1_USR_COMMAND |    /* bit[31]: send 8-bit CMD opcode */
              SPI1_USR_ADDR    |    /* bit[30]: send 24-bit address */
              SPI1_USR_MISO);       /* bit[28]: receive MISO data */
    /* Bits [29,27] = DUMMY, MOSI → 0 (excluded from this transaction) */

    /* ── Configure USER1 register: set address phase bit length ────────
     *
     * SPI_USR_ADDR_BITLEN field in USER1[31:26]:
     *   Stores (address_bits - 1).
     *   For a 24-bit address: value = 24 - 1 = 23 → (23 << 26)
     *
     * SPI_USR_DUMMY_CYCLELEN field in USER1[25:0]:
     *   Stores (dummy_cycles - 1). We're not using dummy cycles here.
     *   Clear the field by writing 0 for those bits.
     *
     * Full register write: 23 << 26 = 0x5C000000
     */
    REG_WRITE(SPI1_USER1_REG, (23UL << SPI1_USER1_ADDR_BITLEN_SHIFT));

    /* ── Configure USER2 register: command opcode and bit length ────────
     *
     * SPI_USR_COMMAND_BITLEN field in USER2[31:28]:
     *   Stores (command_bits - 1). For 8-bit opcode: 8 - 1 = 7 → (7 << 28)
     *
     * SPI_USR_COMMAND_VALUE field in USER2[15:0]:
     *   The actual opcode byte. For standard READ: 0x03.
     *   The hardware transmits this value MSB-first on MOSI.
     *
     * Full register: (7 << 28) | 0x03 = 0x70000003
     */
    REG_WRITE(SPI1_USER2_REG,
              (7UL << SPI1_USER2_CMD_BITLEN_SHIFT) | (uint32_t)FLASH_OP_READ);

    /* ── Set address register ───────────────────────────────────────────
     *
     * SPI1_ADDR_REG[31:0] holds the address to transmit.
     * The hardware extracts the top (ADDR_BITLEN+1) = 24 bits.
     * To align the 24-bit address to bits [31:8], left-shift by 8.
     *
     * Example: addr = 0x010000
     *   addr << 8 = 0x01000000 → stored in register
     *   Hardware sends bits [31:8] = 0x010000 (24 bits, MSB first on MOSI)
     */
    REG_WRITE(SPI1_ADDR_REG, addr << 8);

    /* ── Set MISO data length ───────────────────────────────────────────
     *
     * SPI1_MISO_DLEN_REG stores (number_of_bits_to_receive - 1).
     * For `len` bytes: value = len*8 - 1
     */
    REG_WRITE(SPI1_MISO_DLEN_REG, (uint32_t)((len * 8U) - 1U));

    /* ── Set MOSI data length = 0 (no data to send in this transaction) */
    REG_WRITE(SPI1_MOSI_DLEN_REG, 0U);

    /* ── Trigger the transaction: write SPI_USR bit ─────────────────────
     *
     * Writing SPI1_CMD_USR (bit 18) to SPI1_CMD_REG starts the transaction.
     * The hardware FSM drives CS LOW, sends CMD, sends ADDR, clocks in MISO.
     * Hardware auto-clears bit 18 when the transaction finishes.
     *
     * We use REG_SET_BIT (read-modify-write) intentionally here to avoid
     * accidentally triggering other built-in commands in bits [17:0].
     */
    REG_SET_BIT(SPI1_CMD_REG, SPI1_CMD_USR);

    /* Wait for transaction to complete */
    spi1_wait_idle();

    /* ── Collect received data from SPI data buffer registers ──────────
     *
     * After the transaction, received bytes are in SPI1_W0..W15.
     * Each SPI1_Wn register holds 4 bytes in LITTLE-ENDIAN order:
     *   Byte 0 (first received) → bits [7:0]   of W0
     *   Byte 1                  → bits [15:8]  of W0
     *   Byte 2                  → bits [23:16] of W0
     *   Byte 3                  → bits [31:24] of W0
     *   Byte 4 (next)           → bits [7:0]   of W1
     *   ... etc.
     *
     * We extract each byte by reading the appropriate Wn register and
     * shifting by the appropriate amount.
     *
     * Optimization: read full 32-bit words first, then extract bytes.
     * This minimizes the number of MMIO register reads (each takes ~3 cycles).
     */
    size_t full_words = len / 4U;
    size_t remainder  = len % 4U;

    for (size_t w = 0; w < full_words; w++) {
        uint32_t word = REG_READ(SPI1_Wn_REG(w));
        buf[w*4 + 0] = (uint8_t)((word >>  0) & 0xFFU);
        buf[w*4 + 1] = (uint8_t)((word >>  8) & 0xFFU);
        buf[w*4 + 2] = (uint8_t)((word >> 16) & 0xFFU);
        buf[w*4 + 3] = (uint8_t)((word >> 24) & 0xFFU);
    }

    if (remainder > 0U) {
        uint32_t word = REG_READ(SPI1_Wn_REG(full_words));
        for (size_t b = 0; b < remainder; b++) {
            buf[full_words*4 + b] = (uint8_t)((word >> (b * 8U)) & 0xFFU);
        }
    }
}

/**
 * @brief Read `len` bytes from flash physical address `addr` (chunked).
 *
 * Wraps spi1_read_raw() to handle transfers larger than 64 bytes by
 * automatically chunking the request into 64-byte SPI transactions.
 *
 * This function manages the flash_cache_disable()/enable() window.
 * Callers do NOT need to manage cache state themselves.
 *
 * @param addr   Physical flash byte address
 * @param buf    Destination buffer (MUST be in DRAM)
 * @param len    Total number of bytes to read
 */
IRAM_ATTR void bl_flash_read(uint32_t addr, uint8_t *buf, size_t len)
{
    flash_cache_disable();  /* Stop SPI0 cache — IRAM-only execution zone begins */

    size_t remaining = len;
    uint32_t cur_addr = addr;
    uint8_t *cur_buf = buf;

    while (remaining > 0U) {
        /* Chunk to 64 bytes (SPI1 hardware FIFO limit per transaction) */
        size_t chunk = (remaining > SPI1_MAX_TRANSFER_BYTES)
                        ? SPI1_MAX_TRANSFER_BYTES
                        : remaining;

        spi1_read_raw(cur_addr, cur_buf, chunk);

        cur_addr  += (uint32_t)chunk;
        cur_buf   += chunk;
        remaining -= chunk;
    }

    flash_cache_enable();  /* Restore SPI0 cache — flash XIP resumes */
}

/* =========================================================================
 * CRC32 — TABLE-DRIVEN IMPLEMENTATION
 *
 * Algorithm: CRC-32/ISO-HDLC (also known as "CRC32b" or "Ethernet CRC")
 * ─────────────────────────────────────────────────────────────────────────
 * Parameters:
 *   Width:       32 bits
 *   Polynomial:  0x04C11DB7 (normal representation)
 *                0xEDB88320 (bit-reversed / reflected representation)
 *   Init value:  0xFFFFFFFF
 *   Input reflection:  YES (LSB of each byte is processed first)
 *   Output reflection: YES
 *   Final XOR:   0xFFFFFFFF
 *
 * Used in: Ethernet, ZIP, PNG, Gzip, bzip2, and this bootloader.
 *
 * Table-driven approach:
 *   Pre-compute a 256-entry table where table[i] = CRC32(byte value i).
 *   For each input byte B:
 *     idx = (current_crc XOR B) & 0xFF
 *     current_crc = table[idx] XOR (current_crc >> 8)
 *
 * This is the "slice-by-1" approach (1 byte per iteration).
 * It requires 1 table lookup + 1 XOR + 1 shift = ~3 instructions/byte.
 * At 80 MHz APB, processing a 1 MB image takes approximately 0.5 seconds.
 *
 * The table is DRAM_ATTR because:
 *   During bl_flash_crc32(), we disable the flash cache to do SPI1 reads.
 *   The CRC table must be in DRAM (not XIP flash) to remain accessible.
 * ========================================================================= */

/** CRC32 polynomial in reflected (LSB-first) form */
#define CRC32_POLY_REFLECTED        0xEDB88320UL

/** CRC32 initial value */
#define CRC32_INIT                  0xFFFFFFFFUL

/** CRC32 final XOR value */
#define CRC32_XOR_OUT               0xFFFFFFFFUL

/** 256-entry CRC32 lookup table in DRAM */
DRAM_ATTR static uint32_t s_crc32_table[256];

/** Flag: table has been initialized (prevents double init) */
DRAM_ATTR static bool s_crc32_table_ready = false;

/**
 * @brief Pre-compute the CRC32 lookup table using polynomial 0xEDB88320.
 *
 * This must be called once during bootloader startup, before any CRC
 * computations are performed.
 *
 * Table generation algorithm:
 *   For each possible byte value i (0..255):
 *     crc = i
 *     For each of 8 bits:
 *       if (LSB of crc is 1):
 *         crc = (crc >> 1) XOR polynomial
 *       else:
 *         crc = crc >> 1
 *     table[i] = crc
 *
 * The bit-by-bit loop simulates the serial CRC shift register for one byte.
 * The resulting table[i] gives the final CRC contribution of byte value i.
 */
IRAM_ATTR void bl_crc32_init_table(void)
{
    if (s_crc32_table_ready) return;

    for (uint32_t i = 0U; i < 256U; i++) {
        uint32_t crc = i;

        /* Simulate 8-bit CRC shift register:
         * Each iteration shifts one bit through the polynomial.
         * If the bit shifted out (LSB) is 1, XOR with the reflected polynomial.
         */
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1U) {
                crc = (crc >> 1U) ^ CRC32_POLY_REFLECTED;
            } else {
                crc >>= 1U;
            }
        }
        s_crc32_table[i] = crc;
    }
    s_crc32_table_ready = true;
}

/**
 * @brief Update a running CRC32 with a data buffer.
 *
 * Core CRC update step. Process `len` bytes and return updated CRC.
 *
 * Called with initial CRC = CRC32_INIT (0xFFFFFFFF).
 * After all data is processed, XOR the result with CRC32_XOR_OUT.
 *
 * @param crc    Running CRC value (start with 0xFFFFFFFF)
 * @param data   Data buffer to process (MUST be in DRAM when cache is off)
 * @param len    Number of bytes to process
 * @return       Updated running CRC value
 */
IRAM_ATTR static uint32_t crc32_update(uint32_t crc,
                                        const uint8_t *data,
                                        size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        /*
         * Table lookup step (CRC slice-by-1):
         *   idx = (current_crc XOR current_byte) & 0xFF
         *     → Selects the table entry for the byte we're processing,
         *       XOR'd with the low byte of the current CRC (the "context")
         *   crc = table[idx] XOR (crc >> 8)
         *     → table[idx] accounts for the polynomial division by this byte
         *     → (crc >> 8) shifts out the consumed byte and slides the window
         */
        uint32_t idx = (crc ^ (uint32_t)data[i]) & 0xFFU;
        crc = s_crc32_table[idx] ^ (crc >> 8U);
    }
    return crc;
}

/**
 * @brief Compute CRC32 over a region of SPI flash.
 *
 * Reads flash in 64-byte chunks (SPI1 hardware limit) and feeds each chunk
 * into the CRC32 rolling computation. This avoids needing a large DRAM buffer.
 *
 * @param flash_addr   Physical flash byte address to start reading from
 * @param len          Number of bytes to checksum
 * @param out_crc      Output: computed CRC32 value
 * @return             true on success
 */
IRAM_ATTR bool bl_flash_crc32(uint32_t flash_addr, size_t len, uint32_t *out_crc)
{
    if (!s_crc32_table_ready) {
        bl_uart_puts("[BL-ERR] CRC32 table not initialized!\n");
        return false;
    }

    /* 64-byte chunk buffer in DRAM — accessible while flash cache is disabled */
    DRAM_ATTR static uint8_t crc_chunk[SPI1_MAX_TRANSFER_BYTES];

    uint32_t crc = CRC32_INIT;         /* Initialize CRC accumulator */
    size_t remaining = len;
    uint32_t cur_addr = flash_addr;

    flash_cache_disable();  /* Disable SPI0 cache: IRAM-only execution zone */

    while (remaining > 0U) {
        size_t chunk = (remaining > sizeof(crc_chunk))
                        ? sizeof(crc_chunk)
                        : remaining;

        spi1_read_raw(cur_addr, crc_chunk, chunk);
        crc = crc32_update(crc, crc_chunk, chunk);

        cur_addr  += (uint32_t)chunk;
        remaining -= chunk;
    }

    flash_cache_enable();

    /* Finalize: XOR with 0xFFFFFFFF to produce the final CRC32 value */
    *out_crc = crc ^ CRC32_XOR_OUT;
    return true;
}

/* =========================================================================
 * SHA-256 — HARDWARE ACCELERATOR DRIVER
 *
 * The ESP32 silicon includes a dedicated SHA co-processor connected to the
 * "crypto peripheral" DMA bus. It implements SHA-256 per FIPS 180-4.
 *
 * Key hardware properties:
 *   • Processes exactly 64-byte (512-bit) blocks per invocation
 *   • Internal state is preserved between CONTINUE invocations
 *   • Must be written with BIG-ENDIAN 32-bit words
 *   • Xtensa LX6 is LITTLE-ENDIAN → BSWAP32() required on every word
 *   • SHA_256_BUSY must be polled to 0 before next block or result read
 *
 * Caller responsibility:
 *   • FIPS 180-4 SHA-256 padding MUST be applied before calling
 *   • Padding = 0x80 byte + zeros + 64-bit big-endian bit count
 *   • This driver handles the raw block processing; caller preps blocks
 *
 * Why use hardware SHA instead of software:
 *   • Hardware: ~1 million SHA256 bytes/second at 80 MHz
 *   • Software: ~200 KB/second typical (5× slower)
 *   • For a 1 MB image, hardware saves ~4 seconds of boot latency
 * ========================================================================= */

/**
 * @brief Feed one 64-byte block into the SHA-256 hardware engine.
 *
 * @param block       64-byte message block (MUST be in DRAM)
 * @param first_block true for the very first block (uses SHA_256_START)
 *                    false for all subsequent blocks (uses SHA_256_CONTINUE)
 *
 * This function does NOT handle padding — caller must supply padded blocks.
 */
IRAM_ATTR static void sha256_hw_feed_block(const uint8_t block[SHA256_BLOCK_BYTES],
                                            bool first_block)
{
    /*
     * Write 64 bytes (16 × 32-bit words) to SHA_TEXT_BASE registers.
     *
     * BYTE SWAP REQUIREMENT:
     * SHA-256 internal processing is big-endian (RFC 6234, FIPS 180-4):
     *   The first byte of the message is placed in the MOST SIGNIFICANT
     *   byte of the first 32-bit word. In big-endian, word[0] = b[0]<<24 | b[1]<<16 | b[2]<<8 | b[3].
     *
     * Xtensa LX6 is little-endian: memcpy of 4 bytes to a uint32_t would give
     *   word = b[0] | b[1]<<8 | b[2]<<16 | b[3]<<24  ← WRONG for SHA engine.
     *
     * We must byte-swap each 32-bit word: BSWAP32(x) reverses byte order.
     * GCC compiles this to a single SSAI + SRC instruction pair on Xtensa (2 cycles).
     *
     * Example: message bytes [0x61, 0x62, 0x63, 0x64] ("abcd")
     *   Little-endian as uint32_t: 0x64636261  (wrong for SHA)
     *   After bswap32:             0x61626364  (correct, big-endian for SHA HW)
     */
    const uint32_t *words = (const uint32_t *)block;
    for (uint32_t i = 0U; i < (SHA256_BLOCK_BYTES / 4U); i++) {
        REG_WRITE(SHA_TEXT_BASE + (i * 4U),
                  BSWAP32(words[i]));
    }

    /*
     * Wait for any PREVIOUS block to finish (in case called in rapid succession).
     * SHA_256_BUSY = 1 while engine is active, = 0 when idle.
     * This poll costs ~15 APB cycles per block on average.
     */
    while (REG_READ(SHA_256_BUSY) != 0U) { /* spin */ }

    /*
     * Trigger the hardware:
     *   First block:  Write 1 to SHA_256_START
     *     → Loads the standard SHA-256 initial hash values (IV):
     *       H0=0x6a09e667, H1=0xbb67ae85, H2=0x3c6ef372, H3=0xa54ff53a,
     *       H4=0x510e527f, H5=0x9b05688c, H6=0x1f83d9ab, H7=0x5be0cd19
     *     → Processes the first message block using these IVs
     *
     *   Subsequent blocks: Write 1 to SHA_256_CONTINUE
     *     → Uses the previous round's output H0..H7 as the new IV
     *     → Processes the next message block (Merkle-Damgård chaining)
     */
    if (first_block) {
        REG_WRITE(SHA_256_START, 1U);
    } else {
        REG_WRITE(SHA_256_CONTINUE, 1U);
    }

    /* Wait for this block to finish */
    while (REG_READ(SHA_256_BUSY) != 0U) { /* spin */ }
}

/**
 * @brief Compute SHA-256 over a flash region using the hardware accelerator.
 *
 * Handles:
 *   • Reading flash in 64-byte chunks via SPI1
 *   • Applying FIPS 180-4 SHA-256 padding for the final block
 *   • Feeding each padded block to the SHA-256 hardware engine
 *   • Reading the 32-byte result from the result registers
 *
 * SHA-256 Padding Rules (FIPS 180-4 §5.1.1):
 *   The padded message length must be a multiple of 512 bits (64 bytes).
 *   Padding = [0x80] [0x00 × k] [length_in_bits as 8-byte big-endian]
 *   where k is chosen so that (original_length + 1 + k + 8) ≡ 0 (mod 64)
 *   If original_length mod 64 ≥ 56: two padding blocks are needed.
 *
 * @param flash_addr   Physical flash byte offset
 * @param len          Number of bytes to hash
 * @param digest_out   Output: 32-byte SHA-256 digest (DRAM buffer)
 * @return             true on success
 */
IRAM_ATTR bool bl_flash_sha256(uint32_t flash_addr,
                                uint32_t len,
                                uint8_t  digest_out[SHA256_DIGEST_BYTES])
{
    /* Working block buffer in DRAM (accessible during cache-disable window) */
    DRAM_ATTR static uint8_t sha_block[SHA256_BLOCK_BYTES];

    uint32_t remaining  = len;
    uint32_t cur_addr   = flash_addr;
    bool     first_block = true;

    flash_cache_disable();

    /* Process all full 64-byte blocks */
    while (remaining >= SHA256_BLOCK_BYTES) {
        spi1_read_raw(cur_addr, sha_block, SHA256_BLOCK_BYTES);
        sha256_hw_feed_block(sha_block, first_block);
        first_block  = false;
        cur_addr    += SHA256_BLOCK_BYTES;
        remaining   -= SHA256_BLOCK_BYTES;
    }

    /* ── Build the FINAL padded block(s) ───────────────────────────────
     *
     * remaining = number of leftover bytes (0..63) that don't fill a full block.
     * We must construct the final padded block per FIPS 180-4:
     *
     *   Case A (remaining < 56): Padding fits in one block.
     *     [remaining bytes] [0x80] [zeros to fill to 56 bytes] [8-byte length]
     *
     *   Case B (remaining >= 56): Padding needs two blocks.
     *     Block N:   [remaining bytes] [0x80] [zeros to fill 64 bytes]
     *     Block N+1: [56 zero bytes]   [8-byte length]
     *
     * Length field: total_bits = len * 8, stored as 64-bit big-endian.
     * On Xtensa (little-endian), we must manually byte-reverse the 8 bytes.
     */

    /* Read any remaining data bytes into the padding block */
    if (remaining > 0U) {
        spi1_read_raw(cur_addr, sha_block, (size_t)remaining);
    }

    /* Append the 0x80 padding marker immediately after last data byte */
    sha_block[remaining] = 0x80U;

    /* Zero-fill from (remaining+1) to the end of the block */
    for (uint32_t i = remaining + 1U; i < SHA256_BLOCK_BYTES; i++) {
        sha_block[i] = 0x00U;
    }

    /* Compute total message length in bits (64-bit value) */
    uint64_t total_bits = (uint64_t)len * 8ULL;

    if (remaining < 56U) {
        /* ── Case A: Length fits in this block ─────────────────────── */
        /* Append 8-byte big-endian bit length at bytes [56..63] */
        sha_block[56] = (uint8_t)((total_bits >> 56U) & 0xFFU);
        sha_block[57] = (uint8_t)((total_bits >> 48U) & 0xFFU);
        sha_block[58] = (uint8_t)((total_bits >> 40U) & 0xFFU);
        sha_block[59] = (uint8_t)((total_bits >> 32U) & 0xFFU);
        sha_block[60] = (uint8_t)((total_bits >> 24U) & 0xFFU);
        sha_block[61] = (uint8_t)((total_bits >> 16U) & 0xFFU);
        sha_block[62] = (uint8_t)((total_bits >>  8U) & 0xFFU);
        sha_block[63] = (uint8_t)((total_bits >>  0U) & 0xFFU);

        sha256_hw_feed_block(sha_block, first_block);
    } else {
        /* ── Case B: Need two padding blocks ────────────────────────── */
        /* First padding block: data + 0x80 + zeros (no length here) */
        /* sha_block already has: data + 0x80 + zeros filled above */
        sha256_hw_feed_block(sha_block, first_block);

        /* Second padding block: all zeros + 8-byte length */
        for (uint32_t i = 0U; i < 56U; i++) {
            sha_block[i] = 0x00U;
        }
        sha_block[56] = (uint8_t)((total_bits >> 56U) & 0xFFU);
        sha_block[57] = (uint8_t)((total_bits >> 48U) & 0xFFU);
        sha_block[58] = (uint8_t)((total_bits >> 40U) & 0xFFU);
        sha_block[59] = (uint8_t)((total_bits >> 32U) & 0xFFU);
        sha_block[60] = (uint8_t)((total_bits >> 24U) & 0xFFU);
        sha_block[61] = (uint8_t)((total_bits >> 16U) & 0xFFU);
        sha_block[62] = (uint8_t)((total_bits >>  8U) & 0xFFU);
        sha_block[63] = (uint8_t)((total_bits >>  0U) & 0xFFU);

        sha256_hw_feed_block(sha_block, false);
    }

    /* ── Load the hash result into the result registers ─────────────── */
    /*
     * Writing 1 to SHA_256_LOAD commands the engine to copy its internal
     * 256-bit hash state to SHA_256_RESULT_BASE (8 × 32-bit registers).
     * Must poll SHA_256_BUSY to 0 before reading the result.
     */
    REG_WRITE(SHA_256_LOAD, 1U);
    while (REG_READ(SHA_256_BUSY) != 0U) { /* Wait for load */ }

    /* ── Read 32-byte digest from result registers ───────────────────── */
    /*
     * SHA_256_RESULT_BASE contains 8 × 32-bit words in big-endian order.
     * We byte-swap each word back to native little-endian before storing
     * in the output buffer.
     *
     * Without bswap: digest word 0 would be in big-endian (MSB first in HW),
     *   but our uint32_t variable would interpret it as little-endian.
     * With bswap: byte order in the output array matches standard SHA-256
     *   (each byte in the array is in the correct position for human comparison).
     */
    uint32_t *digest_words = (uint32_t *)digest_out;
    for (uint32_t i = 0U; i < (SHA256_DIGEST_BYTES / 4U); i++) {
        digest_words[i] = BSWAP32(
                              REG_READ(SHA_256_RESULT_BASE + (i * 4U)));
    }

    flash_cache_enable();
    return true;
}

/* =========================================================================
 * IMAGE VALIDATION
 * ========================================================================= */

/** DRAM-resident buffers for image header reads and hash comparison */
DRAM_ATTR static app_image_hdr_t g_img_hdr;
DRAM_ATTR static uint8_t         g_sha256_computed[SHA256_DIGEST_BYTES];
DRAM_ATTR static uint8_t         g_read_scratch[SPI1_MAX_TRANSFER_BYTES];

/**
 * @brief Read and validate the app image header from a flash slot.
 *
 * Checks:
 *   1. magic field == 0xE9 (APP_IMAGE_MAGIC)
 *   2. chip_id == 0x0005 (APP_CHIP_ID_ESP32)
 *   3. payload_length <= slot size
 *   4. header CRC32 over bytes [0x00..0x1B] matches stored crc32 field
 *
 * @param slot_offset   Physical flash offset of the app slot
 * @param hdr_out       Output: populated header (MUST be in DRAM)
 * @return              true if header is valid
 */
IRAM_ATTR bool bl_validate_image_header(uint32_t slot_offset,
                                         app_image_hdr_t *hdr_out)
{
    /* Read the 80-byte header from flash into DRAM buffer */
    bl_flash_read(slot_offset, (uint8_t *)hdr_out, sizeof(app_image_hdr_t));

    /* Check 1: Magic byte */
    if (hdr_out->magic != APP_IMAGE_MAGIC) {
        bl_uart_puts("[BL-ERR] Bad image magic\n");
        bl_uart_print_hex32("  magic", hdr_out->magic);
        return false;
    }

    /* Check 2: Target chip ID */
    if (hdr_out->chip_id != APP_CHIP_ID_ESP32) {
        bl_uart_puts("[BL-ERR] Bad chip ID\n");
        bl_uart_print_hex32("  chip_id", hdr_out->chip_id);
        return false;
    }

    /* Check 3: Sanity-check payload length (must be < slot size) */
    if (hdr_out->payload_length == 0U ||
        hdr_out->payload_length > FLASH_APP_SLOT1_SIZE) {
        bl_uart_puts("[BL-ERR] Invalid payload_length\n");
        bl_uart_print_hex32("  length", hdr_out->payload_length);
        return false;
    }

    /*
     * Check 4: Header CRC32
     * CRC32 covers bytes [0x00..0x1B] = the first 28 bytes (everything before
     * the crc32 field itself at offset 0x1C).
     *
     * We compute CRC32 over the in-memory copy (which is in DRAM, so
     * no SPI1 transaction needed — just compute directly from the struct).
     */
    uint32_t hdr_bytes_for_crc = offsetof(app_image_hdr_t, crc32); /* = 28 bytes */
    uint32_t computed_crc = CRC32_INIT;
    computed_crc = crc32_update(computed_crc,
                                (const uint8_t *)hdr_out,
                                hdr_bytes_for_crc);
    computed_crc ^= CRC32_XOR_OUT;

    if (computed_crc != hdr_out->crc32) {
        bl_uart_puts("[BL-ERR] Header CRC32 mismatch!\n");
        bl_uart_print_hex32("  expected", hdr_out->crc32);
        bl_uart_print_hex32("  computed", computed_crc);
        return false;
    }

    bl_uart_puts("[BL] Image header valid\n");
    bl_uart_print_hex32("  entry_addr    ", hdr_out->entry_addr);
    bl_uart_print_hex32("  segment_count ", hdr_out->segment_count);
    bl_uart_print_hex32("  payload_length", hdr_out->payload_length);
    return true;
}

/**
 * @brief Verify SHA-256 hash of the app image payload in flash.
 *
 * Computes SHA-256 over `payload_length` bytes starting at
 * (slot_offset + sizeof(app_image_hdr_t)) using the HW accelerator.
 * Compares result to hdr->sha256[].
 *
 * @param slot_offset  Physical flash offset of the app slot
 * @param hdr          Previously validated image header (in DRAM)
 * @return             true if SHA-256 matches
 */
IRAM_ATTR bool bl_verify_sha256(uint32_t slot_offset,
                                 const app_image_hdr_t *hdr)
{
    bl_uart_puts("[BL] Computing SHA-256 (hardware engine)...\n");

    uint32_t payload_start = slot_offset + (uint32_t)sizeof(app_image_hdr_t);

    bool ok = bl_flash_sha256(payload_start,
                               hdr->payload_length,
                               g_sha256_computed);
    if (!ok) {
        bl_uart_puts("[BL-ERR] SHA-256 computation failed\n");
        return false;
    }

    /* Constant-time comparison to prevent timing attacks */
    uint8_t diff = 0U;
    for (uint32_t i = 0U; i < SHA256_DIGEST_BYTES; i++) {
        diff |= g_sha256_computed[i] ^ hdr->sha256[i];
    }

    if (diff != 0U) {
        bl_uart_puts("[BL-ERR] SHA-256 MISMATCH — image corrupted or tampered!\n");
        return false;
    }

    bl_uart_puts("[BL] SHA-256 OK\n");
    return true;
}

/* =========================================================================
 * OTA STATE / BOOT SLOT SELECTION
 * ========================================================================= */

/** DRAM-resident OTA control block */
DRAM_ATTR static ota_control_t g_ota_ctrl;

/**
 * @brief Read and validate the OTA control block from flash.
 *
 * Reads the ota_control_t struct from FLASH_PARTITION_TBL_OFFSET (0x8000).
 * Validates the magic word and CRC32. Returns the active boot slot.
 *
 * If the OTA state is invalid/uninitialized: defaults to BOOT_SLOT_1.
 *
 * @return Active boot slot (BOOT_SLOT_1 or BOOT_SLOT_2)
 */
IRAM_ATTR boot_slot_t bl_read_boot_slot(void)
{
    bl_flash_read(FLASH_PARTITION_TBL_OFFSET,
                  (uint8_t *)&g_ota_ctrl,
                  sizeof(ota_control_t));

    /* Validate magic */
    if (g_ota_ctrl.magic != OTA_STATE_MAGIC) {
        bl_uart_puts("[BL] OTA state absent — defaulting to Slot 1\n");
        return BOOT_SLOT_1;
    }

    /* Validate OTA state CRC32 */
    uint32_t crc_len = (uint32_t)sizeof(ota_control_t) - sizeof(uint32_t);
    uint32_t ota_crc = CRC32_INIT;
    ota_crc = crc32_update(ota_crc, (const uint8_t *)&g_ota_ctrl, crc_len);
    ota_crc ^= CRC32_XOR_OUT;

    if (ota_crc != g_ota_ctrl.crc32) {
        bl_uart_puts("[BL-WARN] OTA state CRC corrupt — defaulting to Slot 1\n");
        return BOOT_SLOT_1;
    }

    bl_uart_puts("[BL] OTA state valid\n");
    bl_uart_print_hex32("  active_slot", g_ota_ctrl.active_slot);

    return (boot_slot_t)g_ota_ctrl.active_slot;
}

/* =========================================================================
 * IAP (IN-APPLICATION PROGRAMMING) MODE
 *
 * IAP mode allows firmware update over UART0.
 * Two trigger paths:
 *   A) GPIO 0 LOW at boot  → hardware button on ESP32-CAM BOOT pin
 *   B) RTC_CNTL_STORE6 == 0xDEADBEEF → software request from running app
 *
 * After detecting IAP, the bootloader:
 *   1. Clears the RTC scratch register (prevent stuck-in-IAP loops)
 *   2. Sends a ready byte on UART ('C' = XModem-CRC ready)
 *   3. Loops waiting for firmware upload (protocol TBD by host tool)
 * ========================================================================= */

/**
 * @brief Check both IAP trigger paths.
 *
 * @return true if IAP mode should be entered
 */
IRAM_ATTR bool bl_check_iap_trigger(void)
{
    /* ── PATH A: GPIO 0 hardware trigger ────────────────────────────── */
    /*
     * Configure GPIO 0 as input with pull-up, wait 10 ms for debounce,
     * then sample. Pull-up ensures a floating pin reads HIGH (normal boot).
     * Holding the BOOT button pulls GPIO 0 LOW through the physical button.
     */
    bl_gpio_set_input_pullup(IAP_TRIGGER_GPIO);
    bl_delay_ms(10U);  /* 10 ms debounce — eliminates button bounce artifacts */

    uint8_t gpio_level = bl_gpio_read(IAP_TRIGGER_GPIO);
    if (gpio_level == IAP_TRIGGER_ACTIVE_LEVEL) {
        bl_uart_puts("[BL] IAP trigger: GPIO 0 is LOW (hardware button)\n");
        return true;
    }

    /* ── PATH B: Software trigger via RTC scratch register ───────────── */
    /*
     * RTC_CNTL_STORE6 persists across software resets (but not POR).
     * An application that wants to enter IAP on next boot writes:
     *   REG_WRITE(RTC_CNTL_STORE6_REG, 0xDEADBEEF);
     *   software_reset();
     * The bootloader reads it here and checks for the magic value.
     */
    uint32_t rtc_flag = REG_READ(RTC_CNTL_STORE6_REG);
    if (rtc_flag == IAP_MAGIC_WORD) {
        bl_uart_puts("[BL] IAP trigger: RTC STORE6 software magic matched\n");

        /* IMPORTANT: Clear the flag NOW to prevent infinite IAP loops.
         * If the device is power-cycled mid-IAP, the next boot will
         * proceed normally (flag is gone) rather than entering IAP again.
         * POR would clear this anyway, but software resets would not.
         */
        REG_WRITE(RTC_CNTL_STORE6_REG, IAP_MAGIC_CLEAR);
        return true;
    }

    return false;  /* Neither trigger asserted → normal boot */
}

/**
 * @brief Enter IAP firmware update mode.
 *
 * This function never returns under normal operation.
 * It signals the host tool via UART and loops waiting for firmware data.
 *
 * Production implementation would include:
 *   • XModem-CRC or custom framed packet reception
 *   • Flash sector erase and page programming via SPI1
 *   • CRC verification of received image
 *   • OTA state update to mark the new slot pending
 *   • Software reset to reboot into new image
 */
IRAM_ATTR __attribute__((noreturn))
void bl_enter_iap_mode(void)
{
    bl_uart_puts("\n");
    bl_uart_puts("+==============================================+\n");
    bl_uart_puts("|  ESP32 Secondary BL — IAP PROGRAMMING MODE  |\n");
    bl_uart_puts("|  Awaiting firmware transfer on UART0...      |\n");
    bl_uart_puts("+==============================================+\n\n");

    /*
     * 'C' character signals XModem-CRC readiness to the host upload tool.
     * Standard XModem-CRC protocol: receiver sends 'C' to indicate it
     * wants CRC-16 checksums rather than simple 1-byte checksums.
     */
    bl_uart_putc('C');

    /* IAP receive loop — replace with actual protocol implementation */
    while (1) {
        /* TODO: Receive XModem-CRC packets, erase/program Slot 2, reboot */
        __asm__ volatile("nop");
    }

    __builtin_unreachable();
}

/* =========================================================================
 * APPLICATION EXECUTION HANDOFF
 *
 * ==========================================================================
 * XTENSA LX6 VECTOR RELOCATION — SILICON MECHANICS
 * ==========================================================================
 *
 * The VECBASE special register (SR 231 = 0xE7) is the cornerstone of
 * Xtensa exception dispatch. It holds the BASE ADDRESS of the vector table.
 *
 * Vector table structure (Xtensa vs ARM — critical difference):
 *
 *   ARM Cortex-M:
 *     VTOR register → points to a TABLE OF ADDRESSES.
 *     Each entry is a 32-bit POINTER to a handler function.
 *     CPU dereferences the pointer to find the handler's address.
 *
 *   Xtensa LX6:
 *     VECBASE register → points to EXECUTABLE HANDLER CODE DIRECTLY.
 *     Each "vector" is actual assembly code at a fixed byte OFFSET from VECBASE.
 *     CPU jumps directly to (VECBASE + exception_level_offset).
 *     No table lookup, no pointer dereference — the code IS the vector.
 *
 * Xtensa exception dispatch sequence (hardware):
 *   1. Exception/interrupt detected at end of current instruction
 *   2. CPU saves PC → EPC[level], saves PS → EPS[level]  (SR writes, 1 cycle)
 *   3. PS.INTLEVEL ← exception_level  (masks interrupts at or below this level)
 *   4. PC ← VECBASE + level_offset    (jump to vector code)
 *   5. Vector code runs: typically saves registers, calls C handler
 *
 * After bl_jump_to_app(), the application firmware must have:
 *   • Its own IRAM segment loaded at hdr->entry_addr region
 *   • Its own vector table code at offsets from its VECBASE address
 *   • The app linker script must align its VECBASE to 256 bytes
 *
 * ==========================================================================
 * EXECUTION HANDOFF SEQUENCE
 * ==========================================================================
 *
 *   1. RSIL a4, 15   → Atomically mask all interrupts (PS.INTLEVEL = 15)
 *                       Prevents any interrupt from firing between steps 2-5
 *
 *   2. WSR %1, vecbase → Set VECBASE to application's vector table base
 *                        Application's exception vectors become active
 *                        Any exception from this point → app's handler
 *
 *   3. ISYNC          → Flush the instruction prefetch pipeline.
 *                        MANDATORY after WSR vecbase.
 *                        Without ISYNC: the pipeline may have already fetched
 *                        instructions using the OLD VECBASE. If an exception
 *                        occurs before those fetched instructions retire,
 *                        the CPU would jump to the old (bootloader) vectors.
 *                        ISYNC guarantees the new VECBASE takes effect before
 *                        ANY subsequent instruction fetch.
 *
 *   4. MEMW            → Full memory barrier.
 *                        Ensures all IRAM copies (bl_load_iram_segment) and
 *                        cache configuration writes have propagated through
 *                        the memory system before we read from IRAM.
 *
 *   5. JX %0          → Unconditional register jump to application entry point.
 *                        JX loads the PC from the source register.
 *                        NO link register is written (unlike CALL/CALLX).
 *                        This is a one-way, non-returning jump.
 *                        Bootloader's stack, frame, and variables are abandoned.
 *                        The application's reset handler begins with a clean slate.
 * ========================================================================= */

/**
 * @brief Load application's IRAM segment from flash to IRAM.
 *
 * The application binary may have a segment that must run from IRAM
 * (interrupt handlers, cache-disable routines). This segment is stored
 * in flash and must be DMA-copied to the target IRAM address before jump.
 *
 * For this bootloader, we load the first segment of the app image.
 * A full implementation would iterate over all segment headers.
 *
 * @param slot_offset   Physical flash offset of the app slot
 * @param hdr           Validated app image header
 * @return              Virtual address of the application VECBASE (entry_addr)
 */
IRAM_ATTR static uint32_t bl_load_app_segment(uint32_t slot_offset,
                                               const app_image_hdr_t *hdr)
{
    /* Read the first segment header (immediately after the image header) */
    DRAM_ATTR static app_segment_hdr_t seg_hdr;
    uint32_t seg_hdr_offset = slot_offset + (uint32_t)sizeof(app_image_hdr_t);

    bl_flash_read(seg_hdr_offset, (uint8_t *)&seg_hdr, sizeof(app_segment_hdr_t));

    bl_uart_puts("[BL] Loading IRAM segment:\n");
    bl_uart_print_hex32("  flash_src ", seg_hdr_offset + APP_SEGMENT_HDR_SIZE);
    bl_uart_print_hex32("  load_addr ", seg_hdr.load_addr);
    bl_uart_print_hex32("  data_len  ", seg_hdr.data_len);

    /* Copy segment data from flash → IRAM destination */
    uint32_t data_src  = seg_hdr_offset + (uint32_t)APP_SEGMENT_HDR_SIZE;
    uint8_t *iram_dest = (uint8_t *)(uintptr_t)seg_hdr.load_addr;
    uint32_t remaining = seg_hdr.data_len;

    /* Use the static scratch buffer for chunked copy */
    flash_cache_disable();
    while (remaining > 0U) {
        size_t chunk = (remaining > SPI1_MAX_TRANSFER_BYTES)
                        ? SPI1_MAX_TRANSFER_BYTES
                        : (size_t)remaining;

        spi1_read_raw(data_src, g_read_scratch, chunk);

        /* Copy from DRAM scratch to IRAM destination byte by byte
         * (unaligned IRAM writes require byte access on Xtensa) */
        for (size_t i = 0U; i < chunk; i++) {
            iram_dest[i] = g_read_scratch[i];
        }

        iram_dest  += chunk;
        data_src   += (uint32_t)chunk;
        remaining  -= (uint32_t)chunk;
    }
    flash_cache_enable();

    bl_uart_puts("[BL] IRAM segment loaded OK\n");

    /* Return the application's entry point (stored in image header) */
    return hdr->entry_addr;
}

/**
 * @brief Jump to the application — the point of no return.
 *
 * After this function executes the `jx` instruction, the ESP32 PRO CPU
 * is running the application firmware. The bootloader stack is abandoned.
 * The application is responsible for all subsequent hardware initialization.
 *
 * @param entry_addr    Application reset handler virtual address (from header)
 * @param vecbase_addr  Application's VECBASE address (typically same as IRAM base)
 *
 * This function is declared __attribute__((noreturn)):
 *   • GCC will not generate any function epilogue (no RETW)
 *   • Callers receive a compile-time warning if they assume it returns
 *   • __builtin_unreachable() after the asm tells the optimizer this is dead code
 */
IRAM_ATTR __attribute__((noreturn))
void bl_jump_to_app(uint32_t entry_addr, uint32_t vecbase_addr)
{
    bl_uart_puts("[BL] Performing execution handoff to application\n");
    bl_uart_print_hex32("  entry_addr  ", entry_addr);
    bl_uart_print_hex32("  vecbase_addr", vecbase_addr);
    bl_uart_puts("[BL] *** BOOTLOADER EXITS — APP OWNS CPU ***\n\n");

    /*
     * Re-disable WDT one final time in case something re-enabled it.
     * The application will configure its own WDT after startup.
     */
    bl_wdt_disable();

    /*
     * Inline assembly for execution handoff.
     *
     * Constraint explanation:
     *   "r"(entry_addr)   → %0 : GCC places entry_addr in a general-purpose
     *                             register (a0..a15). We use it with JX.
     *   "r"(vecbase_addr) → %1 : GCC places vecbase_addr in another register.
     *                             We use it with WSR vecbase.
     *   "a4"              : We clobber a4 (used by RSIL to save old PS).
     *   "memory"          : Tells GCC that the asm may read/write memory
     *                       in unpredictable ways — prevents dangerous
     *                       load/store hoisting across this boundary.
     *
     * NOTE: We do NOT restore the old PS value (saved by RSIL into a4).
     *   We want interrupts to STAY masked until the application's
     *   reset handler explicitly unmasks them by writing PS.INTLEVEL = 0.
     *   The application's startup sequence is responsible for:
     *     1. Setting up its own interrupt handlers
     *     2. Calling RSIL or writing PS to restore interrupts
     */
    __asm__ volatile (

        /* ── Step 1: Mask all interrupts atomically ───────────────────
         *
         * RSIL Rx, imm4:
         *   Reads current PS → Rx (we use a4, clobber it)
         *   Sets PS.INTLEVEL = imm4 (15 = mask all levels)
         *   This is ATOMIC — no interrupt can fire between read and write.
         *
         * After this instruction, INTLEVEL = 15.
         * No hardware interrupt (level 1..7) can be taken from this point.
         * The CPU will still DETECT interrupts but will not DISPATCH them.
         */
        "rsil    a4, 15\n\t"            /* Save old PS → a4; set INTLEVEL=15 */

        /* ── Step 2: Relocate exception vector table to application ───
         *
         * WSR Rx, vecbase:
         *   Writes Rx (containing vecbase_addr) to SR 231 (VECBASE register).
         *   VECBASE must be 256-byte aligned (hardware enforced — lower 8 bits
         *   of any value written are silently forced to 0 by the hardware).
         *
         * Effect: all future exceptions/interrupts dispatch to application vectors.
         * Any exception between WSR and ISYNC would still use old vectors
         * because the pipeline hasn't flushed yet → hence ISYNC is required.
         */
        "wsr     %1, vecbase\n\t"       /* VECBASE ← app's 256-byte-aligned base */

        /* ── Step 3: Flush instruction pipeline ───────────────────────
         *
         * ISYNC:
         *   Stalls the processor until all instructions in the pipeline
         *   ahead of ISYNC have been fetched, decoded, and committed.
         *   After ISYNC, the processor re-fetches the next instruction
         *   using the newly-written VECBASE (and any other SR changes).
         *
         * This is MANDATORY after writing VECBASE.
         * Without ISYNC, the pipeline may have already fetched the next
         * few instructions speculatively using the old VECBASE. If an
         * exception occurred during those speculative fetches, the old
         * (bootloader) exception vector would be invoked incorrectly.
         *
         * ISYNC is also needed after WSR PS, WSR windowbase, etc.
         */
        "isync\n\t"                     /* Flush pipeline — new VECBASE takes effect */

        /* ── Step 4: Full memory barrier ──────────────────────────────
         *
         * MEMW:
         *   Stalls the Xtensa load/store unit until all outstanding
         *   memory operations (loads and stores) on the AHB/APB bus complete.
         *   This includes pending SPI0 cache enable writes and IRAM segment
         *   copy stores.
         *
         * Without MEMW: it's theoretically possible (depending on pipeline
         *   implementation) for the JX branch target fetch to observe stale
         *   IRAM content if the segment copy's stores haven't committed yet.
         * With MEMW: guaranteed all stores are visible before we start
         *   executing from the target address.
         */
        "memw\n\t"                      /* Memory barrier: all pending stores commit */

        /* ── Step 5: Jump to application entry point ─────────────────
         *
         * JX Rx:
         *   Sets PC = Rx (register-indirect unconditional jump)
         *   Does NOT update the link register (a0) — this is NOT a CALL.
         *   After JX, the bootloader's entire call stack is orphaned.
         *   The CPU begins fetching and executing instructions at entry_addr.
         *
         * entry_addr = hdr->entry_addr = the virtual address of the
         *   application's reset handler, which will be in IRAM after
         *   bl_load_app_segment() copied it there.
         *
         * The application's reset_handler is responsible for:
         *   • Setting PS.INTLEVEL = 0 (enable interrupts)
         *   • Initializing its own stack (a1)
         *   • Zeroing its own BSS
         *   • Copying its own .data section
         *   • Starting the RTOS scheduler or main()
         */
        "jx      %0\n\t"                /* PC ← entry_addr; one-way non-returning jump */

        :                               /* No output operands */
        : "r"(entry_addr),              /* %0 = application entry point address */
          "r"(vecbase_addr)             /* %1 = application VECBASE address */
        : "a4", "memory"                /* Clobbers: a4 (RSIL dest), memory */
    );

    /* UNREACHABLE — JX transfers control unconditionally */
    __builtin_unreachable();
}

/* =========================================================================
 * BOOTLOADER MAIN
 * ========================================================================= */

/**
 * @brief Secondary bootloader entry point (called from startup.S via CALL0).
 *
 * @note Declared __attribute__((noreturn)) because we either jump to the
 *       application (via bl_jump_to_app) or hang in IAP mode (bl_enter_iap_mode).
 *       Neither function ever returns.
 */
__attribute__((noreturn))
void bootloader_main(void)
{
    /* ── Phase 1: Hardware safety setup ────────────────────────────── */

    /*
     * Disable watchdog timers FIRST, before doing anything else.
     * WDTs are running from ROM BL, and our SHA/CRC loops take seconds.
     */
    bl_wdt_disable();

    /* Initialize UART0 for debug output at 115200 baud */
    bl_uart_init();

    /* Print bootloader banner */
    bl_uart_puts("\n");
    bl_uart_puts("+-------------------------------------------------------+\n");
    bl_uart_puts("|   ESP32 Custom Secondary Bootloader v1.0.0            |\n");
    bl_uart_puts("|   Target: ESP32-D0WD / ESP32-CAM                     |\n");
    bl_uart_puts("|   Build date: " __DATE__ " " __TIME__ "          |\n");
    bl_uart_puts("+-------------------------------------------------------+\n\n");

    /* ── Phase 2: Pre-computation (DRAM-side initialization) ─────────── */

    /*
     * Initialize the 256-entry CRC32 lookup table in DRAM.
     * Must happen before any bl_flash_crc32() or header CRC validation calls.
     * Table generation: O(256 × 8) = 2048 iterations, negligible time.
     */
    bl_crc32_init_table();
    bl_uart_puts("[BL] CRC32 table ready (256 entries @ DRAM)\n");

    /* ── Phase 3: Dual-path IAP trigger check ────────────────────────── */

    if (bl_check_iap_trigger()) {
        /* Either GPIO 0 was LOW or STORE6 had the magic word */
        bl_enter_iap_mode();  /* Never returns */
    }

    bl_uart_puts("[BL] Normal boot mode\n");

    /* ── Phase 4: Select active boot slot via OTA state ─────────────── */

    boot_slot_t active_slot = bl_read_boot_slot();

    uint32_t slot_offset;
    if (active_slot == BOOT_SLOT_2) {
        slot_offset = FLASH_APP_SLOT2_OFFSET;
        bl_uart_puts("[BL] Selected: App Slot 2 (OTA)\n");
    } else {
        slot_offset = FLASH_APP_SLOT1_OFFSET;
        bl_uart_puts("[BL] Selected: App Slot 1 (primary)\n");
    }
    bl_uart_print_hex32("  slot_offset", slot_offset);

    /* ── Phase 5: Validate image header ─────────────────────────────── */

    if (!bl_validate_image_header(slot_offset, &g_img_hdr)) {
        /*
         * If Slot 2 fails, attempt fallback to Slot 1.
         * If Slot 1 also fails, we have no valid image — enter IAP.
         */
        if (active_slot == BOOT_SLOT_2) {
            bl_uart_puts("[BL-WARN] Slot 2 header invalid. Falling back to Slot 1.\n");
            slot_offset = FLASH_APP_SLOT1_OFFSET;
            if (!bl_validate_image_header(slot_offset, &g_img_hdr)) {
                bl_uart_puts("[BL-ERR] Slot 1 also invalid. Entering IAP.\n");
                bl_enter_iap_mode();
            }
        } else {
            bl_uart_puts("[BL-ERR] Primary slot invalid. Entering IAP.\n");
            bl_enter_iap_mode();
        }
    }

    /* ── Phase 6: SHA-256 integrity verification ─────────────────────── */

    if (!bl_verify_sha256(slot_offset, &g_img_hdr)) {
        bl_uart_puts("[BL-ERR] Image integrity failed. Entering IAP.\n");
        bl_enter_iap_mode();
    }

    /* ── Phase 7: Load application IRAM segment from flash ───────────── */

    /*
     * The application's IRAM segment (vectors + fast ISRs) must be
     * copied from flash to IRAM before we jump. The entry point address
     * is in the app image header (hdr->entry_addr).
     */
    uint32_t app_entry = bl_load_app_segment(slot_offset, &g_img_hdr);

    /*
     * The application's VECBASE is typically the start of its IRAM region.
     * We assume the application places its vector table at its IRAM base
     * (entry_addr & ~0xFF) rounded down to 256-byte boundary.
     * Adjust if the application uses a different convention.
     */
    uint32_t app_vecbase = app_entry & ~0xFFUL;  /* Round down to 256-byte boundary */

    bl_uart_print_hex32("[BL] app_entry  ", app_entry);
    bl_uart_print_hex32("[BL] app_vecbase", app_vecbase);

    /* ── Phase 8: Jump to application — no return ─────────────────────── */

    bl_uart_puts("[BL] All checks passed. Handing off to application.\n\n");

    bl_jump_to_app(app_entry, app_vecbase);  /* Never returns */

    /* __attribute__((noreturn)) + __builtin_unreachable() in bl_jump_to_app */
    __builtin_unreachable();
}
