/**
 * @file  ota_flash.c
 * @brief STM32H743 dual-bank flash writer + slot header + metadata log.
 *
 *  Shared by OTA_STM32 (APP) and OTA_Bootloader, byte-identical copy.
 *  Written in strict C90 so Keil AC5 (armcc 5.06) accepts it.
 *
 *  H743 facts that this file relies on:
 *   - FLASH_EraseInitTypeDef.Sector is the sector INSIDE the selected bank
 *     (0..7), NOT a global 0..15 number. FLASH_SECTOR_TOTAL == 8.
 *   - One FLASHWORD is 32 bytes (8 x 32-bit); programming needs a 32-byte
 *     aligned address and a 32-byte source buffer.
 *   - Bank switching is handled by the HAL from the target address.
 */
#include "ota_flash.h"
#include "stm32h7xx_hal.h"
#include <string.h>

/* 上一次 verify 失败的原因码，给开机日志用：
 * 0=通过 1=读不到固件头 2=头字段非法(magic/长度/slot) 3=SP 非法
 * 4=PC 不在镜像内 5=CRC 不符 */
static uint8_t s_vfy_reason;

uint8_t ota_flash_verify_reason(void)
{
    return s_vfy_reason;
}

static uint32_t flash_bank_of(uint32_t addr)
{
    return (addr >= OTA_BANK2_BASE) ? (uint32_t)FLASH_BANK_2
                                    : (uint32_t)FLASH_BANK_1;
}

static uint32_t flash_bank_base(uint32_t bank)
{
    return (bank == (uint32_t)FLASH_BANK_2) ? OTA_BANK2_BASE : OTA_FLASH_BASE;
}

/* Erase one 128KB sector. Caller holds no lock; this function does. */
static int flash_erase_one(uint32_t addr)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t sector_error = 0u;
    uint32_t bank = flash_bank_of(addr);

    e.TypeErase    = FLASH_TYPEERASE_SECTORS;
    e.Banks        = bank;
    e.Sector       = (addr - flash_bank_base(bank)) / OTA_SECTOR_SIZE;
    e.NbSectors    = 1u;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_FLASH_Unlock();
    if (HAL_FLASHEx_Erase(&e, &sector_error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return 0;
    }
    HAL_FLASH_Lock();
    return 1;
}

int ota_flash_erase_slot(uint8_t slot)
{
    uint32_t base = OTA_SLOT_ADDR(slot);
    uint32_t off;

    if ((slot != OTA_SLOT_A) && (slot != OTA_SLOT_B))
    {
        return 0;
    }
    for (off = 0u; off < OTA_SLOT_SIZE; off += OTA_SECTOR_SIZE)
    {
        if (!flash_erase_one(base + off))
        {
            return 0;
        }
    }
    return 1;
}

int ota_flash_erase_off(uint8_t slot, uint32_t off)
{
    uint32_t base = OTA_SLOT_ADDR(slot);

    if ((slot != OTA_SLOT_A) && (slot != OTA_SLOT_B))
    {
        return 0;
    }
    if ((off % OTA_SECTOR_SIZE) != 0u)
    {
        return 0;
    }
    if (off >= OTA_SLOT_SIZE)
    {
        return 0;
    }
    return flash_erase_one(base + off);
}

int ota_flash_program(uint32_t addr, const uint8_t *p, uint32_t n)
{
    uint8_t wbuf[OTA_FLASH_WORD_SIZE];
    uint32_t off = 0u;
    uint32_t take, k;

    if ((p == 0) || ((addr & (OTA_FLASH_WORD_SIZE - 1u)) != 0u))
    {
        return 0;
    }

    HAL_FLASH_Unlock();
    while (off < n)
    {
        take = n - off;
        if (take > OTA_FLASH_WORD_SIZE)
        {
            take = OTA_FLASH_WORD_SIZE;
        }
        for (k = 0u; k < OTA_FLASH_WORD_SIZE; ++k)
        {
            wbuf[k] = 0xFFu;
        }
        for (k = 0u; k < take; ++k)
        {
            wbuf[k] = p[off + k];
        }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr + off,
                              (uint64_t)(uint32_t)wbuf) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return 0;
        }
        /* Always step a full FLASHWORD so the address stays 32B aligned. */
        off += OTA_FLASH_WORD_SIZE;
    }
    HAL_FLASH_Lock();
    return 1;
}

int ota_flash_read_fw_header(uint8_t slot, ota_fw_header_t *h)
{
    const uint8_t *p;

    if ((h == 0) || ((slot != OTA_SLOT_A) && (slot != OTA_SLOT_B)))
    {
        return 0;
    }
    if (sizeof(ota_fw_header_t) != OTA_FW_HEADER_SIZE)
    {
        return 0;
    }
    p = (const uint8_t *)(OTA_SLOT_ADDR(slot) + OTA_FW_HEADER_OFFSET);
    memcpy(h, p, sizeof(ota_fw_header_t));
    return 1;
}

