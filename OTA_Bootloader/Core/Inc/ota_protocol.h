/**
 * @file  ota_protocol.h
 * @brief Shared wire contract for PC tool  <->  ESP32  <->  STM32H743.
 *
 *  THIS FILE IS THE SINGLE SOURCE OF TRUTH.
 *  Master copy: OTA_STM32/Core/Inc/ota_protocol.h
 *  All copies below must stay byte-identical:
 *    OTA_STM32/Core/Inc/ota_protocol.h      (master)
 *    OTA_Bootloader/Core/Inc/ota_protocol.h
 *    OTA_ESP32/include/ota_protocol.h
 *  Python side: OTA_PC_Tool/ota_framer.py mirrors every constant below.
 *
 *  Layout rule: shared protocol files live INSIDE each project, not in a
 *  top-level folder. Verify with: python OTA_STM32/check_generate.py
 *
 *  NOTE: keep all comments ASCII. Keil AC5 does not reliably handle UTF-8
 *        comments in headers included by C sources.
 *
 *  Frame layout (all multi-byte fields LITTLE ENDIAN):
 *
 *   off  len  field
 *    0    2   MAGIC   0xAA 0x55
 *    2    1   VER     0x01
 *    3    1   FLAG    bit0 = RESP (1 = reply / unsolicited report)
 *    4    2   CMD     command code
 *    6    2   SEQ     sequence number / firmware chunk number
 *    8    2   LEN     payload byte count (<= OTA_FRAME_MAX_PAYLOAD)
 *   10   LEN  PAYLOAD
 *   10+L 4   CRC32   over bytes [0 .. 10+LEN), standard zlib CRC32
 *                    (poly 0xEDB88320, init 0xFFFFFFFF, final xor 0xFFFFFFFF)
 *
 *   total length = 14 + LEN, max 1038 bytes.
 *
 *  Link topology:
 *    PC (USB CDC COM port) <-- Serial --> ESP32-S3 <-- UART1 921600 --> STM32
 *    ESP32 acts as a transparent frame bridge in local mode, and as a
 *    WiFi/MQTT/HTTP gateway in cloud mode. STM32 only ever speaks this
 *    frame format, no matter where the command came from.
 */

#ifndef OTA_PROTOCOL_H
#define OTA_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- frame -- */
#define OTA_MAGIC0                  0xAAu
#define OTA_MAGIC1                  0x55u
#define OTA_PROTO_VER               0x01u
#define OTA_FLAG_RESP               0x01u
#define OTA_FRAME_HEADER_LEN        10u
#define OTA_FRAME_CRC_LEN           4u
#define OTA_FRAME_OVERHEAD          (OTA_FRAME_HEADER_LEN + OTA_FRAME_CRC_LEN)
#define OTA_FRAME_MAX_PAYLOAD       1024u
#define OTA_FRAME_MAX_LEN           (OTA_FRAME_OVERHEAD + OTA_FRAME_MAX_PAYLOAD)

/* ------------------------------------------------------------- commands -- */
#define CMD_PING                    0x0001u  /* heartbeat, payload empty      */
#define CMD_GET_STATUS              0x0002u  /* ask STM32 for its state       */
#define CMD_WIFI_STATE              0x0010u  /* ESP32 -> STM32 wifi report    */
#define CMD_CLOUD_CMD               0x0011u  /* OneNET command -> STM32       */
#define CMD_CLOUD_STATE             0x0012u  /* STM32 -> ESP32 -> OneNET      */

#define CMD_OTA_START               0x0020u  /* begin upgrade, ota_start_t    */
#define CMD_FW_CHUNK                0x0021u  /* offset(4) + raw bytes         */
#define CMD_FW_END                  0x0022u  /* all chunks sent, verify now   */
#define CMD_OTA_ABORT               0x0023u  /* cancel, keep current slot     */
#define CMD_OTA_RESULT              0x0030u  /* STM32 report, ota_status_t    */
#define CMD_OTA_PROGRESS            0x0031u  /* STM32 report, ota_status_t    */

#define CMD_SWITCH_SLOT             0x0040u  /* demo: jump to the other slot  */
#define CMD_REBOOT                  0x0041u  /* soft reset                    */
#define CMD_SET_WIFI                0x0042u  /* PC -> ESP32 ssid/password     */

