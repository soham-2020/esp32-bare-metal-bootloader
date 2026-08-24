/**
 * @file    startup.S
 * @brief   Xtensa LX6 bare-metal startup for the ESP32 secondary bootloader.
 *
 * ==========================================================================
 * WHO CALLS US AND IN WHAT STATE
 * ==========================================================================
 *
 * The Espressif mask ROM bootloader (not modifiable, burned into silicon)
 * runs immediately on power-on from internal ROM at 0x40000000.
 * It performs the following before handing control to us:
 *
 *   1. Configures the SPI0 flash controller (QSPI, 40 MHz by default)
 *   2. Reads the image descriptor at flash physical offset 0x0000 (our
 *      ROM-compatible header that we embed at the start of our binary)
 *   3. Validates the 0xE9 magic byte in that header
 *   4. Copies our bootloader binary from flash offset 0x1000 to IRAM
 *      starting at virtual address 0x40080000
 *   5. Issues a J (unconditional jump) to our _bootloader_start symbol
 *
 * CPU state on entry to _bootloader_start:
 *   • PC  = 0x40080000 (_bootloader_start in IRAM)
 *   • a0  = 0x40000000 (ROM bootloader return address — we ignore this)
 *   • a1  = undefined / contains ROM BL stack pointer (do NOT use yet)
 *   • a2  = ROM BL reserved data pointer (ignore)
 *   • INTLEVEL = 0 (interrupts enabled — change immediately)
 *   • VECBASE = points to ROM BL's vector table (we change this too)
 *   • WDT timer is running (must be disabled in bl_wdt_disable())
 *   • PLL is configured: CPU @ 240 MHz, APB @ 80 MHz
 *   • SPI0 cache is ENABLED (XIP works, code can run from flash too)
 *   • APP CPU (core 1) is held in reset by ROM BL
 *
 * ==========================================================================
 * XTENSA ABI PRIMER — REGISTER WINDOWS
 * ==========================================================================
 *
 * Xtensa LX6 uses a hardware register window mechanism.
 * Physical register file: 64 registers (a0..a63)
 * Software sees:          16 registers (a0..a15) — a "window" into the 64
 *
 * On CALL4/CALL8/CALL12 instructions, the hardware rotates the window:
 *   CALL4  → shifts window by 4 (a4..a7 of callee = a0..a3 of caller)
 *   CALL8  → shifts window by 8 (a8..a11 of callee = a0..a3 of caller)
 *
 * Special registers related to windows:
 *   WINDOWBASE  : Index of the current window's base register (0..15)
 *   WINDOWSTART : Bitmask of live (in-use) windows
 *   PS.CALLINC  : Window increment set by last CALL instruction
 *   PS.WOE      : Window Overflow Enable — if 1, CALL can trigger overflow
 *
 * We initialize both to a clean "window 0 only" state so that:
 *   • No spurious window overflow exception fires before our handler is set
 *   • CALL0 (flat ABI) in startup works correctly
 *
 * CALL0 vs CALL4/8/12:
 *   CALL0 saves the return address in a0 (caller's a0) without rotating
 *   the window. It is simpler for startup code that doesn't need deep stacks.
 *   CALL4 saves return address in a4 (callee's a0 after rotation).
 *   Our startup uses CALL0 for simplicity; bootloader_main uses ENTRY.
 *
 * ==========================================================================
 * LINKER SCRIPT SYMBOLS USED HERE
 * ==========================================================================
 *
 *   _bl_stack_top   : Top of bootloader stack in DRAM (initial SP value)
 *   _bss_start      : First address of BSS (uninitialized data) in DRAM
 *   _bss_end        : One-past-last address of BSS in DRAM
 *   _data_lma       : Load memory address of .data in flash/IRAM
 *   _data_start     : Virtual memory address of .data in DRAM
 *   _data_end       : End of .data in DRAM
 *   _bl_vecbase     : 256-byte aligned base for our BL vector table
 */

    /* ── Section declaration ──────────────────────────────────────────────
     * "ax" = allocatable + executable
     * .iram.text is our custom section name for IRAM-resident code.
     * The linker script maps .iram.text to the iram0 memory region (0x40080000).
     * All code in this file will be loaded to IRAM by the ROM bootloader.
     */
    .section    .iram.text, "ax"
    .align      4           /* 4-byte instruction alignment on Xtensa */

