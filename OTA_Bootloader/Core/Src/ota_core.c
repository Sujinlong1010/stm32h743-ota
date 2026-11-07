/**
 * @file  ota_core.c
 * @brief OTA session state machine shared by STM32 APP and Bootloader.
 *
 *  Flow (identical for local USB and OneNET cloud, because ESP32 is a dumb
 *  frame bridge):
 *
 *    PC -> CMD_OTA_START (fw_size, fw_crc32, version, target_slot)
 *          STM32 erases the INACTIVE slot (never the running one)
 *    PC -> CMD_FW_CHUNK (u32 offset + raw bytes), strict sequential offsets
 *          STM32 streams into 32-byte H7 FLASHWORDs
 *    PC -> CMD_FW_END
 *          STM32 writes the 256B header at the slot tail, re-reads and
 *          CRC32-verifies the whole image, then appends a trial metadata
 *          record and reports OTA_STAGE_SUCCESS
 *    PC -> CMD_REBOOT
 *          STM32 defers the reset until the ACK has left the UART
 *    Bootloader -> trial boots the new slot; 3 failed tries => rollback
 *    APP -> ota_core_confirm_app() clears the trial flag
 *
 *  Strict C90 (Keil AC5). No C99 declarations inside statements.
 */
#include "ota_core.h"
#include "ota_flash.h"
#include "ota_link.h"
#include <string.h>

typedef struct
{
    uint8_t  busy;
    uint8_t  target_slot;
    uint32_t fw_size;
    uint32_t fw_crc;
    uint32_t received;
    uint32_t running_crc;
    uint32_t next_addr;
    uint8_t  wbuf[OTA_FLASH_WORD_SIZE];
    uint16_t wlen;
    uint8_t  version[24];
    uint32_t last_ms;
    uint32_t last_report;
    uint32_t reset_at;
    uint8_t  need_reset;
} ota_session_t;

static ota_core_cfg_t s_cfg;
static ota_session_t  s_s;
static ota_status_t   s_status;
static uint8_t        s_boot_slot = OTA_SLOT_A;

static uint32_t core_now(void)
{
    return (s_cfg.millis != 0) ? s_cfg.millis() : 0u;
}

static void set_status(uint8_t stage, uint8_t result, uint8_t progress,
                       uint32_t detail)
{
    s_status.stage       = stage;
    s_status.result      = result;
    s_status.progress    = progress;
    s_status.active_slot = s_cfg.active_slot;
    s_status.detail      = detail;
}

static void core_log(const char *s)
{
    (void)ota_link_log(s);
}

static void schedule_reset(void)
{
    uint32_t d = s_cfg.reset_delay_ms;
    if (d == 0u)
    {
        d = 250u;
    }
    s_s.need_reset = 1u;
    s_s.reset_at   = core_now() + d;
}

/* 惰性擦除游标：从槽基址开始，已经擦到哪个地址（永远按 128KB 扇区对齐）。
 *
 * 为什么不能一次擦完整个槽：OneNET 的 set-device-property 是同步接口，
 * 平台自己只等约 5 秒就返回 10411「设备响应超时」，而擦 896KB（7 个扇区）
 * 要 7~14 秒 —— 无论 ESP32 愿意等多久都没用。改成写某个扇区之前才擦它，
 * 单次擦除约 1.3 秒，稳定落在平台超时以内。 */
static uint32_t s_erase_upto;

/* 保证 [slot_base, end_addr) 覆盖到的扇区都已被擦除。 */
static int ensure_erased(uint32_t end_addr)
{
    uint32_t base = OTA_SLOT_ADDR(s_s.target_slot);

    while (s_erase_upto < end_addr)
    {
        if (!ota_flash_erase_off(s_s.target_slot, s_erase_upto - base))
        {
            return 0;
        }
        s_erase_upto += OTA_SECTOR_SIZE;
    }
    return 1;
}

