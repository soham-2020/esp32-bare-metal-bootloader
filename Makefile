# ==============================================================================
# Makefile — ESP32 Custom Secondary Bootloader
# ==============================================================================
#
# Target:   ESP32-D0WD / ESP32-CAM (Tensilica Xtensa LX6 dual-core)
# Toolchain: xtensa-esp32-elf (Espressif crosstool-ng distribution)
#
# Toolchain installation:
#   Option A — via ESP-IDF:
#     git clone --recursive https://github.com/espressif/esp-idf.git
#     cd esp-idf && ./install.sh esp32
#     # Toolchain installs to ~/.espressif/tools/xtensa-esp32-elf/
#
#   Option B — standalone tarball (Linux/macOS):
#     wget https://github.com/espressif/crosstool-NG/releases/download/esp-12.2.0_20230208/xtensa-esp32-elf-12.2.0_20230208-x86_64-linux-gnu.tar.xz
#     tar xf xtensa-esp32-elf-*.tar.xz
#     export PATH=$PATH:$(pwd)/xtensa-esp32-elf/bin
#
#   Option C — Windows (via ESP-IDF installer):
#     https://dl.espressif.com/dl/esp-idf/?idf=5.2
#     Installs to C:\Users\<USER>\.espressif\tools\xtensa-esp32-elf\
#
# Flashing:
#   pip install esptool                 (Python 3 required)
#   make flash PORT=COM3                (Windows)
#   make flash PORT=/dev/ttyUSB0        (Linux)
#   make flash PORT=/dev/cu.usbserial-* (macOS)
# ==============================================================================

# ==============================================================================
# USER CONFIGURATION — Edit these paths for your environment
# ==============================================================================

# Toolchain binary prefix.
# Modern Espressif toolchain (v11+, released 2022+) uses the unified prefix:
#   xtensa-esp-elf-  (covers ESP32, ESP32-S2, ESP32-S3 etc. in one toolchain)
# Legacy toolchain releases (pre-2022) used:
#   xtensa-esp32-elf-  (ESP32 only)
# If the GCC binary is already in PATH, the prefix alone is sufficient.
# Otherwise, set to full path: C:\Espressif\tools\xtensa-esp-elf\...\bin\xtensa-esp-elf
TOOLCHAIN_PREFIX    ?= xtensa-esp-elf

# esptool command. Install with: pip install esptool
ESPTOOL             ?= python -m esptool

# Serial port for flashing (override on command line: make flash PORT=COMx)
PORT                ?= COM3

# Flash baud rate (921600 = maximum for most USB-UART bridges)
FLASH_BAUD          ?= 921600

# Flash mode: qio (quad-I/O, fastest), qout, dio, dout
FLASH_MODE          ?= qio

# Flash frequency in MHz
FLASH_FREQ_MHZ      ?= 40m

# Flash size string for esptool
FLASH_SIZE          ?= 4MB

# ==============================================================================
# DIRECTORY STRUCTURE
# ==============================================================================

SRC_DIR     := src
INC_DIR     := include
BUILD_DIR   := build

# ==============================================================================
# TOOLCHAIN BINARIES
# ==============================================================================

CC          := $(TOOLCHAIN_PREFIX)-gcc
AS          := $(TOOLCHAIN_PREFIX)-gcc       # Use GCC as assembler (handles .S + CPP)
LD          := $(TOOLCHAIN_PREFIX)-gcc       # Use GCC as linker front-end
OBJCOPY     := $(TOOLCHAIN_PREFIX)-objcopy
OBJDUMP     := $(TOOLCHAIN_PREFIX)-objdump
NM          := $(TOOLCHAIN_PREFIX)-nm
SIZE        := $(TOOLCHAIN_PREFIX)-size
READELF     := $(TOOLCHAIN_PREFIX)-readelf
GDB         := $(TOOLCHAIN_PREFIX)-gdb

# ==============================================================================
# SOURCE FILES
# ==============================================================================

# C source files
C_SRCS      := $(SRC_DIR)/main_bootloader.c