/* ==========================================================================
 * _bootloader_start — Primary entry point
 *
 * This symbol is the target of the ROM bootloader's final J instruction.
 * It MUST be the very first instruction in the output binary (guaranteed
 * by the linker script placing .iram.text first in the IRAM output section).
 * ========================================================================== */
    .global     _bootloader_start
    .type       _bootloader_start, @function
_bootloader_start:

    /* ── STEP 1: Mask all interrupts immediately ────────────────────────
     *
     * RSIL (Read/Set Interrupt Level) is an ATOMIC instruction that:
     *   a) Reads the current PS register into destination register
     *   b) Sets PS.INTLEVEL to the specified immediate value
     *   c) Does NOT modify any other PS bits
     *
     * Syntax: RSIL  Rx, imm4
     *   Rx   = receives the OLD PS value (useful if caller needs to restore)
     *   imm4 = new INTLEVEL (0=all enabled, 15=all masked on ESP32)
     *
     * INTLEVEL 15 masks all configurable hardware interrupts.
     * This is safe to do immediately — no context to save yet.
     *
     * We save old PS in a2 (unused after this point; just good practice).
     */
    rsil    a2, 15              /* a2 ← old PS, INTLEVEL ← 15 (disable all IRQs) */

    /* ── STEP 2: Set VECBASE to our bootloader's vector table ──────────
     *
     * The ROM bootloader left VECBASE pointing at its own exception table.
     * Any exception during our startup (e.g., load-use hazard, IRAM fault)
     * would dispatch to ROM handlers instead of ours.
     *
     * We update VECBASE to point to _bl_vecbase (our table in .iram.vectors)
     * BEFORE we do anything that might cause an exception.
     *
     * WSR (Write Special Register) writes an address register to a named SR.
     * "vecbase" is the Xtensa assembler name for SR 231 (0xE7).
     * ISYNC is MANDATORY after writing VECBASE:
     *   Without ISYNC, the Xtensa LX6 superscalar pipeline may have already
     *   fetched the next few instructions and cached their exception dispatch
     *   address using the OLD VECBASE. ISYNC flushes and restarts the pipeline.
     */
    movi    a3, _bl_vecbase     /* a3 ← address of our BL vector table */
    wsr     a3, vecbase         /* VECBASE ← a3 (must be 256-byte aligned!) */
    isync                       /* Flush instruction pipeline after VECBASE change */

    /* ── STEP 3: Initialize register windows cleanly ────────────────────
     *
     * WINDOWBASE: The index (0..15) of the window that maps to a0 currently.
     * WINDOWSTART: A bitmask where bit[N]=1 means window N is live.
     *
     * We set WINDOWBASE=0 and WINDOWSTART=0x1 (only window 0 live).
     * This ensures:
     *   • No previous window is considered "live" (avoids underflow exceptions)
     *   • CALL0 works correctly from this point onward
     *   • ENTRY/RETW in bootloader_main() will work as expected
     *
     * RSYNC: Unlike ISYNC (which stalls for instruction fetch completion),
     * RSYNC stalls until all pending register/SR operations complete.
     * Required after WSR to special registers like windowbase/windowstart.
     */
    movi    a0, 0               /* a0 ← 0 (WINDOWBASE value) */
    wsr     a0, windowbase      /* WINDOWBASE ← 0 */
    movi    a0, 1               /* a0 ← 1 (WINDOWSTART bitmask, window 0 only) */
    wsr     a0, windowstart     /* WINDOWSTART ← 0x1 */
    rsync                       /* Wait for SR writes to take effect */

    /* ── STEP 4: Enable Window Overflow Enable (WOE) bit in PS ─────────
     *
     * PS.WOE (bit 18) must be 1 for the CALL/ENTRY/RETW window rotation
     * mechanism to work. If WOE=0, CALL instructions still execute but
     * the hardware won't trigger window overflow/underflow exceptions,
     * causing silent register corruption in nested function calls.
     *
     * We read current PS, OR in bit 18, write back.
     * MOVI loads the bitmask. OR merges the bit. WSR writes back.
     * After WSR ps, ISYNC flushes so the new PS takes effect immediately.
     */
    rsr     a0, ps              /* a0 ← current PS register value */
    movi    a3, (1 << 18)       /* a3 ← WOE bit mask (bit 18 of PS) */
    or      a0, a0, a3          /* a0 ← PS with WOE bit set */
    wsr     a0, ps              /* PS ← updated value (WOE=1) */
    isync                       /* Flush pipeline after PS change */

    /* ── STEP 5: Initialize the stack pointer (a1 = SP) ─────────────────
     *
     * The Xtensa ABI requires a1 to hold the stack pointer.
     * The stack grows DOWNWARD (PUSH = decrement SP, then store).
     * _bl_stack_top is defined by the linker script as the HIGHEST address
     * of the stack region (entry SP value for an empty stack).
     *
     * Stack alignment: Xtensa ABI requires 16-byte alignment.
     * _bl_stack_top is guaranteed 16-byte aligned by the linker script.
     *
     * MOVI loads an arbitrary 32-bit address into a register by emitting a
     * literal pool entry and an L32R (load 32-bit relative) instruction.
     */
    movi    a1, _bl_stack_top   /* a1 (SP) ← top of bootloader stack in DRAM */

    /* ── STEP 6: Zero-initialize BSS segment ────────────────────────────
     *
     * C standard requires all uninitialized static/global variables to be
     * zero at program start. These live in the .bss section in DRAM.
     * The linker script marks BSS NOLOAD (no data in flash binary).
     * We must manually zero the DRAM region [_bss_start .. _bss_end).
     *
     * Strategy: write 32-bit zero words in a tight loop.
     * This is faster than byte-by-byte and Xtensa S32I takes 3 cycles.
     *
     * Corner case: if BSS is empty (_bss_start == _bss_end), skip loop.
     *
     * Register allocation:
     *   a4 = current BSS pointer (starts at _bss_start, increments by 4)
     *   a5 = BSS end pointer    (fixed, equals _bss_end)
     *   a6 = zero constant      (value to store)
     */
    movi    a4, _bss_start      /* a4 ← &bss[0] */
    movi    a5, _bss_end        /* a5 ← &bss[end] (exclusive) */
    movi    a6, 0               /* a6 ← 0 (store value) */

    /* If BSS is empty (start == end), skip the loop entirely */
    beq     a4, a5, .Lbss_done