#define CMD_LOG                     0x00E0u  /* plain ASCII debug text        */
#define CMD_ERROR                   0x00FFu  /* status byte in payload[0]     */

/* --------------------------------------------------------------- status -- */
#define ST_OK                       0x00u
#define ST_BUSY                     0x01u
#define ST_BAD_CRC                  0x02u
#define ST_BAD_SEQ                  0x03u
#define ST_BAD_CMD                  0x04u
#define ST_ERASE_FAIL               0x05u
#define ST_WRITE_FAIL               0x06u
#define ST_VERIFY_FAIL              0x07u
#define ST_SLOT_BUSY                0x08u
#define ST_TIMEOUT                  0x09u
#define ST_NO_SPACE                 0x0Au
#define ST_ROLLBACK                 0x0Bu
#define ST_NO_WIFI                  0x0Cu
#define ST_BAD_HEADER               0x0Du

/* ---------------------------------------------------------- ota stages -- */
#define OTA_STAGE_IDLE              0u
#define OTA_STAGE_RECEIVING         1u
#define OTA_STAGE_VERIFYING         2u
#define OTA_STAGE_WRITING           3u
#define OTA_STAGE_PENDING_REBOOT    4u
#define OTA_STAGE_SUCCESS           5u
#define OTA_STAGE_FAILED            6u
#define OTA_STAGE_ROLLED_BACK       7u

/* --------------------------------------------------------------- slots -- */
#define OTA_SLOT_A                  0u
#define OTA_SLOT_B                  1u
#define OTA_SLOT_NONE               0xFFu

/* ------------------------------------------------ flash layout (H743) -- */
/* 2MB internal flash, DUAL BANK: bank1 0x08000000..0x080FFFFF,
 * bank2 0x08100000..0x081FFFFF, each bank has 8 sectors x 128KB.
 *
 * IMPORTANT (H7 HAL quirk): FLASH_EraseInitTypeDef.Sector is the sector
 * number INSIDE the selected bank (0..7), NOT a global 0..15 number.
 * FLASH_SECTOR_TOTAL is 8 on STM32H743. So slot A is bank1 sectors 1..7,
 * slot B is bank2 sectors 0..6, metadata is bank2 sector 7.
 *
 *   Bootloader : 0x08000000  bank1 sector 0          128KB
 *   Slot A     : 0x08020000  bank1 sectors 1..7      896KB
 *   Slot B     : 0x08100000  bank2 sectors 0..6      896KB
 *   Metadata   : 0x081E0000  bank2 sector 7          128KB
 *
 * Firmware header lives in the LAST 256 bytes of a slot, NOT at its start.
 * Reason: the APP vector table must sit exactly at the slot start so that
 * SCB->VTOR can be set to a properly aligned slot base. A 256B prefix would
 * force VTOR to slot+0x100, which is risky on Cortex-M7 with 150+ vectors.
 * The raw binary is written from the slot start; the header is written last.
 */
#define OTA_FLASH_BASE              0x08000000u
#define OTA_BANK2_BASE              0x08100000u
#define OTA_SECTOR_SIZE             (128u * 1024u)
#define OTA_BOOT_ADDR               0x08000000u
#define OTA_BOOT_SIZE               (128u * 1024u)
#define OTA_SLOT_A_ADDR             0x08020000u
#define OTA_SLOT_B_ADDR             0x08100000u
#define OTA_SLOT_SIZE               (896u * 1024u)
#define OTA_META_ADDR               0x081E0000u
#define OTA_META_SIZE               (128u * 1024u)
#define OTA_FW_HEADER_SIZE          256u
#define OTA_FW_HEADER_OFFSET        (OTA_SLOT_SIZE - OTA_FW_HEADER_SIZE)
#define OTA_FW_MAX_SIZE             (OTA_SLOT_SIZE - OTA_FW_HEADER_SIZE)
#define OTA_FLASH_WORD_SIZE         32u   /* H7 programs 8x32bit per write */
#define OTA_BOOT_TRY_MAX            3u    /* trial boots before rollback  */