# Assembly source files (.S = uppercase = preprocessed by C preprocessor)
ASM_SRCS    := $(SRC_DIR)/startup.S

# Derive object file names (all in BUILD_DIR)
C_OBJS      := $(patsubst $(SRC_DIR)/%.c,   $(BUILD_DIR)/%.o, $(C_SRCS))
ASM_OBJS    := $(patsubst $(SRC_DIR)/%.S,   $(BUILD_DIR)/%.o, $(ASM_SRCS))
ALL_OBJS    := $(ASM_OBJS) $(C_OBJS)

# Output files
ELF         := $(BUILD_DIR)/bootloader.elf
BIN         := $(BUILD_DIR)/bootloader.bin
MAP         := $(BUILD_DIR)/bootloader.map
DIS         := $(BUILD_DIR)/bootloader.dis
NM_OUT      := $(BUILD_DIR)/bootloader.nm

# ==============================================================================
# COMPILER FLAGS — Architecture
#
# These flags are required for all Xtensa LX6 code generation.
# ==============================================================================

ARCH_FLAGS := \
    -mlongcalls                     \
    -mtext-section-literals

# -mlongcalls:
#   The Xtensa CALL instruction has a limited range: ±512 MB from the call site.
#   In our bootloader, code in IRAM (0x40080000) might need to call functions
#   whose addresses are computed at runtime (e.g., function pointers). Without
#   -mlongcalls, the linker might fail with "out of range" errors for calls
#   that span large address differences. With -mlongcalls, GCC emits an L32R
#   (literal load) + CALLX instruction pair, which can reach any 32-bit address.
#   Performance cost: 2 extra cycles per call (tiny for a bootloader).
#
# -mtext-section-literals:
#   On Xtensa, the L32R instruction loads 32-bit constants from a "literal pool"
#   (a nearby table of constants in the instruction stream). Without this flag,
#   all literal pools are grouped at the end of the .text section. With this flag,
#   each function's literals are placed immediately BEFORE the function in the
#   same .text subsection. This improves cache locality and avoids L32R range
#   errors (L32R can only reach ±256 KB). Required for IRAM-resident code.

# ==============================================================================
# COMPILER FLAGS — C language and optimization
# ==============================================================================

C_STD       := -std=c11

# Optimization: -Os (optimize for size).
# Alternatives: -O0 (debug, no optimization), -O2 (speed, larger code)
# For a size-constrained 28KB bootloader, -Os is the right choice.
OPT_FLAGS   := \
    -Os                             \
    -ffunction-sections             \
    -fdata-sections

# -Os:
#   Optimize for code size. Enables all -O2 optimizations that don't
#   increase code size, plus additional size-reducing transforms like
#   function outlining and common subexpression elimination. Critical for
#   fitting within our 28 KB flash budget.
#
# -ffunction-sections:
#   Place each function in its own linker section (.text.<funcname>).
#   Enables the linker's --gc-sections to dead-strip unused functions.
#   Without this: if any symbol in a .o file is used, ALL symbols in that
#   file are included in the output (section granularity).
#   With this: only the specific used functions are included.
#
# -fdata-sections:
#   Same as -ffunction-sections but for data (each global/static gets its own
#   section). Allows unused globals to be stripped by --gc-sections.

# Warning flags — production-grade diagnostics
WARN_FLAGS  := \
    -Wall                           \
    -Wextra                         \
    -Wpedantic                      \
    -Werror                         \
    -Wno-unused-parameter           \
    -Wno-unused-function            \
    -Wshadow                        \
    -Wdouble-promotion              \
    -Wformat=2                      \
    -Wformat-truncation

# Freestanding / bare-metal flags
BARE_FLAGS  := \
    -ffreestanding                  \
    -fno-builtin                    \
    -fno-exceptions                 \
    -fno-unwind-tables              \
    -fno-asynchronous-unwind-tables \
    -fno-common                     \
    -fno-stack-protector            \
    -nostdlib                       \
    -nostdinc