.Lbss_loop:
    /* S32I: Store 32-bit from register to [base + offset], offset must be
     * a 8-bit multiple-of-4 constant (0..1020). offset=0 here.
     * This instruction takes 3 cycles on Xtensa LX6 (store pipeline).
     */
    s32i    a6, a4, 0           /* [a4] ← 0 */
    addi    a4, a4, 4           /* a4 ← a4 + 4 (advance pointer one word) */

    /*
     * BLT: Branch if Less Than (unsigned).
     * Continue loop while current pointer < end pointer.
     */
    blt     a4, a5, .Lbss_loop  /* if a4 < a5: branch back to .Lbss_loop */

.Lbss_done:

    /* ── STEP 7: Copy .data section from LMA to VMA ─────────────────────
     *
     * Initialized global/static variables live in .data.
     * At reset, these are stored in flash (LMA = load memory address).
     * The CPU needs them in DRAM (VMA = virtual memory address) for R/W.
     *
     * The ROM BL copies our entire BL binary (code + data) to IRAM.
     * So the LMA of .data is somewhere in IRAM (after the code).
     * The VMA of .data is in DRAM (for writable access via data bus).
     * startup.S copies from _data_lma (IRAM source) to _data_start (DRAM dest).
     *
     * Register allocation:
     *   a4 = LMA source pointer  (IRAM, where ROM BL placed our binary)
     *   a5 = VMA dest pointer    (DRAM, where runtime .data must be)
     *   a6 = VMA end pointer     (_data_end)
     *   a7 = temp word for copy
     *
     * Reads via L32I (load 32-bit from IRAM): 3 cycles.
     * Writes via S32I (store 32-bit to DRAM): 3 cycles.
     */
    movi    a4, _data_lma       /* a4 ← LMA: where ROM BL left .data in IRAM */
    movi    a5, _data_start     /* a5 ← VMA: DRAM destination */
    movi    a6, _data_end       /* a6 ← VMA end (exclusive) */

    /* If .data is empty (start == end), skip copy loop */
    beq     a5, a6, .Ldata_done

