/**
 * @file    flash_map.h
 * @brief   SPI Flash physical memory map and image format definitions.
 *
 * Physical Flash Layout — 4 MB (W25Q32JV or equivalent, on ESP32-CAM)
 * ─────────────────────────────────────────────────────────────────────────
 *
 *  Physical     Size      Region              Description
 *  ─────────── ─────────  ──────────────────  ─────────────────────────────
 *  0x000000     4 KB      ROM BL Header       Espressif image descriptor read
 *                                             by mask ROM (magic 0xE9, etc.)
 *  0x001000    28 KB      BOOTLOADER CODE     ★ Our secondary bootloader
 *  0x008000     4 KB      PARTITION TABLE     Custom slot descriptor table
 *  0x009000    28 KB      Reserved            Padding / future NVS config
 *  0x010000    64 KB      Reserved            Alignment gap to App boundary
 *  0x010000   ~1 MB      APP SLOT 1 PRIMARY  Main application firmware
 *  0x110000   ~1 MB      APP SLOT 2 OTA/IAP  Firmware update payload
 *  0x3E0000    64 KB      NVS Storage         Key-value persistent store
 *  0x3F0000    64 KB      Factory / PHY data  Calibration, serial#, MAC
 * ─────────────────────────────────────────────────────────────────────────
 *
 * Flash hardware characteristics (W25Q32JV):
 *   Total size:   4,194,304 bytes (4 MB = 0x400000)
 *   Sector size:  4,096 bytes (4 KB) — minimum erasable unit
 *   Page size:    256 bytes — maximum programmable unit per operation
 *   Bus width:    4-bit QSPI at up to 80 MHz (ESP32-CAM default: 40 MHz)
 *
 * LMA/VMA address relationship:
 *   Physical flash offset 0x1000 maps to virtual address 0x40081000 via
 *   the SPI0 cache MMU when the flash XIP window is 0x400C0000 base.
 *   The ROM bootloader copies our BL binary from flash offset 0x1000 to
 *   IRAM at virtual address 0x40080000 before jumping to our entry point.
 */

#ifndef FLASH_MAP_H
#define FLASH_MAP_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * PHYSICAL FLASH OFFSETS (byte addresses from flash chip pin 1)
 * These are the raw 24-bit addresses sent over SPI to the flash chip.
 * ========================================================================= */

/** Espressif ROM BL image descriptor header (4 KB) */
#define FLASH_ROM_HDR_OFFSET        0x000000UL
#define FLASH_ROM_HDR_SIZE          0x001000UL   /* 4 KB */

/** Secondary bootloader binary (28 KB, sectors 1–7) */
#define FLASH_BOOTLOADER_OFFSET     0x001000UL
#define FLASH_BOOTLOADER_SIZE       0x007000UL   /* 28 KB */

/** Custom partition table (4 KB, sector 8) */
#define FLASH_PARTITION_TBL_OFFSET  0x008000UL
#define FLASH_PARTITION_TBL_SIZE    0x001000UL   /* 4 KB */

/** Reserved (36 KB) — padding to align App Slot 1 at 64 KB boundary */
#define FLASH_RESERVED_OFFSET       0x009000UL
#define FLASH_RESERVED_SIZE         0x007000UL   /* 28 KB */

/** Application Slot 1 — Primary firmware (start aligned to 64 KB boundary) */
#define FLASH_APP_SLOT1_OFFSET      0x010000UL
#define FLASH_APP_SLOT1_SIZE        0x100000UL   /* 1 MB */
#define FLASH_APP_SLOT1_END         (FLASH_APP_SLOT1_OFFSET + FLASH_APP_SLOT1_SIZE)

/** Application Slot 2 — OTA / IAP update payload */
#define FLASH_APP_SLOT2_OFFSET      0x110000UL
#define FLASH_APP_SLOT2_SIZE        0x100000UL   /* 1 MB */
#define FLASH_APP_SLOT2_END         (FLASH_APP_SLOT2_OFFSET + FLASH_APP_SLOT2_SIZE)

