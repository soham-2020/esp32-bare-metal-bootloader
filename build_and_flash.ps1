#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Build and Flash Script for ESP32 Custom Secondary Bootloader.
    Runs on Windows PowerShell 5.1+ or PowerShell Core 7+.

.DESCRIPTION
    This script:
      1. Verifies / installs the xtensa-esp32-elf toolchain
      2. Verifies / installs esptool
      3. Cleans the build directory
      4. Compiles startup.S and main_bootloader.c with xtensa-esp32-elf-gcc
      5. Links with linker.ld using xtensa-esp32-elf-gcc
      6. Extracts a raw binary using xtensa-esp32-elf-objcopy
      7. Detects the connected ESP32 serial port
      8. Erases the ESP32 flash
      9. Flashes bootloader.bin to physical address 0x1000

.USAGE
    .\build_and_flash.ps1                      # Auto-detect port
    .\build_and_flash.ps1 -Port COM4           # Specify port
    .\build_and_flash.ps1 -BuildOnly           # Compile only, skip flash
    .\build_and_flash.ps1 -FlashOnly -Port COM4 # Flash only (skip compile)
#>

param(
    [string]  $Port       = "",          # Serial port override (e.g., "COM4")
    [switch]  $BuildOnly  = $false,      # Build but don't flash
    [switch]  $FlashOnly  = $false,      # Flash only (skip build)
    [int]     $FlashBaud  = 921600,      # Flash baud rate
    [string]  $FlashMode  = "dio",       # Flash mode: qio, qout, dio, dout
    [string]  $FlashFreq  = "40m",       # Flash SPI frequency
    [string]  $FlashSize  = "4MB"        # Flash chip size
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Colour helpers ────────────────────────────────────────────────────────────
function Info  { param($m) Write-Host "[INFO]  $m" -ForegroundColor Cyan    }
function OK    { param($m) Write-Host "[ OK ]  $m" -ForegroundColor Green   }
function Warn  { param($m) Write-Host "[WARN]  $m" -ForegroundColor Yellow  }
function Err   { param($m) Write-Host "[ERR!]  $m" -ForegroundColor Red     }
function Step  { param($m) Write-Host "`n=== $m ===" -ForegroundColor Magenta }

# ── Paths ─────────────────────────────────────────────────────────────────────
$SCRIPT_DIR   = $PSScriptRoot
$SRC_DIR      = Join-Path $SCRIPT_DIR "src"
$INC_DIR      = Join-Path $SCRIPT_DIR "include"
$BUILD_DIR    = Join-Path $SCRIPT_DIR "build"
$LINKER_SCRIPT = Join-Path $SCRIPT_DIR "linker.ld"

$ELF_OUT      = Join-Path $BUILD_DIR "bootloader.elf"
$BIN_OUT      = Join-Path $BUILD_DIR "bootloader.bin"
$MAP_OUT      = Join-Path $BUILD_DIR "bootloader.map"

# Flash parameters
$FLASH_ADDR   = "0x1000"   # Secondary BL physical flash offset

# ── Toolchain discovery ───────────────────────────────────────────────────────

function Find-XtensaToolchain {
    <#
    Search strategy (in priority order):
      1. Already in PATH (e.g., ESP-IDF environment activated)
      2. Common ESP-IDF Windows installer paths
      3. MSYS2-based ESP-IDF installs
      4. Manual install in common locations
    #>

    # Try PATH first
    $inPath = Get-Command "xtensa-esp32-elf-gcc.exe" -ErrorAction SilentlyContinue
    if ($inPath) {
        return (Split-Path $inPath.Source)
    }

    # Common ESP-IDF Windows installer locations
    $candidates = @(
        "C:\Espressif\tools\xtensa-esp32-elf\esp-2022r1-11.2.0\xtensa-esp32-elf\bin",
        "C:\Espressif\tools\xtensa-esp32-elf\esp-12.2.0_20230208\xtensa-esp32-elf\bin",
        "C:\Espressif\tools\xtensa-esp32-elf\esp-13.2.0_20240530\xtensa-esp32-elf\bin",
        "C:\esp\xtensa-esp32-elf\bin",
        "C:\msys64\opt\xtensa-esp32-elf\bin",
        "$env:USERPROFILE\.espressif\tools\xtensa-esp32-elf",
        "C:\Users\$env:USERNAME\.espressif\tools\xtensa-esp32-elf"
    )

    foreach ($path in $candidates) {
        if (Test-Path "$path\xtensa-esp32-elf-gcc.exe") {
            return $path
        }
        # Also try subdirectories (version-numbered)
        if (Test-Path $path) {
            $sub = Get-ChildItem $path -Recurse -Filter "xtensa-esp32-elf-gcc.exe" `
                   -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($sub) { return (Split-Path $sub.FullName) }
        }
    }

    # Try Espressif IDF_TOOLS_PATH env var
    if ($env:IDF_TOOLS_PATH) {
        $sub = Get-ChildItem $env:IDF_TOOLS_PATH -Recurse `
               -Filter "xtensa-esp32-elf-gcc.exe" -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($sub) { return (Split-Path $sub.FullName) }
    }

    return $null
}

function Install-XtensaToolchain {
    <#
    Download the Espressif xtensa-esp32-elf toolchain for Windows.
    Uses the official GitHub release (no ESP-IDF required).

    Toolchain v12.2.0 (stable, works with ESP-IDF v5.x):
      ~180 MB download, extracts to ~600 MB
    #>
    $TOOLCHAIN_VERSION = "esp-12.2.0_20230208"
    $TOOLCHAIN_URL = "https://github.com/espressif/crosstool-NG/releases/download/$TOOLCHAIN_VERSION/xtensa-esp32-elf-$TOOLCHAIN_VERSION-x86_64-w64-mingw32.zip"
    $INSTALL_DIR = "C:\Espressif\tools\xtensa-esp32-elf\$TOOLCHAIN_VERSION"
    $ZIP_PATH    = "$env:TEMP\xtensa-esp32-elf.zip"

    Step "Installing xtensa-esp32-elf toolchain"
    Info "Download URL: $TOOLCHAIN_URL"
    Info "Install path: $INSTALL_DIR"
    Warn "This is a ~180 MB download. Please wait..."

    try {
        # Use .NET WebClient for progress display
        $wc = New-Object System.Net.WebClient
        $wc.DownloadFile($TOOLCHAIN_URL, $ZIP_PATH)
        OK "Download complete: $ZIP_PATH"
    } catch {
        Err "Download failed: $_"
        Err "Please manually download from:"
        Err "  https://github.com/espressif/crosstool-NG/releases"
        Err "  or install ESP-IDF from: https://dl.espressif.com/dl/esp-idf/"
        exit 1
    }

    Info "Extracting toolchain (this takes 1-2 minutes)..."
    if (!(Test-Path $INSTALL_DIR)) { New-Item -ItemType Directory -Path $INSTALL_DIR | Out-Null }
    Expand-Archive -Path $ZIP_PATH -DestinationPath $INSTALL_DIR -Force
    Remove-Item $ZIP_PATH -Force

    $BIN_DIR = "$INSTALL_DIR\xtensa-esp32-elf\bin"
    if (!(Test-Path "$BIN_DIR\xtensa-esp32-elf-gcc.exe")) {
        # Try one level deeper (some archives have extra nesting)
        $BIN_DIR = Get-ChildItem $INSTALL_DIR -Recurse -Filter "xtensa-esp32-elf-gcc.exe" `
                   -ErrorAction SilentlyContinue | Select-Object -First 1 |
                   ForEach-Object { Split-Path $_.FullName }
    }

    if ($BIN_DIR) {
        OK "Toolchain installed at: $BIN_DIR"
        # Add to current session PATH
        $env:PATH = "$BIN_DIR;$env:PATH"
        return $BIN_DIR
    } else {
        Err "Extraction succeeded but gcc not found. Check $INSTALL_DIR"
        exit 1
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# STEP 1: Resolve toolchain
# ──────────────────────────────────────────────────────────────────────────────

if (!$FlashOnly) {

    Step "Resolving xtensa-esp32-elf toolchain"

    $TOOLCHAIN_BIN = Find-XtensaToolchain

    if (!$TOOLCHAIN_BIN) {
        Warn "xtensa-esp32-elf toolchain not found on this system."
        $answer = Read-Host "Download and install it now? (~180 MB) [Y/n]"
        if ($answer -ne 'n' -and $answer -ne 'N') {
            $TOOLCHAIN_BIN = Install-XtensaToolchain
        } else {
            Err "Cannot build without the xtensa-esp32-elf toolchain."
            Err "Install ESP-IDF from: https://dl.espressif.com/dl/esp-idf/"
            exit 1
        }
    } else {
        OK "Toolchain found: $TOOLCHAIN_BIN"
    }

    # Add to PATH for this session
    if ($env:PATH -notlike "*$TOOLCHAIN_BIN*") {
        $env:PATH = "$TOOLCHAIN_BIN;$env:PATH"
    }

    # Define tool paths
    $GCC     = "$TOOLCHAIN_BIN\xtensa-esp32-elf-gcc.exe"
    $OBJCOPY = "$TOOLCHAIN_BIN\xtensa-esp32-elf-objcopy.exe"
    $SIZE    = "$TOOLCHAIN_BIN\xtensa-esp32-elf-size.exe"

    # Verify
    $GCC_VER = & $GCC --version 2>&1 | Select-Object -First 1
    OK "Compiler: $GCC_VER"
}

# ──────────────────────────────────────────────────────────────────────────────
# STEP 2: Verify esptool
# ──────────────────────────────────────────────────────────────────────────────

if (!$BuildOnly) {
    Step "Verifying esptool"
    try {
        $esptool_ver = python -m esptool version 2>&1 | Select-Object -First 1
        OK "esptool: $esptool_ver"
    } catch {
        Warn "esptool not found. Installing..."
        pip install esptool 2>&1 | Write-Host
        $esptool_ver = python -m esptool version 2>&1 | Select-Object -First 1
        OK "esptool installed: $esptool_ver"
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# STEP 3: Detect serial port (unless specified)
# ──────────────────────────────────────────────────────────────────────────────

if (!$BuildOnly) {
    Step "Detecting ESP32 serial port"

    if ($Port -eq "") {
        # Look for known ESP32 USB-UART bridge VIDs/PIDs
        $ESP32_VIDS = @("10C4", "1A86", "0403", "2341", "303A")  # CP2102, CH340/CH9102, FTDI, Arduino, Espressif

        $ports = Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue |
                 Where-Object {
                     $vid = $_.InstanceId -replace '.*VID_([0-9A-F]{4}).*','$1'
                     $ESP32_VIDS -contains $vid
                 }

        if ($ports) {
            # Extract COM port number from FriendlyName
            $Port = ($ports | Select-Object -First 1).FriendlyName -replace '.*\((COM\d+)\).*','$1'
            OK "Auto-detected port: $Port ($($ports[0].FriendlyName))"
        } else {
            # Fall back: list ALL available COM ports
            $allPorts = Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue
            if ($allPorts) {
                Warn "No known ESP32 USB-UART bridge found. Available ports:"
                $allPorts | ForEach-Object { Warn "  $($_.FriendlyName)" }
                $Port = Read-Host "Enter COM port manually (e.g., COM4)"
            } else {
                Err "No COM ports found. Is the ESP32 connected via USB?"
                exit 1
            }
        }
    } else {
        OK "Using specified port: $Port"
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# STEP 4: Clean build directory
# ──────────────────────────────────────────────────────────────────────────────

if (!$FlashOnly) {
    Step "Cleaning build directory"

    if (Test-Path $BUILD_DIR) {
        Remove-Item -Recurse -Force $BUILD_DIR
        OK "Removed: $BUILD_DIR"
    }
    New-Item -ItemType Directory -Path $BUILD_DIR | Out-Null
    OK "Created: $BUILD_DIR"
}

# ──────────────────────────────────────────────────────────────────────────────
# STEP 5: Compile and link
# ──────────────────────────────────────────────────────────────────────────────

if (!$FlashOnly) {

    # ── Architecture flags (Xtensa LX6 / ESP32) ──────────────────────────────
    $ARCH = @(
        "-mlongcalls",                    # Enable long call sequences (CALL + L32R) for full address space
        "-mtext-section-literals"         # Place literal pools adjacent to functions
    )

    # ── C compiler flags ──────────────────────────────────────────────────────
    $CFLAGS = @(
        "-std=c11",                       # C11 standard (for _Static_assert, stdbool, stdint)
        "-Os",                            # Optimize for size (28 KB budget)
        "-ffunction-sections",            # Each function in own section (enables --gc-sections)
        "-fdata-sections",                # Each data item in own section
        "-ffreestanding",                 # No hosted C runtime assumed
        "-fno-builtin",                   # Don't replace calls with GCC built-ins
        "-fno-exceptions",                # No C++ exceptions
        "-fno-unwind-tables",             # No .eh_frame unwind tables
        "-fno-asynchronous-unwind-tables",# No async unwind (saves ~2KB)
        "-fno-common",                    # No tentative definitions merged
        "-fno-stack-protector",           # No __stack_chk_fail (not available bare-metal)
        "-nostdlib",                      # Don't link standard libs
        "-nostdinc",                      # Don't search system include dirs
        "-Wall", "-Wextra",               # Enable warnings
        "-Werror",                        # Treat warnings as errors
        "-Wno-unused-parameter",          # Allow unused function parameters
        "-Wno-unused-function",           # Allow unused static functions
        "-I$INC_DIR",                     # Our header files
        "-DESP32=1",                      # Preprocessor define
        "-DXTENSA_LX6=1"
    ) + $ARCH

    # ── Assembler flags ───────────────────────────────────────────────────────
    $ASFLAGS = @(
        "-x", "assembler-with-cpp",       # Run C preprocessor on .S files
        "-I$INC_DIR"
    ) + $ARCH

    # ── Linker flags ──────────────────────────────────────────────────────────
    $LDFLAGS = @(
        "-T", $LINKER_SCRIPT,             # Custom linker script
        "-Wl,-Map=$MAP_OUT",              # Generate map file
        "-Wl,--gc-sections",              # Dead-strip unused sections
        "-Wl,--no-undefined",             # Error on unresolved symbols
        "-Wl,--fatal-warnings",           # Warnings = errors in linker
        "-nostdlib",                      # No standard libs
        "-ffreestanding"
    )

    # ── Compile startup.S ─────────────────────────────────────────────────────
    Step "Compiling startup.S"
    $ASM_OBJ = Join-Path $BUILD_DIR "startup.o"
    $ASM_SRC = Join-Path $SRC_DIR "startup.S"

    $cmd = @($GCC) + $ASFLAGS + @("-c", $ASM_SRC, "-o", $ASM_OBJ)
    Info "CMD: $($cmd -join ' ')"
    & $GCC @ASFLAGS -c $ASM_SRC -o $ASM_OBJ
    if ($LASTEXITCODE -ne 0) { Err "startup.S compilation FAILED (exit $LASTEXITCODE)"; exit 1 }
    OK "startup.o compiled ($('{0:N0}' -f (Get-Item $ASM_OBJ).Length) bytes)"

    # ── Compile main_bootloader.c ─────────────────────────────────────────────
    Step "Compiling main_bootloader.c"
    $C_OBJ = Join-Path $BUILD_DIR "main_bootloader.o"
    $C_SRC = Join-Path $SRC_DIR "main_bootloader.c"

    & $GCC @CFLAGS -c $C_SRC -o $C_OBJ
    if ($LASTEXITCODE -ne 0) { Err "main_bootloader.c compilation FAILED (exit $LASTEXITCODE)"; exit 1 }
    OK "main_bootloader.o compiled ($('{0:N0}' -f (Get-Item $C_OBJ).Length) bytes)"

    # ── Link ELF ──────────────────────────────────────────────────────────────
    Step "Linking bootloader.elf"

    & $GCC @LDFLAGS $ASM_OBJ $C_OBJ -o $ELF_OUT
    if ($LASTEXITCODE -ne 0) { Err "Linking FAILED (exit $LASTEXITCODE)"; exit 1 }
    OK "ELF linked: $ELF_OUT"

    # ── Extract raw binary ────────────────────────────────────────────────────
    Step "Generating bootloader.bin (raw binary)"

    & $OBJCOPY `
        --only-section=.iram.vectors `
        --only-section=.iram.text    `
        --only-section=.iram.rodata  `
        --only-section=.dram.data    `
        -O binary                     `
        $ELF_OUT $BIN_OUT

    if ($LASTEXITCODE -ne 0) { Err "objcopy FAILED (exit $LASTEXITCODE)"; exit 1 }

    $BIN_SIZE = (Get-Item $BIN_OUT).Length
    OK "Binary created: $BIN_OUT"
    OK "Binary size:    $BIN_SIZE bytes (budget: 28672 bytes)"

    if ($BIN_SIZE -gt 28672) {
        Err "BINARY EXCEEDS 28 KB BUDGET by $($BIN_SIZE - 28672) bytes!"
        Err "Reduce code size or adjust linker.ld FLASH_BOOTLOADER_SIZE."
        exit 1
    }

    # ── Section size report ───────────────────────────────────────────────────
    Step "Section size report"
    & $SIZE --format=berkeley $ELF_OUT
    Write-Host ""
}

# ──────────────────────────────────────────────────────────────────────────────
# STEP 6: Verify binary exists before flashing
# ──────────────────────────────────────────────────────────────────────────────

if (!$BuildOnly) {
    Step "Verifying binary"

    if (!(Test-Path $BIN_OUT)) {
        Err "bootloader.bin not found at: $BIN_OUT"
        Err "Run without -FlashOnly to build first."
        exit 1
    }

    $BIN_SIZE = (Get-Item $BIN_OUT).Length
    OK "bootloader.bin exists: $BIN_SIZE bytes"

    if ($BIN_SIZE -eq 0) {
        Err "bootloader.bin is empty! Build may have failed."
        exit 1
    }

    # ── Print manual reset instructions ──────────────────────────────────────
    Write-Host ""
    Write-Host "┌─────────────────────────────────────────────────────────┐" -ForegroundColor Yellow
    Write-Host "│  IMPORTANT: Put ESP32-CAM into DOWNLOAD MODE first!     │" -ForegroundColor Yellow
    Write-Host "│                                                          │" -ForegroundColor Yellow
    Write-Host "│  Method A (if no auto-reset circuit):                   │" -ForegroundColor Yellow
    Write-Host "│    1. Hold the BOOT button (GPIO 0) on the board        │" -ForegroundColor Yellow
    Write-Host "│    2. Press and release the RESET (EN) button           │" -ForegroundColor Yellow
    Write-Host "│    3. Release the BOOT button                           │" -ForegroundColor Yellow
    Write-Host "│    4. Board is now in ROM download mode                 │" -ForegroundColor Yellow
    Write-Host "│                                                          │" -ForegroundColor Yellow
    Write-Host "│  Method B (ESP32-CAM with programmer board):            │" -ForegroundColor Yellow
    Write-Host "│    Connect IO0 to GND before powering on                │" -ForegroundColor Yellow
    Write-Host "│    esptool auto-reset (DTR/RTS) should handle this      │" -ForegroundColor Yellow
    Write-Host "│                                                          │" -ForegroundColor Yellow
    Write-Host "│  The board is in download mode when the blue LED        │" -ForegroundColor Yellow
    Write-Host "│  dims or the UART shows no output (silent = ready).     │" -ForegroundColor Yellow
    Write-Host "└─────────────────────────────────────────────────────────┘" -ForegroundColor Yellow
    Write-Host ""

    $ready = Read-Host "Press ENTER when board is in download mode (or Ctrl+C to cancel)"

    # ── STEP 7: Erase ESP32 flash ─────────────────────────────────────────────
    Step "Erasing ESP32 flash (full chip erase)"
    Info "Port: $Port | Baud: $FlashBaud"

    python -m esptool              `
        --chip esp32               `
        --port $Port               `
        --baud $FlashBaud          `
        --before default_reset     `
        --after no_reset           `
        erase_flash

    if ($LASTEXITCODE -ne 0) {
        Warn "Chip erase with default_reset failed. Trying without auto-reset..."
        python -m esptool              `
            --chip esp32               `
            --port $Port               `
            --baud 115200              `
            --before no-reset          `
            --after no_reset           `
            erase_flash

        if ($LASTEXITCODE -ne 0) {
            Err "Flash erase FAILED."
            Err "Troubleshooting:"
            Err "  1. Verify the board is in download mode (BOOT+RESET sequence)"
            Err "  2. Try a lower baud rate: -FlashBaud 115200"
            Err "  3. Check USB cable (use data cable, not charge-only)"
            Err "  4. Check driver: CP2102/CH9102 requires VCP driver from silicon labs"
            exit 1
        }
    }
    OK "Flash erased successfully"

    # ── STEP 8: Flash bootloader.bin to 0x1000 ───────────────────────────────
    Step "Flashing bootloader.bin to address 0x1000"
    Info "Binary: $BIN_OUT"
    Info "Target: $Port @ $FlashBaud baud → flash:0x1000"

    python -m esptool              `
        --chip esp32               `
        --port $Port               `
        --baud $FlashBaud          `
        --before default_reset     `
        --after hard_reset         `
        write_flash                `
            --flash_mode  $FlashMode  `
            --flash_freq  $FlashFreq  `
            --flash_size  $FlashSize  `
            $FLASH_ADDR $BIN_OUT

    if ($LASTEXITCODE -ne 0) {
        Warn "Flash with QIO mode failed. Retrying with DIO mode..."
        python -m esptool              `
            --chip esp32               `
            --port $Port               `
            --baud 460800              `
            --before default_reset     `
            --after hard_reset         `
            write_flash                `
                --flash_mode dio       `
                --flash_freq 40m       `
                --flash_size 4MB       `
                $FLASH_ADDR $BIN_OUT

        if ($LASTEXITCODE -ne 0) {
            Err "Flash FAILED. See troubleshooting output above."
            exit 1
        }
    }

    Write-Host ""
    Write-Host "╔══════════════════════════════════════════════════════╗" -ForegroundColor Green
    Write-Host "║   FLASH COMPLETE                                     ║" -ForegroundColor Green
    Write-Host "║   bootloader.bin → ESP32 flash:0x1000               ║" -ForegroundColor Green
    Write-Host "║   Board has been reset. Bootloader is now running.  ║" -ForegroundColor Green
    Write-Host "╚══════════════════════════════════════════════════════╝" -ForegroundColor Green
    Write-Host ""
    Write-Host "Monitor UART output at 115200 baud:" -ForegroundColor Cyan
    Write-Host "  python -m serial.tools.miniterm $Port 115200 --raw" -ForegroundColor Cyan
}