.Ldata_loop:
    l32i    a7, a4, 0           /* a7 ← word from IRAM source */
    s32i    a7, a5, 0           /* [DRAM dest] ← a7 */
    addi    a4, a4, 4           /* advance source */
    addi    a5, a5, 4           /* advance dest */
    blt     a5, a6, .Ldata_loop /* if dest < end: continue */

.Ldata_done:

    /* ── STEP 8: Clear a0 to mark end of call stack for debuggers ───────
     *
     * Setting a0 = 0 signals to debuggers (like OpenOCD/GDB) that this is
     * the outermost stack frame. GDB's backtrace stops when it sees a0 = 0.
     * This prevents spurious "frame 1: ??? in ???" entries in backtraces.
     */
    movi    a0, 0               /* a0 ← 0 (no return address / top of call stack) */

    /* ── STEP 9: Call bootloader_main() ─────────────────────────────────
     *
     * CALL0: Jump-and-link without window rotation.
     *   • Stores PC+3 (return address) in a0
     *   • Jumps to target symbol
     *   • Does NOT rotate register windows (flat ABI)
     *
     * bootloader_main() uses ENTRY N (windowed ABI) internally.
     * ENTRY from a CALL0 context works because ENTRY reads PS.CALLINC
     * (which CALL0 does NOT set, leaving it 0). ENTRY with CALLINC=0 is
     * legal on Xtensa — it sets up a windowed frame but with zero increment,
     * effectively using window 0 as its frame. This is non-standard but
     * harmless for a single-level call from assembly.
     *
     * For correctness in deeply nested C calls, the bootloader C code uses
     * the windowed ABI (default for xtensa-esp32-elf-gcc without -mcall0).
     * So CALL0 here is just the outermost jump into C-land.
     *
     * bootloader_main is declared __attribute__((noreturn)) so we never
     * actually return to this call site.
     */
    call0   bootloader_main     /* Jump to C bootloader — NEVER RETURNS */

    /* ── SAFETY: Infinite loop in case of unexpected return ─────────────
     *
     * bootloader_main() should never return (it's noreturn).
     * If it somehow does (e.g., due to a compiler/linker bug), we hang.
     * The watchdog timer (if re-enabled) would eventually trigger a reset.
     */
.Lhang:
    j       .Lhang              /* Infinite busy loop — WDT will reset eventually */

    .size   _bootloader_start, . - _bootloader_start


/* ==========================================================================
 * BOOTLOADER EXCEPTION VECTOR TABLE
 *
 * The Xtensa LX6 requires exception vectors to be EXECUTABLE CODE at
 * fixed offsets from VECBASE. This is a fundamental difference from ARM:
 *
 *   ARM Cortex-M:  Vector table = array of ADDRESSES (pointers to handlers)
 *   Xtensa LX6:    Vector table = EXECUTABLE CODE at each exception offset
 *
 * VECBASE register constraints:
 *   • Must be 256-byte aligned (lower 8 bits must be 0)
 *   • The section must actually be at that address at runtime
 *
 * Exception vector offset table for Xtensa LX6 (ESP32):
 * ─────────────────────────────────────────────────────────────────────────
 *  Offset  Exception Type                     Our Handler
 *  ──────  ─────────────────────────────────  ─────────────────────────────
 *  +0x000  Level 1 interrupt (general IRQ)    Infinite loop (hang)
 *  +0x040  Kernel (software) exception        Infinite loop
 *  +0x180  Level 2 interrupt                  Infinite loop
 *  +0x1C0  Level 3 interrupt                  Infinite loop
 *  +0x200  Level 4 (cache error)              Infinite loop
 *  +0x240  Level 5 interrupt                  Infinite loop
 *  +0x280  Level 6 interrupt                  Infinite loop
 *  +0x2C0  Level 7 NMI                        Infinite loop
 *  +0x300  Debug exception                    Infinite loop
 *  +0x3C0  Double exception (fault in fault)  Infinite loop
 *  +0x400  Window overflow 4                  CALL0/RETURN sequence
 *  +0x480  Window underflow 4                 CALL0/RETURN sequence
 *  +0x4C0  Window overflow 8                  CALL0/RETURN sequence
 *  +0x540  Window underflow 8                 CALL0/RETURN sequence
 *  +0x580  Window overflow 12                 CALL0/RETURN sequence
 *  +0x600  Window underflow 12                CALL0/RETURN sequence
 * ─────────────────────────────────────────────────────────────────────────
 *
 * Bootloader policy: Any unexpected exception = unrecoverable bug.
 * All vectors are simple infinite loops. The watchdog timer (if active)
 * will detect the hang and trigger a hardware reset to recover.
 *
 * IMPORTANT: .align 256 is MANDATORY here.
 * The VECBASE hardware register enforces that its value has bits [7:0] = 0.
 * Writing a non-256-aligned address to VECBASE produces UNDEFINED BEHAVIOR.
 * ========================================================================== */

    .section    .iram.vectors, "ax"
    .align      256         /* 256-byte alignment: REQUIRED by VECBASE hardware */

    .global     _bl_vecbase