int ota_flash_write_fw_header(uint8_t slot, const ota_fw_header_t *h)
{
    uint8_t buf[OTA_FW_HEADER_SIZE];
    uint32_t addr;

    if ((h == 0) || ((slot != OTA_SLOT_A) && (slot != OTA_SLOT_B)))
    {
        return 0;
    }
    if (sizeof(ota_fw_header_t) != OTA_FW_HEADER_SIZE)
    {
        return 0;
    }
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, h, sizeof(ota_fw_header_t));
    addr = OTA_SLOT_ADDR(slot) + OTA_FW_HEADER_OFFSET;
    return ota_flash_program(addr, buf, (uint32_t)sizeof(buf));
}

int ota_flash_verify_slot(uint8_t slot, ota_fw_header_t *h)
{
    ota_fw_header_t hdr;
    uint32_t base, sp, pc, crc;

    if ((slot != OTA_SLOT_A) && (slot != OTA_SLOT_B))
    {
        return 0;
    }
    s_vfy_reason = 1u;
    if (!ota_flash_read_fw_header(slot, &hdr))
    {
        return 0;
    }
    if (h != 0)
    {
        memcpy(h, &hdr, sizeof(hdr));
    }
    if ((hdr.magic != OTA_FW_MAGIC) ||
        (hdr.header_size != OTA_FW_HEADER_SIZE) ||
        (hdr.fw_size == 0u) || (hdr.fw_size > OTA_FW_MAX_SIZE) ||
        (hdr.slot != slot))
    {
        s_vfy_reason = 2u;
        return 0;
    }

    base = OTA_SLOT_ADDR(slot);
    sp = *(const uint32_t *)base;
    pc = *(const uint32_t *)(base + 4u);
    if ((sp == 0u) || (sp == 0xFFFFFFFFu))
    {
        s_vfy_reason = 3u;
        return 0;
    }
    /* Reset_Handler must point inside the image and be a Thumb address. */
    if ((pc < base + 8u) || (pc >= base + hdr.fw_size) || ((pc & 1u) == 0u))
    {
        s_vfy_reason = 4u;
        return 0;
    }

    crc = ota_crc32_update(0xFFFFFFFFu, (const uint8_t *)base, hdr.fw_size)
          ^ 0xFFFFFFFFu;
    if (crc != hdr.fw_crc32)
    {
        s_vfy_reason = 5u;
        return 0;
    }
    s_vfy_reason = 0u;
    return 1;
}

void ota_flash_copy_version(uint8_t slot, char *out, uint32_t cap)
{
    ota_fw_header_t hdr;
    uint32_t i;

    if ((out == 0) || (cap == 0u))
    {
        return;
    }
    out[0] = '\0';
    if (!ota_flash_verify_slot(slot, &hdr))
    {
        return;
    }
    for (i = 0u; (i < cap - 1u) && (i < sizeof(hdr.version)); ++i)
    {
        char c = (char)hdr.version[i];
        if (c == '\0')
        {
            break;
        }
        out[i] = c;
    }
    out[i] = '\0';
}

/* ------------------------------------------------------------ metadata -- */
int ota_meta_load(ota_meta_t *m)
{
    const uint8_t *base = (const uint8_t *)OTA_META_ADDR;
    uint32_t i;
    int found = 0;

    if (m == 0)
    {
        return 0;
    }
    for (i = 0u; i < OTA_META_MAX_RECORDS; ++i)
    {
        const ota_meta_t *r =
            (const ota_meta_t *)(base + i * OTA_META_RECORD_SIZE);
        uint32_t crc;

        if (r->magic != OTA_META_MAGIC)
        {
            break;                 /* first blank record = end of the log */
        }
        crc = ota_crc32((const uint8_t *)r, 32u);
        if (crc != r->crc32)
        {
            break;                 /* torn write, use the previous record */
        }
        memcpy(m, r, sizeof(ota_meta_t));
        found = 1;
    }
    return found;
}

int ota_meta_append(const ota_meta_t *m)
{
    const uint8_t *base = (const uint8_t *)OTA_META_ADDR;
    uint8_t buf[OTA_META_RECORD_SIZE];
    ota_meta_t local;
    uint32_t i, k, free_idx, addr;
    int blank;

    if ((m == 0) || (sizeof(ota_meta_t) > OTA_META_RECORD_SIZE))
    {
        return 0;
    }

    free_idx = OTA_META_MAX_RECORDS;
    for (i = 0u; i < OTA_META_MAX_RECORDS; ++i)
    {
        const uint32_t *w =
            (const uint32_t *)(base + i * OTA_META_RECORD_SIZE);
        blank = 1;
        for (k = 0u; k < OTA_META_RECORD_SIZE / 4u; ++k)
        {
            if (w[k] != 0xFFFFFFFFu)
            {
                blank = 0;
                break;
            }
        }
        if (blank)
        {
            free_idx = i;
            break;
        }
    }

    if (free_idx >= OTA_META_MAX_RECORDS)
    {
        if (!ota_meta_erase())
        {
            return 0;
        }
        free_idx = 0u;
    }

    memcpy(&local, m, sizeof(local));
    local.magic = OTA_META_MAGIC;
    local.crc32 = ota_crc32((const uint8_t *)&local, 32u);

    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &local, sizeof(local));
    addr = OTA_META_ADDR + free_idx * OTA_META_RECORD_SIZE;
    return ota_flash_program(addr, buf, (uint32_t)sizeof(buf));
}

int ota_meta_erase(void)
{
    /* Metadata occupies bank2 sector 7 (0x081E0000). */
    return flash_erase_one(OTA_META_ADDR);
}
