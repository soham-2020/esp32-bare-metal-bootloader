/**
 * @file    linker.ld
 * @brief   GNU LD linker script for the ESP32-D0WD secondary bootloader.
 *
 * ==========================================================================
 * ADDRESS SPACE OVERVIEW — ESP32 MEMORY MAP
 * ==========================================================================
 *
 * Physical Memory (on-chip):
 * ─────────────────────────────────────────────────────────────────────────
 *  Internal ROM0:  32 KB  (mask ROM — Espressif first-stage bootloader)
 *  Internal ROM1:  32 KB  (ROM library functions — rtc_init, etc.)
 *  Internal SRAM0: 192 KB (IBUS+DBUS, used as IRAM + DRAM)
 *  Internal SRAM1: 128 KB (DBUS only, used as DRAM + CACHE)
 *  RTC FAST MEM:    8 KB  (survives deep sleep; RTC domain)
 *  RTC SLOW MEM:    8 KB  (survives deep sleep; RTC domain)
 *
 * Virtual Address Map (as seen by Xtensa CPU):
 * ─────────────────────────────────────────────────────────────────────────
 *  0x00000000 – 0x3FFFFFFF : DBUS (Data Bus) address space
 *    0x3FF00000 – 0x3FF7FFFF : Peripheral MMIO (DPORT, SPI, UART, GPIO, ...)
 *    0x3FF80000 – 0x3FF81FFF : RTC FAST memory (DBUS access, 8 KB)
 *    0x3FFAE000 – 0x3FFFFFFF : Internal SRAM (DRAM, data bus)
 *                               Our DRAM region starts here
 *
 *  0x40000000 – 0x4FFFFFFF : IBUS (Instruction Bus) address space
 *    0x40000000 – 0x40007FFF : Internal ROM0 (32 KB — Espressif mask ROM)
 *    0x40008000 – 0x4000FFFF : Internal ROM1 (32 KB — ROM library)
 *    0x40070000 – 0x4007FFFF : RTC FAST memory (IBUS access, 64 KB view)
 *    0x40080000 – 0x400BFFFF : Internal SRAM0 (IRAM, 256 KB)
 *                               Our BL binary goes here (starting at 0x40080000)
 *    0x400C0000 – 0x40BFFFFF : External SPI flash (XIP via SPI0 cache)
 *                               Virtual flash access window
 *    0x3F400000 – 0x3F7FFFFF : External SPI flash (DBUS read-only window)
 *
 * ==========================================================================
 * LMA vs. VMA — THE CRITICAL DISTINCTION
 * ==========================================================================
 *
 * LMA (Load Memory Address): Where binary data PHYSICALLY RESIDES at reset.
 *   For our bootloader, the ROM BL reads from physical flash and copies
 *   our entire binary to IRAM. So LMA = "position in IRAM after ROM BL copy".
 *
 * VMA (Virtual Memory Address): Where the CPU EXPECTS to find data at runtime.
 *   Code:      VMA = LMA = IRAM (0x40080000) — ROM BL copies code there
 *   Data (.data): LMA = in IRAM (after the code); VMA = DRAM (0x3FFAE000+)
 *     startup.S copies .data from its LMA (IRAM) to its VMA (DRAM).
 *
 * The AT() directive in GNU LD specifies an alternative LMA for a section:
 *   .data : { ... } > dram0  AT > iram0
 *     VMA = dram0 region (CPU uses DRAM addresses for .data variables)
 *     LMA = following iram0 content (ROM BL loads data section into IRAM)
 *   _data_lma = LOADADDR(.data) gives the LMA (IRAM) source address.
 *
 * ==========================================================================
 * VECBASE ALIGNMENT REQUIREMENT — SILICON CONSTRAINT
 * ==========================================================================
 *
 * The Xtensa VECBASE special register has a hardware enforcement:
 *   Bits [7:0] of VECBASE are ALWAYS READ AS 0.
 *   Writing a non-256-aligned address silently masks the lower 8 bits.
 *
 * Therefore:
 *   _bl_vecbase MUST be at a 256-byte-aligned address in IRAM.
 *   .iram.vectors section uses .align 256 in startup.S.
 *   The linker script places .iram.vectors FIRST in the IRAM output section.
 *   Since IRAM starts at 0x40080000 (already 256-byte aligned), the first
 *   instruction at that address is automatically 256-byte aligned.
 *
 * ASSERT in this file verifies the alignment at link time.
 *
 * ==========================================================================
 * BOOTLOADER SIZE BUDGET
 * ==========================================================================
 *
 *  Flash allocation (sectors 1–7):   0x7000 = 28,672 bytes = 28 KB
 *  IRAM budget (code + vectors):     28 KB maximum
 *  DRAM budget (data + BSS + stack): 16 KB allocated
 *
 * The ROM BL copies the binary from flash to IRAM byte-for-byte.
 * Our binary must not exceed 28 KB or it will overwrite other flash regions.
 */