# -ffreestanding:
#   Declares that the code runs without a hosted C runtime. The compiler will
#   not assume that standard library functions (printf, malloc, memcpy, etc.)
#   are available. It won't automatically call __stack_chk_fail or similar.
#   REQUIRED for any embedded bare-metal code.
#
# -fno-builtin:
#   Prevents GCC from replacing function calls with optimized built-in versions.
#   For example, memset() on a struct might be replaced with a compiler built-in
#   that expects libc initialization. In bare-metal, this can cause linker errors
#   or incorrect behavior. Always use in freestanding environments.
#
# -fno-exceptions:
#   Disable C++ exception handling. Generates no exception unwind tables.
#   We're in C, but this prevents any inadvertent C++ exception metadata from
#   being emitted (e.g., from mixed C/C++ projects or compiler intrinsics).
#
# -fno-unwind-tables / -fno-asynchronous-unwind-tables:
#   Suppress generation of .eh_frame and .eh_frame_hdr sections.
#   These sections contain C++ exception unwind information.
#   In a C-only bare-metal bootloader, these are pure waste — they add hundreds
#   of bytes to the binary without providing any functionality.
#   Without these flags: GCC emits .eh_frame by default, linker includes it.
#
# -fno-common:
#   Don't merge tentative symbol definitions (global variables without explicit
#   initializer) into a .common section. Force them to .bss instead.
#   Prevents multiple .o files from "sharing" globals accidentally.
#
# -fno-stack-protector:
#   Don't add stack canary code (__stack_chk_fail). In a freestanding env,
#   __stack_chk_guard is not available, and the guard code would call a missing
#   function, causing a link error.
#
# -nostdlib:
#   Don't link against the standard C library (libc, libm, etc.) or GCC
#   support libraries (libgcc partially). We provide our own startup, no main.
#
# -nostdinc:
#   Don't search the compiler's default include directories for header files.
#   We only use our own headers in -I$(INC_DIR). Prevents accidentally including
#   hosted system headers (stdio.h etc.) which would not work in bare-metal.

# Include paths
INCLUDES    := -I$(INC_DIR)

# Preprocessor defines
DEFINES     :=                          \
    -DESP32=1                           \
    -DXTENSA_LX6=1                      \
    -DBL_VERSION_MAJOR=1                \
    -DBL_VERSION_MINOR=0

# Combined C compiler flags
CFLAGS      := $(ARCH_FLAGS) $(C_STD) $(OPT_FLAGS) $(WARN_FLAGS) \
               $(BARE_FLAGS) $(INCLUDES) $(DEFINES)

# ==============================================================================
# ASSEMBLER FLAGS
#
# We use GCC as the assembler front-end for .S files:
#   • GCC runs the C preprocessor on .S files (handles #include, #define)
#   • Then passes preprocessed output to the assembler (xtensa-esp32-elf-as)
# This allows sharing register definitions from esp32_regs.h in assembly.
# ==============================================================================

ASFLAGS     :=                          \
    $(ARCH_FLAGS)                       \
    -x assembler-with-cpp               \
    $(INCLUDES)                         \
    $(DEFINES)

# -x assembler-with-cpp:
#   Explicitly tell GCC to treat the input as assembly-with-preprocessing.
#   Without this, GCC would need to infer from the .S extension (which it does
#   for .S but not for .s). Being explicit is more portable.

# ==============================================================================
# LINKER FLAGS
# ==============================================================================

LDFLAGS     :=                          \
    -T linker.ld                        \
    -Wl,-Map=$(MAP)                     \
    -Wl,--gc-sections                   \
    -Wl,--no-undefined                  \
    -Wl,--fatal-warnings                \
    -Wl,-print-memory-usage             \
    -nostdlib                           \
    -ffreestanding