/* Metadata sector is used as an append-only log: 64-byte records so every
 * write is naturally aligned to the 32-byte flash word. No erase needed for
 * a state change until all records are consumed. */
#define OTA_META_RECORD_SIZE        64u
#define OTA_META_MAX_RECORDS        (OTA_META_SIZE / OTA_META_RECORD_SIZE)

#define OTA_SLOT_ADDR(slot) \
    ((uint32_t)((slot) == OTA_SLOT_B ? OTA_SLOT_B_ADDR : OTA_SLOT_A_ADDR))

/* ------------------------------------------------------- payload types -- */
typedef struct
{
    uint32_t fw_size;        /* bytes of the binary, header NOT included */
    uint32_t fw_crc32;       /* zlib CRC32 of the binary                 */
    uint8_t  version[24];    /* NUL terminated, e.g. "2.0.0"            */
    uint8_t  target_slot;    /* OTA_SLOT_A / OTA_SLOT_B                  */
    uint8_t  reserved[3];
} ota_start_t;               /* 36 bytes */

typedef struct
{
    uint8_t  stage;          /* OTA_STAGE_*                              */
    uint8_t  result;         /* ST_*                                     */
    uint8_t  progress;       /* 0..100                                   */
    uint8_t  active_slot;    /* slot currently running                   */
    uint32_t detail;         /* bytes received / error detail            */
} ota_status_t;              /* 8 bytes */

typedef struct
{
    uint8_t  connected;      /* 0 / 1                                    */
    int8_t   rssi;           /* dBm                                      */
    uint8_t  ip[4];          /* IPv4                                     */
} wifi_state_t;              /* 6 bytes */

/* Firmware header stored at slot_base + OTA_FW_HEADER_OFFSET (slot tail).
 * Size MUST be exactly OTA_FW_HEADER_SIZE (256).
 * fw_crc32 covers the raw binary at slot_base .. slot_base + fw_size - 1.
 */
#define OTA_FW_MAGIC                0x3141544Fu  /* "OTA1" little endian */
typedef struct
{
    uint32_t magic;          /* OTA_FW_MAGIC                             */
    uint32_t header_size;    /* 256                                      */
    uint32_t fw_size;        /* payload size                             */
    uint32_t fw_crc32;       /* zlib CRC32 of payload                    */
    uint8_t  version[24];    /* NUL terminated                           */
    uint8_t  slot;           /* A or B                                   */
    uint8_t  confirmed;      /* 0 = trial, 1 = confirmed good            */
    uint8_t  reserved[2];
    uint32_t build_time;     /* unix time, informational                 */
    uint8_t  pad[208];       /* -> total 256 bytes                       */
} ota_fw_header_t;

/* Boot metadata. One record per state change, appended into the metadata
 * sector at record_index * OTA_META_RECORD_SIZE. Bootloader scans forward
 * and uses the LAST record with a valid magic + crc32. */
#define OTA_META_MAGIC              0x42544F4Fu  /* "OOTB" little endian */
typedef struct
{
    uint32_t magic;          /* OTA_META_MAGIC                           */
    uint8_t  active_slot;    /* slot to boot                             */
    uint8_t  trial_slot;     /* slot awaiting confirmation, 0xFF = none  */
    uint8_t  boot_try;       /* consecutive failed attempts              */
    uint8_t  last_result;    /* ST_* of the last upgrade                 */
    uint32_t last_version_crc;
    uint8_t  reserved[20];
    uint32_t crc32;          /* CRC32 over the preceding 32 bytes        */
} ota_meta_t;                /* 36 bytes, CRC covers the first 32 */

/* ------------------------------------------------------------- CRC32 ----- */
/* AC5 is C90 and does not accept "inline" here; plain static is safest
 * for both Keil AC5 (STM32) and GCC/xtensa (ESP32). */
static uint32_t ota_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint32_t i, b;
    for (i = 0; i < n; ++i)
    {
        crc ^= (uint32_t)p[i];
        for (b = 0; b < 8u; ++b)
        {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static uint32_t ota_crc32(const uint8_t *p, uint32_t n)
{
    return ota_crc32_update(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

#ifdef __cplusplus
}
#endif

#endif /* OTA_PROTOCOL_H */