/* ===========================================================================
 * MEMORY REGION DEFINITIONS
 * =========================================================================== */

MEMORY
{
    /*
     * iram0: Internal SRAM mapped to the Instruction Bus (IBUS).
     * This is where the ROM BL copies our bootloader binary.
     *
     * ORIGIN: 0x40080000 — Start of SRAM0 on the IBUS.
     * LENGTH: 0x7000 (28 KB) — Our bootloader flash allocation limit.
     *
     * Permissions: r=readable, w=writable, x=executable
     * All three are valid for IRAM (it's true SRAM — read/write/execute).
     */
    iram0   (rwx) : ORIGIN = 0x40080000, LENGTH = 0x7000    /* 28 KB IRAM */

    /*
     * dram0: Internal SRAM mapped to the Data Bus (DBUS).
     * Used for initialized globals (.data), BSS, and the call stack.
     *
     * ORIGIN: 0x3FFAE000 — Start of SRAM1 on DBUS.
     *   (Actual usable DRAM starts at 0x3FFAE000 per ESP32 memory map.)
     *   We skip the very beginning of DRAM to avoid the ROM BL data structures
     *   that the ROM places there (ROM BL stack, ROM log buffer, etc.).
     *
     * LENGTH: 0x4000 (16 KB) — Generous allocation for BL data + stack.
     *
     * Permissions: r=readable, w=writable (no execute — data bus)
     */
    dram0   (rw)  : ORIGIN = 0x3FFAE000, LENGTH = 0x4000    /* 16 KB DRAM */

    /*
     * flash_xip: SPI flash mapped to the IBUS via SPI0 cache.
     * The CPU uses this address window to execute code stored in flash
     * (XIP — execute in place) when the SPI0 cache is active.
     *
     * ORIGIN: 0x400C0000 — Start of the XIP IBUS flash window.
     * LENGTH: 0x300000 (3 MB) — Covers our flash layout.
     *
     * This region is INFORMATIONAL in this linker script. We reference it
     * in comments and potential future sections, but our bootloader binary
     * is fully in IRAM (not XIP). The application binary uses this window.
     */
    flash_xip (rx) : ORIGIN = 0x400C0000, LENGTH = 0x300000  /* 3 MB XIP window */
}

/* ===========================================================================
 * SECTION LAYOUT DEFINITIONS
 * =========================================================================== */