_bl_vecbase:

    /* Offset +0x000 — Level 1 interrupt / General Exception vector */
    .org    _bl_vecbase + 0x000
.Lvec_level1:
    j       .Lvec_level1    /* Spin: no L1 exception handler in bootloader */

    /* Offset +0x040 — Kernel (Level 0) Exception vector */
    .org    _bl_vecbase + 0x040
.Lvec_kernel:
    j       .Lvec_kernel

    /* Offset +0x180 — Level 2 interrupt vector */
    .org    _bl_vecbase + 0x180
.Lvec_level2:
    j       .Lvec_level2

    /* Offset +0x1C0 — Level 3 interrupt vector */
    .org    _bl_vecbase + 0x1C0
.Lvec_level3:
    j       .Lvec_level3

    /* Offset +0x200 — Level 4 (cache error) vector */
    .org    _bl_vecbase + 0x200
.Lvec_level4:
    j       .Lvec_level4

    /* Offset +0x240 — Level 5 interrupt vector */
    .org    _bl_vecbase + 0x240
.Lvec_level5:
    j       .Lvec_level5

    /* Offset +0x280 — Level 6 interrupt vector */
    .org    _bl_vecbase + 0x280
.Lvec_level6:
    j       .Lvec_level6

    /* Offset +0x2C0 — Level 7 NMI (Non-Maskable Interrupt) vector */
    .org    _bl_vecbase + 0x2C0
.Lvec_nmi:
    j       .Lvec_nmi

    /* Offset +0x300 — Debug Exception vector */
    .org    _bl_vecbase + 0x300
.Lvec_debug:
    j       .Lvec_debug

    /* Offset +0x3C0 — Double Exception vector (fault inside fault handler) */
    .org    _bl_vecbase + 0x3C0
.Lvec_double_exc:
    j       .Lvec_double_exc

    /* Offset +0x400 — Window Overflow 4 vector
     *
     * This vector is called when a CALL4 causes a register window overflow.
     * The correct handler rotates the window and saves registers to the
     * overflow stack area. Since we use windowed ABI in C code (the default
     * for xtensa-esp32-elf-gcc), we need a minimal overflow handler here.
     *
     * Minimal correct overflow-4 handler:
     *   MOVSP moves the stack contents if needed, then RFWO returns.
     * For maximum simplicity in a bootloader, we just hang — if overflow
     * occurs it means our call stack is unexpectedly deep (bug).
     */
    .org    _bl_vecbase + 0x400
.Lvec_wof4:
    j       .Lvec_wof4      /* Hang on window overflow — indicates stack bug */

    /* Offset +0x480 — Window Underflow 4 vector */
    .org    _bl_vecbase + 0x480
.Lvec_wuf4:
    j       .Lvec_wuf4

    /* Offset +0x4C0 — Window Overflow 8 vector */
    .org    _bl_vecbase + 0x4C0
.Lvec_wof8:
    j       .Lvec_wof8

    /* Offset +0x540 — Window Underflow 8 vector */
    .org    _bl_vecbase + 0x540
.Lvec_wuf8:
    j       .Lvec_wuf8

    /* Offset +0x580 — Window Overflow 12 vector */
    .org    _bl_vecbase + 0x580
.Lvec_wof12:
    j       .Lvec_wof12

    /* Offset +0x600 — Window Underflow 12 vector */
    .org    _bl_vecbase + 0x600
.Lvec_wuf12:
    j       .Lvec_wuf12