/* 所有写入都必须走这里，保证目标扇区先被擦过。 */
static int flash_word(uint32_t addr, const uint8_t *p)
{
    if (!ensure_erased(addr + OTA_FLASH_WORD_SIZE))
    {
        return 0;
    }
    return ota_flash_program(addr, p, OTA_FLASH_WORD_SIZE);
}

static int writer_write(const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0u; i < n; ++i)
    {
        s_s.wbuf[s_s.wlen++] = p[i];
        if (s_s.wlen == OTA_FLASH_WORD_SIZE)
        {
            if (!flash_word(s_s.next_addr, s_s.wbuf))
            {
                return 0;
            }
            s_s.next_addr += OTA_FLASH_WORD_SIZE;
            s_s.wlen = 0u;
        }
    }
    return 1;
}

static int writer_flush(void)
{
    uint16_t k;

    if (s_s.wlen == 0u)
    {
        return 1;
    }
    for (k = s_s.wlen; k < (uint16_t)OTA_FLASH_WORD_SIZE; ++k)
    {
        s_s.wbuf[k] = 0xFFu;
    }
    if (!flash_word(s_s.next_addr, s_s.wbuf))
    {
        return 0;
    }
    s_s.next_addr += OTA_FLASH_WORD_SIZE;
    s_s.wlen = 0u;
    return 1;
}