SECTIONS
{
    /* ======================================================================
     * .iram.vectors — Exception vector table (MUST be first in IRAM)
     *
     * VMA: 0x40080000 (IRAM start — this is our VECBASE value)
     * LMA: same as VMA (ROM BL copies code here, no LMA/VMA split for code)
     *
     * Placement rules:
     *   1. MUST be at a 256-byte-aligned address (VECBASE requirement)
     *   2. MUST be the very first section in IRAM output
     *   Both are automatically satisfied because iram0 ORIGIN = 0x40080000
     *   (which is 256-byte aligned) and this section comes first.
     *
     * startup.S has .align 256 in the .iram.vectors section to guarantee
     * alignment even if the linker places this section after something else.
     *
     * KEEP prevents the linker's --gc-sections from dead-stripping vectors.
     * Without KEEP, if no C code explicitly references _bl_vecbase, the
     * linker might remove the entire vector table (breaking everything).
     * ====================================================================== */
    .iram.vectors :
    {
        _bl_vecbase = .;          /* Symbol = address of our VECBASE */
        KEEP(*(.iram.vectors))    /* Retain vector table even with --gc-sections */
        . = ALIGN(4);             /* 4-byte align after vectors */
    } > iram0

    /* ======================================================================
     * .iram.text — Primary bootloader executable code
     *
     * VMA: IRAM (immediately after .iram.vectors)
     * LMA: same as VMA (all code is in IRAM, no LMA/VMA split)
     *
     * Contains:
     *   • _bootloader_start (startup assembly entry point)
     *   • All IRAM_ATTR C functions (flash read, CRC, SHA, jump, etc.)
     *   • Default .text (non-IRAM_ATTR functions also land here since we
     *     have no XIP flash capability in the bootloader)
     *   • .literal sections (Xtensa constant pools adjacent to code)
     *
     * Why .literal sections:
     *   Xtensa L32R instruction loads 32-bit constants from a nearby literal
     *   pool (within ±512 KB). The -mtext-section-literals compiler flag
     *   places literals in .literal subsections adjacent to the function.
     *   These must be in the same IRAM region as the code.
     * ====================================================================== */
    .iram.text :
    {
        . = ALIGN(4);
        _iram_text_start = .;     /* Symbol: start of bootloader code */

        /* Entry point must be first in .iram.text after vectors */
        *startup.o(.iram.text)    /* startup.S entry point code */

        /* All IRAM_ATTR C functions */
        *(.iram.text)
        *(.iram.text.*)

        /* Standard .text: in a bootloader, all code runs from IRAM */
        *(.text)
        *(.text.*)

        /* Xtensa constant pools (L32R literal tables) */
        *(.literal)
        *(.literal.*)

        . = ALIGN(4);
        _iram_text_end = .;       /* Symbol: end of bootloader code (for size check) */
    } > iram0

    /* ======================================================================
     * .iram.rodata — Read-only data that must survive cache disable
     *
     * String literals used in IRAM_ATTR functions (like debug print strings)
     * must be in IRAM, not in flash (XIP), because we disable the flash cache
     * during SPI1 transactions. If a string were in XIP flash and we tried to
     * access it with cache disabled, the CPU would fault.
     *
     * With -mtext-section-literals, short strings adjacent to functions end
     * up in .literal sections (handled above). Longer const arrays go here.
     * ====================================================================== */
    .iram.rodata :
    {
        . = ALIGN(4);
        *(.iram.rodata)
        *(.iram.rodata.*)
        . = ALIGN(4);
    } > iram0

    /* ======================================================================
     * .dram.data — Initialized global and static variables
     *
     * VMA: dram0 region (0x3FFAE000+) — CPU reads/writes variables here at runtime
     * LMA: Follows .iram.text in IRAM (AT > iram0)
     *
     * The AT > iram0 directive causes the LINKER to:
     *   • Assign VMA = next available address in dram0 (runtime addresses)
     *   • Assign LMA = next available address in iram0 (stored addresses)
     *   • Include the section data in the iram0 output region of the binary
     *
     * The ROM BL copies our entire binary to IRAM, including this data.
     * So at boot, the .data content is sitting in IRAM at its LMA.
     * startup.S reads it from there (_data_lma) and copies it to DRAM (_data_start).
     *
     * Contains:
     *   • DRAM_ATTR globals (g_img_hdr, CRC table, buffers)
     *   • Standard .data section (initialized global variables)
     *   • .rodata: in a bare-metal BL, .rodata goes to DRAM too
     *     because it might be accessed with cache disabled
     * ====================================================================== */
    .dram.data :
    {
        . = ALIGN(4);
        _data_start = .;          /* Symbol: VMA start of .data in DRAM */

        /* DRAM_ATTR variables from our source */
        *(.dram.data)
        *(.dram.data.*)

        /* Standard initialized data */
        *(.data)
        *(.data.*)

        /*
         * Read-only data: placed in DRAM (not flash) so it's accessible
         * during flash cache disable windows (CRC table, string literals).
         */
        *(.rodata)
        *(.rodata.*)

        . = ALIGN(4);
        _data_end = .;            /* Symbol: VMA end of .data in DRAM */
    } > dram0 AT > iram0         /* VMA in DRAM; LMA follows code in IRAM */

    /*
     * _data_lma: The LMA (Load Memory Address) of .dram.data.
     * LOADADDR() is a GNU LD built-in that returns the LMA of a section.
     * startup.S uses this as the source address for the LMA→VMA copy loop.
     *
     * At runtime: _data_lma is in IRAM (e.g., 0x40080000 + code_size).
     * startup.S copies [_data_lma .. _data_lma + (_data_end - _data_start)]
     *              to   [_data_start .. _data_end] in DRAM.
     */
    _data_lma = LOADADDR(.dram.data);

    /* ======================================================================
     * .bss — Uninitialized (zero-initialized) global and static variables
     *
     * VMA: dram0 (immediately after .data in DRAM)
     * LMA: N/A — NOLOAD means no data in the binary file for this section.
     *
     * The C standard requires BSS to be zero at program start.
     * startup.S zeros [_bss_start .. _bss_end) with S32I zero writes.
     *
     * NOLOAD: tells the linker to assign VMA addresses but not output
     * any data to the binary for this section. BSS has no initial value
     * (it's all zeros by definition) so there's nothing to store.
     * ====================================================================== */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _bss_start = .;           /* Symbol: start of BSS in DRAM */

        *(.bss)
        *(.bss.*)
        *(COMMON)                 /* Tentative definitions (unresolved commons) */

        . = ALIGN(4);
        _bss_end = .;             /* Symbol: end of BSS in DRAM */
    } > dram0

    /* ======================================================================
     * .stack — Bootloader call stack
     *
     * VMA: dram0 (after BSS). Stack grows DOWNWARD.
     * LMA: N/A — NOLOAD (no data in binary; just reserved address space)
     *
     * Xtensa ABI requirements:
     *   • a1 (SP) must point to the TOP of the stack on function entry
     *   • Stack frames grow downward (push = decrement, then store)
     *   • SP must be 16-byte aligned at every CALL site
     *
     * We reserve 8 KB for the bootloader stack. This is generous —
     * our BL has shallow call depth. Increase if adding complex protocols.
     *
     * _bl_stack_top is the INITIAL SP value (highest address, empty stack).
     * startup.S writes: movi a1, _bl_stack_top
     * ====================================================================== */
    .stack (NOLOAD) :
    {
        . = ALIGN(16);            /* 16-byte aligned: Xtensa ABI requirement */
        _bl_stack_bottom = .;     /* Lowest valid stack address (stack guard boundary) */

        . += 0x2000;              /* Reserve 8 KB = 0x2000 bytes for stack */

        . = ALIGN(16);
        _bl_stack_top = .;        /* Initial SP value (highest address, empty stack) */
    } > dram0

    /* ======================================================================
     * /DISCARD/ — Sections to throw away
     *
     * These sections are generated by the compiler/assembler but serve no
     * purpose in a freestanding embedded binary:
     *   .comment:    Compiler/toolchain version string (GCC banner text)
     *   .debug_*:    DWARF debug information (excluded from final binary)
     *   .eh_frame*:  C++ exception unwind tables (we have no C++ exceptions)
     *   .note.*:     Build notes, ABI notes
     *   .gnu.attr*:  GNU attribute sections
     * ====================================================================== */
    /DISCARD/ :
    {
        *(.comment)
        *(.debug_info)
        *(.debug_abbrev)
        *(.debug_aranges)
        *(.debug_ranges)
        *(.debug_line)
        *(.debug_str)
        *(.debug_frame)
        *(.debug_loc)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.gnu.build-id)
        *(.note.ABI-tag)
        *(.gnu.attributes)
    }
}