/** Non-volatile storage (64 KB) */
#define FLASH_NVS_OFFSET            0x3E0000UL
#define FLASH_NVS_SIZE              0x010000UL   /* 64 KB */

/** Factory calibration data (64 KB) */
#define FLASH_FACTORY_OFFSET        0x3F0000UL
#define FLASH_FACTORY_SIZE          0x010000UL   /* 64 KB */

/** Total SPI flash size */
#define FLASH_TOTAL_SIZE            0x400000UL   /* 4 MB */

/* =========================================================================
 * FLASH HARDWARE GEOMETRY
 * ========================================================================= */
#define FLASH_SECTOR_SIZE           0x1000UL   /**< 4 KB — minimum erase unit */
#define FLASH_PAGE_SIZE             0x100UL    /**< 256 B — minimum write unit */

/* =========================================================================
 * APPLICATION IMAGE FORMAT
 *
 * Layout of bytes at the start of each Application Slot:
 *
 * Offset   Size  Field            Description
 * ───────  ────  ───────────────  ────────────────────────────────────────
 *  +0x00    1B   magic            MUST be 0xE9 (matches ESP-IDF convention)
 *  +0x01    1B   segment_count    Number of loadable ELF-like segments
 *  +0x02    1B   spi_mode         Flash SPI mode (0=QIO,1=QOUT,2=DIO,3=DOUT)
 *  +0x03    1B   spi_speed_size   [7:4]=speed, [3:0]=flash size code
 *  +0x04    4B   entry_addr       Virtual address of reset handler function
 *  +0x08    1B   wp_pin           Write-protect GPIO pin (0xFF = unused)
 *  +0x09    3B   spi_pin_drv      SPI pin drive strength (3 bytes)
 *  +0x0C    2B   chip_id          Chip target ID (0x0005 = ESP32)
 *  +0x0E    1B   min_chip_rev     Minimum chip revision required
 *  +0x0F    1B   min_chip_rev_full Minimum chip revision (extended)
 *  +0x10    2B   max_chip_rev     Maximum supported chip revision
 *  +0x12    6B   reserved         Must be 0x000000000000
 *  +0x18    4B   payload_length   Total bytes of all loadable segments
 *  +0x1C    4B   crc32            CRC32 over bytes [0x00..0x1B]
 *  +0x20   32B   sha256[32]       SHA-256 hash over ALL loadable segment data
 * ───────────────────────────────────────────────────────────────────────
 * Total header: 80 bytes (0x50)
 *
 * Note: The 0xE9 magic byte is the Espressif standard. The ROM bootloader
 * reads offset 0x0 of the secondary BL flash region and checks for 0xE9.
 * Our secondary BL similarly checks each App Slot for 0xE9 before jumping.
 * ========================================================================= */

/** Magic byte at offset 0 of every valid ESP32 image */
#define APP_IMAGE_MAGIC             0xE9U

/** Extended magic: 4-byte word including the magic and padding */
#define APP_IMAGE_MAGIC_WORD        0x5AA5C3E9UL

/** Chip ID for ESP32 targets */
#define APP_CHIP_ID_ESP32           0x0005U

/**
 * Image header size in bytes.
 * Byte count of the packed struct below:
 *   1+1+1+1 + 4 + 1+3 + 2+1+1+2 + 6 + 4 + 4 + 32 = 64 bytes
 */
#define APP_IMAGE_HDR_SIZE          64U   /* 0x40 bytes */

/**
 * @brief Application image header structure.
 *
 * Placed at byte offset 0 within every application slot in flash.
 * The bootloader reads this structure and validates:
 *   1. magic == 0xE9
 *   2. crc32 over bytes [0..27] matches stored crc32
 *   3. sha256 over all segment data matches stored sha256
 *
 * packed attribute: ensures zero padding bytes between fields so the
 * struct maps exactly to the on-flash byte layout.
 */
