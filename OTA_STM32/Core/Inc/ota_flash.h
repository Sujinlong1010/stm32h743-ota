/**
 * @file  ota_flash.h
 * @brief STM32H743 dual-bank flash writer + slot header + metadata log.
 *
 *  Shared by OTA_STM32 (APP) and OTA_Bootloader, byte-identical copy.
 *  Keep all comments ASCII: Keil AC5 is used by both projects.
 */
#ifndef OTA_FLASH_H
#define OTA_FLASH_H

#include <stdint.h>
#include "ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Erase the whole 896KB slot (7 x 128KB sectors). */
int      ota_flash_erase_slot(uint8_t slot);
/* 只擦 slot 内偏移 off 所在的那一个 128KB 扇区（off 需按扇区对齐）。
   为什么要这个：平台同步接口只等 5 秒，而擦整个 896KB 槽要 7~14 秒，
   所以改成写入某个扇区前才擦它，单次擦除约 1.3 秒。 */
int      ota_flash_erase_off(uint8_t slot, uint32_t off);
/* 上一次 ota_flash_verify_slot 的失败原因码（0=通过）。 */
uint8_t  ota_flash_verify_reason(void);

/* Program n bytes at a 32-byte aligned address. Short tail is padded 0xFF
 * so it always forms a complete H7 FLASHWORD (8 x 32-bit). */
int      ota_flash_program(uint32_t addr, const uint8_t *p, uint32_t n);

int      ota_flash_read_fw_header(uint8_t slot, ota_fw_header_t *h);
int      ota_flash_write_fw_header(uint8_t slot, const ota_fw_header_t *h);

/* Full validation: header magic/size/range, vector table sanity and
 * zlib CRC32 over the raw binary. Returns 1 on success. */
int      ota_flash_verify_slot(uint8_t slot, ota_fw_header_t *h);

/* Append-only metadata log in bank2 sector 7 (64-byte records). */
int      ota_meta_load(ota_meta_t *m);
int      ota_meta_append(const ota_meta_t *m);
int      ota_meta_erase(void);

/* Copy the NUL-terminated version of a validated slot. "" if invalid. */
void     ota_flash_copy_version(uint8_t slot, char *out, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* OTA_FLASH_H */