/* ===========================================================================
 * ENTRY POINT DECLARATION
 *
 * ENTRY() tells the linker the symbol where execution begins.
 * This sets the ELF e_entry field (used by debuggers and objcopy --entry).
 * The ROM BL reads the binary header to find the entry point — it does NOT
 * use the ELF e_entry field directly. But setting this correctly helps tools.
 * =========================================================================== */
ENTRY(_bootloader_start)

/* ===========================================================================
 * COMPILE-TIME ASSERTIONS
 *
 * GNU LD ASSERT() checks conditions at link time. If the condition is false,
 * the link fails with the provided error message. These catch:
 *   • Overflowing the flash allocation budget (28 KB)
 *   • VECBASE misalignment (would cause silent exception dispatch bugs)
 *   • DRAM overflow (data + BSS + stack exceeding 16 KB)
 *
 * ASSERT is evaluated AFTER all symbols are resolved — it sees final values.
 * =========================================================================== */

/* ── Assert 1: Total IRAM code fits within 28 KB flash allocation ─────────
 *
 * (_iram_text_end - ORIGIN(iram0)) gives the total code bytes placed in IRAM.
 * Adding the .data LMA size (data that the ROM BL also copies to IRAM):
 *   Total binary = code + vectors + data (for ROM BL to copy to IRAM)
 * We check that code alone fits; add data if needed.
 */