typedef struct __attribute__((packed)) {
    uint8_t     magic;            /**< Image magic: must be 0xE9 */
    uint8_t     segment_count;    /**< Number of loadable binary segments */
    uint8_t     spi_mode;         /**< Flash SPI mode (0=QIO, 2=DIO, 3=DOUT) */
    uint8_t     spi_speed_size;   /**< [7:4]=SPI speed code, [3:0]=flash size */
    uint32_t    entry_addr;       /**< Reset handler virtual address (app entry) */
    uint8_t     wp_pin;           /**< Write-protect pin (0xFF = not used) */
    uint8_t     spi_pin_drv[3];   /**< SPI pin drive strength per pin */
    uint16_t    chip_id;          /**< Target chip ID (0x0005 = ESP32) */
    uint8_t     min_chip_rev;     /**< Minimum chip revision */
    uint8_t     min_chip_rev_full;/**< Minimum chip revision (extended form) */
    uint16_t    max_chip_rev;     /**< Maximum supported chip revision */
    uint8_t     reserved[6];      /**< Reserved, must be 0 */
    uint32_t    payload_length;   /**< Sum of sizes of all loadable segments */
    uint32_t    crc32;            /**< CRC32/ISO-HDLC over preceding 28 bytes */
    uint8_t     sha256[32];       /**< SHA-256 over all segment data bytes */
} app_image_hdr_t;

/** Compile-time assertion: header must be exactly 80 bytes */
_Static_assert(sizeof(app_image_hdr_t) == APP_IMAGE_HDR_SIZE,
               "app_image_hdr_t size mismatch — check struct padding");

/* =========================================================================
 * SEGMENT HEADER (follows image header, one per segment_count)
 *
 * Each segment describes one memory region to load.
 * Format (16 bytes):
 *   +0x00  4B  load_addr    Virtual destination address in IRAM/DRAM
 *   +0x04  4B  data_len     Byte count of segment data
 *   +0x08  4B  reserved1    Must be 0
 *   +0x0C  4B  reserved2    Must be 0
 * Followed immediately by data_len bytes of segment payload.
 * ========================================================================= */

/** Size of one segment header in bytes */
#define APP_SEGMENT_HDR_SIZE        16U

/**
 * @brief Segment header structure.
 */
typedef struct __attribute__((packed)) {
    uint32_t    load_addr;   /**< Virtual DRAM/IRAM destination address */
    uint32_t    data_len;    /**< Number of bytes in this segment's data */
    uint32_t    reserved1;   /**< Reserved, must be 0 */
    uint32_t    reserved2;   /**< Reserved, must be 0 */
} app_segment_hdr_t;

_Static_assert(sizeof(app_segment_hdr_t) == APP_SEGMENT_HDR_SIZE,
               "app_segment_hdr_t size mismatch");

/* =========================================================================
 * OTA STATE BLOCK
 * Stored at FLASH_PARTITION_TBL_OFFSET (0x8000). Controls which slot is
 * the active boot target and tracks update progress.
 * ========================================================================= */

/** Magic word identifying a valid OTA state block */
#define OTA_STATE_MAGIC             0xCAFEBABEUL

/**
 * @brief Boot slot selection enum.
 */
typedef enum {
    BOOT_SLOT_NONE  = 0x00,  /**< No slot selected — force IAP */
    BOOT_SLOT_1     = 0x01,  /**< Boot from App Slot 1 (0x010000) */
    BOOT_SLOT_2     = 0x02,  /**< Boot from App Slot 2 (0x110000) */
} boot_slot_t;

/**
 * @brief OTA slot state machine.
 */