# -T linker.ld:
#   Use our custom linker script. Provides memory regions (MEMORY block)
#   and section layout (SECTIONS block) for the ESP32 address space.
#
# -Wl,-Map=$(MAP):
#   Generate a linker map file (bootloader.map). This file shows:
#     • Every section and its final VMA/LMA and size
#     • Every symbol and its address
#     • What .o file contributed each piece of code
#   ESSENTIAL for debugging linker issues and verifying IRAM placement.
#
# -Wl,--gc-sections:
#   Dead-strip unreferenced sections. Works in tandem with -ffunction-sections
#   and -fdata-sections in CFLAGS. Any function or variable not reachable from
#   the entry point is removed. Saves significant code space.
#   Note: KEEP() in linker.ld prevents GC from removing critical sections
#   (like .iram.vectors) that might not be explicitly referenced in C.
#
# -Wl,--no-undefined:
#   Fail the link if any symbol is referenced but not defined.
#   Without this: the linker produces a broken binary with unresolved symbols.
#   With this: link fails loudly with "undefined reference to 'X'" — much better.
#
# -Wl,--fatal-warnings:
#   Treat all linker warnings as errors. Catches subtle issues like:
#     • Sections overflowing their memory regions
#     • Alignment gaps
#     • Multiply-defined symbols
#
# -Wl,-print-memory-usage:
#   Print a summary of memory usage per region after linking.
#   Shows how many bytes of iram0, dram0 are used vs. available.
#   Very useful for ensuring we're within the 28 KB budget.
#
# -nostdlib:
#   Pass -nostdlib to the linker (tells it not to automatically link libgcc,
#   libc, crt0.o, etc.). We provide our own startup (startup.S) and don't
#   use any standard library functions.

# ==============================================================================
# PHONY TARGETS (not file targets — always executed when named)
# ==============================================================================

.PHONY: all clean flash disasm symbols size readelf check help

# ==============================================================================
# PRIMARY TARGETS
# ==============================================================================

##@ Build

## all: Build the bootloader ELF and raw binary (default target)
all: $(BIN)
	@echo ""
	@echo "+===================================================+"
	@echo "|  BUILD COMPLETE                                    |"
	@echo "|  ELF: $(ELF)"
	@echo "|  BIN: $(BIN)"
	@echo "+===================================================+"
	@$(MAKE) --no-print-directory size

## ── Link the ELF ──────────────────────────────────────────────────────────
# Dependencies: all object files + the linker script (relink if LD changes)
$(ELF): $(ALL_OBJS) linker.ld | $(BUILD_DIR)
	@echo ""
	@echo "[LD]  Linking $@"
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@
	@echo "[LD]  Done. Map: $(MAP)"

## ── Generate raw binary from ELF ──────────────────────────────────────────
# We use objcopy to extract only the loadable sections in binary format.
# The ROM bootloader expects a raw binary starting at the entry point.
# --only-section filters to just the sections we care about.
$(BIN): $(ELF)
	@echo "[BIN] Generating raw binary $@"
	$(OBJCOPY) \
	    --only-section=.iram.vectors   \
	    --only-section=.iram.text      \
	    --only-section=.iram.rodata    \
	    --only-section=.dram.data      \
	    -O binary                       \
	    $< $@
	@echo "[BIN] Binary size: $$(wc -c < $@) bytes (budget: 28672 bytes)"
	@if [ $$(wc -c < $@) -gt 28672 ]; then \
	    echo "[BIN] ERROR: Binary exceeds 28 KB budget!"; exit 1; \
	fi

## ── Compile C source files ────────────────────────────────────────────────
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "[CC]  $< → $@"
	$(CC) $(CFLAGS) -c $< -o $@

## ── Assemble .S source files ──────────────────────────────────────────────
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	@echo "[AS]  $< → $@"
	$(AS) $(ASFLAGS) -c $< -o $@

## ── Create build directory ────────────────────────────────────────────────
$(BUILD_DIR):
	@mkdir -p $@

##@ Analysis

## size: Show section sizes and memory usage summary
size: $(ELF)
	@echo ""
	@echo "=== Section Sizes (Berkeley format) ==="
	$(SIZE) --format=berkeley $<
	@echo ""
	@echo "=== Section Sizes (SysV format) ==="
	$(SIZE) --format=sysv $<