ASSERT((_iram_text_end - ORIGIN(iram0)) <= LENGTH(iram0),
       "LINKER ERROR: Bootloader code exceeds 28 KB IRAM budget!")

/* ── Assert 2: VECBASE is 256-byte aligned ─────────────────────────────────
 *
 * (_bl_vecbase & 0xFF) must be 0.
 * If _bl_vecbase is not 256-byte aligned, the VECBASE register will silently
 * mask the lower 8 bits, pointing to the WRONG memory location, causing
 * random crashes on any exception.
 */
ASSERT((_bl_vecbase & 0xFF) == 0,
       "LINKER ERROR: _bl_vecbase is not 256-byte aligned! Fix .iram.vectors placement.")

/* ── Assert 3: _bl_stack_top fits within dram0 region ─────────────────────
 *
 * The stack top must not exceed the end of our allocated DRAM region.
 * If this fails, reduce stack size or increase dram0 LENGTH.
 */
ASSERT(_bl_stack_top <= (ORIGIN(dram0) + LENGTH(dram0)),
       "LINKER ERROR: BSS + data + stack exceed 16 KB DRAM budget!")

/* ── Assert 4: BSS start is 4-byte aligned (required for S32I zero loop) ──
 *
 * startup.S's BSS zeroing loop uses S32I (32-bit store). The destination
 * address must be 4-byte aligned for this to work correctly.
 */
ASSERT((_bss_start & 0x3) == 0,
       "LINKER ERROR: _bss_start is not 4-byte aligned!")

/* ── Assert 5: .data copy source is 4-byte aligned ────────────────────────
 *
 * startup.S's .data copy loop uses L32I/S32I (32-bit loads/stores).
 * Both source (_data_lma) and destination (_data_start) must be 4-byte aligned.
 */
ASSERT((_data_lma & 0x3) == 0,
       "LINKER ERROR: _data_lma is not 4-byte aligned!")

ASSERT((_data_start & 0x3) == 0,
       "LINKER ERROR: _data_start is not 4-byte aligned!")