typedef enum {
    OTA_ST_EMPTY    = 0x00,  /**< Slot has never been programmed */
    OTA_ST_NEW      = 0x01,  /**< Image written, not yet verified */
    OTA_ST_PENDING  = 0x02,  /**< Verified, next boot target */
    OTA_ST_VALID    = 0x03,  /**< Successfully booted at least once */
    OTA_ST_INVALID  = 0x04,  /**< Failed integrity check */
    OTA_ST_ABORTED  = 0x05,  /**< Update was interrupted mid-transfer */
} ota_slot_state_t;

/**
 * @brief OTA control block written to FLASH_PARTITION_TBL_OFFSET.
 *
 * All enum-typed fields are stored as uint32_t to guarantee fixed
 * 4-byte width regardless of compiler enum size settings.
 *
 * Size breakdown (all packed, no padding needed since all 4-byte aligned):
 *   magic(4) + active_slot(4) + slot1_state(4) + slot2_state(4)
 *   + boot_attempt(1) + max_attempts(1) + padding(2)
 *   + slot1_version(4) + slot2_version(4) + last_update_unix(4)
 *   + rollback_allowed(1) + pad2(3) + crc32(4)
 *   = 4+4+4+4+1+1+2+4+4+4+1+3+4 = 40 bytes
 */
typedef struct __attribute__((packed)) {
    uint32_t    magic;               /**< OTA_STATE_MAGIC (0xCAFEBABE) */
    uint32_t    active_slot;         /**< boot_slot_t: 0=none,1=slot1,2=slot2 */
    uint32_t    slot1_state;         /**< ota_slot_state_t for slot 1 */
    uint32_t    slot2_state;         /**< ota_slot_state_t for slot 2 */
    uint8_t     boot_attempt;        /**< Incremented each boot attempt */
    uint8_t     max_attempts;        /**< Max retries before rollback (e.g. 3) */
    uint16_t    padding;             /**< Explicit pad to keep 4-byte alignment */
    uint32_t    slot1_version;       /**< Semantic version of slot 1 image */
    uint32_t    slot2_version;       /**< Semantic version of slot 2 image */
    uint32_t    last_update_unix;    /**< Timestamp of last successful update */
    uint8_t     rollback_allowed;    /**< 1 = allow rollback on failure */
    uint8_t     pad2[3];             /**< Explicit pad */
    uint32_t    crc32;               /**< CRC32 over all preceding bytes */
} ota_control_t;

_Static_assert(sizeof(ota_control_t) == 40U,
               "ota_control_t size mismatch — check struct layout");

/* =========================================================================
 * IAP / GPIO TRIGGER CONFIGURATION
 * ========================================================================= */

/** GPIO pin sampled at boot to detect hardware IAP request */
#define IAP_TRIGGER_GPIO            0U    /**< GPIO 0 = BOOT button on ESP32-CAM */

/** Active level: IAP triggered when GPIO reads this level */
#define IAP_TRIGGER_ACTIVE_LEVEL    0U    /**< Active LOW (button pulls GPIO to GND) */

/* =========================================================================
 * VIRTUAL ADDRESS CONSTANTS (CPU Address Space)
 * After the ROM BL copies our binary to IRAM, the CPU accesses our code at:
 * ========================================================================= */

/** Base virtual address of IRAM (start of our bootloader in CPU space) */
#define BL_IRAM_VADDR               0x40080000UL

/** Base virtual address of the flash XIP window (IBUS-mapped flash) */
#define FLASH_XIP_VADDR_BASE        0x400C0000UL

/** Virtual address of App Slot 1 in XIP window */
#define APP_SLOT1_XIP_VADDR         (FLASH_XIP_VADDR_BASE + FLASH_APP_SLOT1_OFFSET)

/** Virtual address of App Slot 2 in XIP window */
#define APP_SLOT2_XIP_VADDR         (FLASH_XIP_VADDR_BASE + FLASH_APP_SLOT2_OFFSET)

#endif /* FLASH_MAP_H */