static int meta_write(uint8_t active, uint8_t trial, uint8_t boot_try,
                      uint8_t result)
{
    ota_meta_t m;
    memset(&m, 0, sizeof(m));
    m.active_slot = active;
    m.trial_slot  = trial;
    m.boot_try    = boot_try;
    m.last_result = result;
    return ota_meta_append(&m);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void ota_core_init(const ota_core_cfg_t *cfg)
{
    if (cfg != 0)
    {
        memcpy(&s_cfg, cfg, sizeof(s_cfg));
    }
    memset(&s_s, 0, sizeof(s_s));
    set_status(OTA_STAGE_IDLE, ST_OK, 0u, 0u);
    s_s.last_ms     = core_now();
    s_s.last_report = s_s.last_ms;
}

void ota_core_get_status(ota_status_t *st)
{
    if (st != 0)
    {
        memcpy(st, &s_status, sizeof(ota_status_t));
    }
}

uint8_t ota_core_active_slot(void)
{
    return s_cfg.active_slot;
}

uint8_t ota_core_is_idle(void)
{
    return (uint8_t)(s_s.busy ? 0u : 1u);
}

uint32_t ota_core_slot_base(uint8_t slot)
{
    return OTA_SLOT_ADDR(slot);
}

void ota_core_copy_version(char *out, uint32_t cap)
{
    uint8_t slot = (s_cfg.active_slot == OTA_SLOT_NONE)
                 ? s_boot_slot : s_cfg.active_slot;
    ota_flash_copy_version(slot, out, cap);
}

/* ------------------------------------------------------------- handler -- */
static uint8_t on_start(const uint8_t *payload, uint16_t len)
{
    ota_start_t st;

    if (len < (uint16_t)sizeof(ota_start_t))
    {
        return (uint8_t)ST_BAD_CMD;
    }
    memcpy(&st, payload, sizeof(st));
    if (s_s.busy)
    {
        return (uint8_t)ST_BUSY;
    }
    if ((st.target_slot != OTA_SLOT_A) && (st.target_slot != OTA_SLOT_B))
    {
        return (uint8_t)ST_BAD_CMD;
    }
    if ((s_cfg.active_slot != OTA_SLOT_NONE) &&
        (st.target_slot == s_cfg.active_slot))
    {
        core_log("OTA: refusing to erase the running slot");
        return (uint8_t)ST_SLOT_BUSY;
    }
    if ((st.fw_size == 0u) || (st.fw_size > OTA_FW_MAX_SIZE))
    {
        return (uint8_t)ST_NO_SPACE;
    }

    set_status(OTA_STAGE_WRITING, ST_OK, 0u, 0u);
    /* 只复位游标，不擦。第一个分片到达时才擦它所在的第一个扇区。 */
    s_erase_upto = OTA_SLOT_ADDR(st.target_slot);
    core_log("OTA: start accepted, sectors erased on demand");

    s_s.busy         = 1u;
    s_s.target_slot  = st.target_slot;
    s_s.fw_size      = st.fw_size;
    s_s.fw_crc       = st.fw_crc32;
    s_s.received     = 0u;
    s_s.running_crc  = 0xFFFFFFFFu;
    s_s.next_addr    = OTA_SLOT_ADDR(st.target_slot);
    s_s.wlen         = 0u;
    s_s.last_ms      = core_now();
    memcpy(s_s.version, st.version, sizeof(s_s.version));
    set_status(OTA_STAGE_RECEIVING, ST_OK, 0u, 0u);
    core_log("OTA: ready for chunks");
    return (uint8_t)ST_OK;
}

static uint8_t on_chunk(const uint8_t *payload, uint16_t len)
{
    uint32_t off, n;

    if (!s_s.busy)
    {
        return (uint8_t)ST_BAD_SEQ;
    }
    if (len < 4u)
    {
        return (uint8_t)ST_BAD_SEQ;
    }
    off = rd32(payload);
    n   = (uint32_t)len - 4u;
    if (off != s_s.received)
    {
        return (uint8_t)ST_BAD_SEQ;
    }
    if ((n == 0u) || (s_s.received + n > s_s.fw_size))
    {
        return (uint8_t)ST_NO_SPACE;
    }
    if (!writer_write(&payload[4], n))
    {
        s_s.busy = 0u;
        set_status(OTA_STAGE_FAILED, ST_WRITE_FAIL, 0u, s_s.received);
        return (uint8_t)ST_WRITE_FAIL;
    }
    s_s.running_crc = ota_crc32_update(s_s.running_crc, &payload[4], n);
    s_s.received   += n;
    s_s.last_ms     = core_now();
    set_status(OTA_STAGE_RECEIVING, ST_OK,
               (uint8_t)((s_s.received * 100u) / s_s.fw_size), s_s.received);
    return (uint8_t)ST_OK;
}

static uint8_t on_end(void)
{
    ota_fw_header_t hdr;
    uint32_t crc;
    uint32_t hdr_sector_off;

    if (!s_s.busy)
    {
        return (uint8_t)ST_BAD_SEQ;
    }
    if (s_s.received != s_s.fw_size)
    {
        return (uint8_t)ST_BAD_SEQ;
    }
    if (!writer_flush())
    {
        s_s.busy = 0u;
        set_status(OTA_STAGE_FAILED, ST_WRITE_FAIL, 0u, s_s.received);
        return (uint8_t)ST_WRITE_FAIL;
    }

    crc = s_s.running_crc ^ 0xFFFFFFFFu;
    if (crc != s_s.fw_crc)
    {
        s_s.busy = 0u;
        set_status(OTA_STAGE_FAILED, ST_BAD_CRC, 0u, crc);
        return (uint8_t)ST_BAD_CRC;
    }

    /* 固件头存的偏移不是扇区对齐的（896K - 256），按扇区取下界来擦。 */
    hdr_sector_off = (uint32_t)((OTA_FW_HEADER_OFFSET / OTA_SECTOR_SIZE)
                                * OTA_SECTOR_SIZE);

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = OTA_FW_MAGIC;
    hdr.header_size = OTA_FW_HEADER_SIZE;
    hdr.fw_size     = s_s.fw_size;
    hdr.fw_crc32    = s_s.fw_crc;
    hdr.slot        = s_s.target_slot;
    hdr.confirmed   = 0u;
    hdr.build_time  = 0u;
    memcpy(hdr.version, s_s.version, sizeof(hdr.version));
    hdr.version[sizeof(hdr.version) - 1u] = 0u;

    /* 固件头写在槽的最后一个扇区。38KB 的固件只占第一个扇区，
     * 惰性擦除根本没擦到最后一扇区，直接 program 会失败（HAL 对未擦除
     * 地址编程会报错）。所以这里单独补擦一次。
     * 注意条件方向：只有当「该扇区还没被擦过」时才擦 —— 如果固件大到
     * 覆盖了最后一个扇区，它已经被擦过并写入了数据，再擦就把固件擦掉了。 */
    if (s_erase_upto <= (OTA_SLOT_ADDR(s_s.target_slot) + hdr_sector_off))
    {
        if (!ota_flash_erase_off(s_s.target_slot, hdr_sector_off))
        {
            s_s.busy = 0u;
            set_status(OTA_STAGE_FAILED, ST_ERASE_FAIL, 0u, s_s.received);
            return (uint8_t)ST_ERASE_FAIL;
        }
    }

    if (!ota_flash_write_fw_header(s_s.target_slot, &hdr))
    {
        s_s.busy = 0u;
        set_status(OTA_STAGE_FAILED, ST_WRITE_FAIL, 0u, s_s.received);
        return (uint8_t)ST_WRITE_FAIL;
    }
    set_status(OTA_STAGE_VERIFYING, ST_OK, 99u, s_s.fw_size);
    if (!ota_flash_verify_slot(s_s.target_slot, 0))
    {
        s_s.busy = 0u;
        set_status(OTA_STAGE_FAILED, ST_VERIFY_FAIL, 0u, crc);
        return (uint8_t)ST_VERIFY_FAIL;
    }

    if (!meta_write(s_s.target_slot, s_s.target_slot, 0u, ST_OK))
    {
        s_s.busy = 0u;
        set_status(OTA_STAGE_FAILED, ST_WRITE_FAIL, 0u, s_s.received);
        return (uint8_t)ST_WRITE_FAIL;
    }

    s_s.busy = 0u;
    set_status(OTA_STAGE_SUCCESS, ST_OK, 100u, s_s.fw_size);
    (void)ota_link_report(CMD_OTA_RESULT, 0u,
                          (const uint8_t *)&s_status,
                          (uint16_t)sizeof(ota_status_t));
    core_log("OTA: image verified, trial pending reboot");
    set_status(OTA_STAGE_PENDING_REBOOT, ST_OK, 100u, s_s.fw_size);
    return (uint8_t)ST_OK;
}

uint8_t ota_core_on_frame(uint16_t cmd, uint16_t seq,
                          const uint8_t *payload, uint16_t len)
{
    ota_status_t snap;

    (void)seq;
    switch (cmd)
    {
    case CMD_PING:
        return (uint8_t)ST_OK;

    case CMD_GET_STATUS:
        memcpy(&snap, &s_status, sizeof(snap));
        ota_link_set_ack_extra((const uint8_t *)&snap,
                               (uint16_t)sizeof(snap));
        return (uint8_t)ST_OK;

    case CMD_OTA_START:
        return on_start(payload, len);

    case CMD_FW_CHUNK:
        return on_chunk(payload, len);

    case CMD_FW_END:
        return on_end();

    case CMD_OTA_ABORT:
        s_s.busy = 0u;
        s_s.wlen = 0u;
        set_status(OTA_STAGE_IDLE, ST_OK, 0u, 0u);
        core_log("OTA: aborted, running slot untouched");
        return (uint8_t)ST_OK;

    case CMD_REBOOT:
        schedule_reset();
        return (uint8_t)ST_OK;

    case CMD_SWITCH_SLOT:
    {
        uint8_t cur = (s_cfg.active_slot == OTA_SLOT_NONE)
                    ? s_boot_slot : s_cfg.active_slot;
        uint8_t other = (uint8_t)(1u - cur);
        if (!ota_flash_verify_slot(other, 0))
        {
            return (uint8_t)ST_BAD_HEADER;
        }
        (void)meta_write(other, OTA_SLOT_NONE, 0u, ST_OK);
        schedule_reset();
        return (uint8_t)ST_OK;
    }

    case CMD_WIFI_STATE:
    case CMD_CLOUD_CMD:
    case CMD_CLOUD_STATE:
    case CMD_SET_WIFI:
        return (uint8_t)ST_OK;

    default:
        return (uint8_t)ST_BAD_CMD;
    }
}

void ota_core_poll(void)
{
    uint32_t t = core_now();

    if (s_s.need_reset && (t >= s_s.reset_at))
    {
        s_s.need_reset = 0u;
        if (s_cfg.reset_now != 0)
        {
            s_cfg.reset_now();
        }
        return;
    }
    if (s_s.busy && ((t - s_s.last_ms) > OTA_SESSION_TIMEOUT_MS))
    {
        s_s.busy = 0u;
        s_s.wlen = 0u;
        set_status(OTA_STAGE_FAILED, ST_TIMEOUT, 0u, s_s.received);
        core_log("OTA: session timed out");
    }
    /* Only the APP reports periodically; the Bootloader stays quiet. */
    if ((s_cfg.active_slot != OTA_SLOT_NONE) &&
        ((t - s_s.last_report) > OTA_STATUS_PERIOD_MS))
    {
        s_s.last_report = t;
        (void)ota_link_report(CMD_OTA_PROGRESS, 0u,
                              (const uint8_t *)&s_status,
                              (uint16_t)sizeof(ota_status_t));
    }
}

/* ---------------------------------------------------------- bootloader -- */
uint8_t ota_core_pick_boot_slot(void)
{
    ota_meta_t m;
    uint8_t active, trial, other;
    int have;

    have = ota_meta_load(&m);
    if (!have)
    {
        memset(&m, 0, sizeof(m));
        m.active_slot = OTA_SLOT_A;
        m.trial_slot  = OTA_SLOT_NONE;
        m.boot_try    = 0u;
        m.last_result = ST_OK;
    }
    active = (m.active_slot == OTA_SLOT_B) ? OTA_SLOT_B : OTA_SLOT_A;
    trial  = m.trial_slot;

    if ((trial == OTA_SLOT_A) || (trial == OTA_SLOT_B))
    {
        if (m.boot_try >= OTA_BOOT_TRY_MAX)
        {
            other = (uint8_t)(1u - trial);
            (void)meta_write(other, OTA_SLOT_NONE, 0u, ST_ROLLBACK);
            active = other;
            set_status(OTA_STAGE_ROLLED_BACK, ST_ROLLBACK, 0u, 0u);
        }
        else
        {
            (void)meta_write(trial, trial,
                             (uint8_t)(m.boot_try + 1u), m.last_result);
            active = trial;
        }
    }

    if (!ota_flash_verify_slot(active, 0))
    {
        other = (uint8_t)(1u - active);
        if (!ota_flash_verify_slot(other, 0))
        {
            s_boot_slot = OTA_SLOT_A;
            return (uint8_t)OTA_SLOT_NONE;
        }
        active = other;
    }

    s_boot_slot = active;
    set_status(OTA_STAGE_IDLE, ST_OK, 0u, 0u);
    return active;
}

int ota_core_confirm_app(void)
{
    ota_meta_t m;

    if (s_cfg.active_slot == OTA_SLOT_NONE)
    {
        return 0;
    }
    if (!ota_meta_load(&m))
    {
        return 0;
    }
    if (m.trial_slot != s_cfg.active_slot)
    {
        return 0;                 /* nothing pending, do not touch flash */
    }
    m.active_slot = s_cfg.active_slot;
    m.trial_slot  = OTA_SLOT_NONE;
    m.boot_try    = 0u;
    m.last_result = ST_OK;
    return ota_meta_append(&m);
}