## disasm: Generate annotated disassembly listing
disasm: $(DIS)

$(DIS): $(ELF)
	@echo "[DISASM] Generating disassembly → $@"
	$(OBJDUMP)              \
	    --disassemble        \
	    --source             \
	    --all-headers        \
	    --demangle           \
	    --no-show-raw-insn   \
	    --visualize-jumps    \
	    $< > $@
	@echo "[DISASM] Written to $@"
	@echo "[DISASM] Hint: grep for 'bl_jump_to_app\|jx\|wsr.*vecbase' to review handoff code"

## symbols: Show all symbols sorted by address
symbols: $(ELF)
	@echo "[NM] Symbol listing → $(NM_OUT)"
	$(NM) --numeric-sort --print-size $< > $(NM_OUT)
	@echo "[NM] Top 20 largest symbols:"
	@sort -k2 -rn $(NM_OUT) | head -20

## readelf: Show ELF headers, sections, and segments
readelf: $(ELF)
	$(READELF) --all $<

## check: Verify binary is within budget and alignment constraints
check: $(BIN) $(ELF)
	@echo "=== Bootloader Verification Checks ==="
	@BIN_SIZE=$$(wc -c < $(BIN)); \
	echo "  Binary size:     $$BIN_SIZE bytes"; \
	echo "  Budget (28 KB):  28672 bytes"; \
	if [ $$BIN_SIZE -le 28672 ]; then \
	    echo "  Size check:      PASS"; \
	else \
	    echo "  Size check:      FAIL ($$(($$BIN_SIZE - 28672)) bytes over budget)"; \
	fi
	@echo "  VECBASE symbol:"
	@$(NM) $< | grep _bl_vecbase | awk '{print "    _bl_vecbase = 0x"$$1" (aligned: "((strtonum("0x"$$1) % 256 == 0) ? "YES" : "NO")")"}'
	@echo "  Entry point:"
	@$(NM) $< | grep _bootloader_start | awk '{print "    _bootloader_start = 0x"$$1}'

##@ Flashing

## flash: Flash the bootloader binary to ESP32 at offset 0x1000
flash: $(BIN)
	@echo "[FLASH] Flashing $(BIN) to offset 0x1000 on $(PORT) at $(FLASH_BAUD) baud"
	$(ESPTOOL)                          \
	    --chip      esp32               \
	    --port      $(PORT)             \
	    --baud      $(FLASH_BAUD)       \
	    --before     default_reset      \
	    --after      hard_reset         \
	    write_flash                     \
	        --flash_mode  $(FLASH_MODE) \
	        --flash_freq  $(FLASH_FREQ_MHZ) \
	        --flash_size  $(FLASH_SIZE) \
	        0x1000 $(BIN)
#
#   Explanation of esptool arguments:
#
#   --chip esp32:
#     Selects the ESP32 target chip. esptool supports esp32, esp32s2, esp32s3, etc.
#     This ensures correct protocol for the ROM bootloader handshake.
#
#   --port $(PORT):
#     USB-UART serial port. Adjust PORT variable for your OS:
#       Windows:  COM3, COM4, etc.
#       Linux:    /dev/ttyUSB0, /dev/ttyACM0
#       macOS:    /dev/cu.usbserial-XXXX
#
#   --baud $(FLASH_BAUD):
#     Serial communication speed. 921600 baud is near-maximum for most
#     CP2102/CH340 USB-UART bridges. If flashing fails, try 460800 or 115200.
#
#   --before default_reset:
#     Toggle DTR/RTS lines before connecting to automatically reset the ESP32
#     into ROM download mode. On most boards (DevKit, ESP32-CAM with programmer),
#     this hardware reset sequence works automatically.
#
#   --after hard_reset:
#     After successful flash, perform a hardware reset so the chip starts
#     executing the new bootloader immediately.
#
#   write_flash:
#     esptool subcommand to write binary data to flash.
#
#   --flash_mode qio:
#     QSPI Quad-I/O mode: uses all 4 data lines bidirectionally.
#     This is the fastest mode (supported by W25Q32JV on ESP32-CAM).
#     Use 'dio' if QIO causes CRC errors (some boards don't support it).
#
#   --flash_freq 40m:
#     SPI clock frequency: 40 MHz. ESP32-CAM default.
#     The ROM bootloader initializes flash at 40 MHz; higher requires config.
#
#   --flash_size 4MB:
#     Declare flash chip size. Must match actual chip (W25Q32JV = 4MB).
#     Used by esptool to validate address range of write operations.
#
#   0x1000 $(BIN):
#     Write the binary to physical flash offset 0x1000.
#     This is our secondary bootloader slot: FLASH_BOOTLOADER_OFFSET = 0x1000.
#     The ROM bootloader reads offset 0x0 for our header (a ROM-compatible
#     binary header that tells the ROM BL where to find our entry point).

## flash_full: Flash both the ROM-compatible header at 0x0 AND our BL at 0x1000
## (Advanced: requires a pre-built ROM header binary)
flash_full: $(BIN)
	@echo "[FLASH] Full flash: header @ 0x0, bootloader @ 0x1000"
	$(ESPTOOL)                          \
	    --chip      esp32               \
	    --port      $(PORT)             \
	    --baud      $(FLASH_BAUD)       \
	    --before     default_reset      \
	    --after      hard_reset         \
	    write_flash                     \
	        --flash_mode  $(FLASH_MODE) \
	        --flash_freq  $(FLASH_FREQ_MHZ) \
	        --flash_size  $(FLASH_SIZE) \
	        0x0000 $(BUILD_DIR)/rom_header.bin \
	        0x1000 $(BIN)

## monitor: Open serial terminal to watch UART output (115200 baud)
monitor:
	@echo "[MONITOR] Opening serial terminal on $(PORT) at 115200 baud"
	@echo "[MONITOR] Press Ctrl+] to exit"
	python -m serial.tools.miniterm \
	    --raw                        \
	    --rts 0                      \
	    --dtr 0                      \
	    $(PORT) 115200

##@ Utilities

## clean: Remove all build artifacts
clean:
	@echo "[CLEAN] Removing $(BUILD_DIR)/"
	@rm -rf $(BUILD_DIR)
	@echo "[CLEAN] Done"

## help: Show this help message
help:
	@echo "ESP32 Custom Secondary Bootloader — Makefile Targets"
	@echo ""
	@echo "Build:"
	@echo "  make all          Build ELF + binary (default)"
	@echo "  make clean        Remove build directory"
	@echo ""
	@echo "Analysis:"
	@echo "  make size         Show section sizes"
	@echo "  make disasm       Generate disassembly listing"
	@echo "  make symbols      Show symbols by address"
	@echo "  make readelf      Show ELF headers"
	@echo "  make check        Verify binary constraints"
	@echo ""
	@echo "Flashing:"
	@echo "  make flash        Flash BL to 0x1000 (set PORT=COMx)"
	@echo "  make flash_full   Flash header + BL"
	@echo "  make monitor      Open UART terminal"
	@echo ""
	@echo "Configuration variables (override on command line):"
	@echo "  PORT=$(PORT)"
	@echo "  FLASH_BAUD=$(FLASH_BAUD)"
	@echo "  FLASH_MODE=$(FLASH_MODE)"
	@echo "  TOOLCHAIN_PREFIX=$(TOOLCHAIN_PREFIX)"

# ==============================================================================
# DEPENDENCY TRACKING
#
# GCC can auto-generate .d dependency files that track which headers each .c
# file includes. Including these lets Make rebuild .o files when headers change.
# -MMD: generate .d file alongside .o
# -MP: add phony targets for headers (prevents errors when headers are deleted)
# ==============================================================================

DEPFLAGS    := -MMD -MP
CFLAGS      += $(DEPFLAGS)

# Include generated dependency files (silently ignore if they don't exist yet)
-include $(ALL_OBJS:.o=.d)
