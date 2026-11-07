/**
 * @file  ota_link.c
 * @brief Shared frame parser / builder. See ota_link.h for the wire rules.
 */
#include "ota_link.h"
#include <string.h>

static ota_link_io_t        s_io;
static ota_link_handler_t   s_cb;
static uint8_t              s_tx[OTA_FRAME_MAX_LEN];
static uint8_t              s_rx[OTA_FRAME_MAX_LEN];
static uint16_t             s_pos;
static uint16_t             s_need;
static uint32_t             s_last_frame;
/* 最近一次收到字节的时刻，用于半帧超时重同步。 */
static uint32_t             s_rx_last_ms;
static uint8_t              s_ack_extra[OTA_LINK_ACK_EXTRA_MAX];
static uint16_t             s_ack_extra_len;
static uint32_t             s_rx_ok;      /* CRC 通过的帧计数，屏幕显示用 */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

void ota_link_init(const ota_link_io_t *io, ota_link_handler_t cb)
{
    s_io.tx     = io->tx;
    s_io.rx     = io->rx;
    s_io.millis = io->millis;
    s_cb        = cb;
    s_pos           = 0;
    s_need          = 0;
    s_ack_extra_len = 0;
    s_last_frame    = (io->millis != 0) ? io->millis() : 0u;
    s_rx_last_ms    = (io->millis != 0) ? io->millis() : 0u;
}

uint32_t ota_link_now(void)
{
    return (s_io.millis != 0) ? s_io.millis() : 0u;
}

uint32_t ota_link_last_frame_time(void)
{
    return s_last_frame;
}

uint32_t ota_link_rx_ok(void)
{
    return s_rx_ok;
}

/* Build and send one frame. resp = 1 for reports, 0 for raw requests. */
static int send_frame(uint16_t cmd, uint16_t seq, const uint8_t *payload,
                      uint16_t len, uint8_t resp)
{
    uint32_t crc;
    uint16_t total;

    if ((s_io.tx == 0) || (len > OTA_FRAME_MAX_PAYLOAD))
    {
        return 0;
    }

    s_tx[0] = (uint8_t)OTA_MAGIC0;
    s_tx[1] = (uint8_t)OTA_MAGIC1;
    s_tx[2] = (uint8_t)OTA_PROTO_VER;
    s_tx[3] = resp ? (uint8_t)OTA_FLAG_RESP : 0u;
    wr16(&s_tx[4], cmd);
    wr16(&s_tx[6], seq);
    wr16(&s_tx[8], len);

    if ((len > 0u) && (payload != 0))
    {
        memcpy(&s_tx[OTA_FRAME_HEADER_LEN], payload, len);
    }

    total = (uint16_t)(OTA_FRAME_HEADER_LEN + len);
    crc = ota_crc32(s_tx, (uint32_t)total);
    s_tx[total + 0] = (uint8_t)(crc & 0xFFu);
    s_tx[total + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    s_tx[total + 2] = (uint8_t)((crc >> 16) & 0xFFu);
    s_tx[total + 3] = (uint8_t)((crc >> 24) & 0xFFu);

    s_io.tx(s_tx, (uint16_t)(total + OTA_FRAME_CRC_LEN));
    return 1;
}

int ota_link_ack(uint16_t cmd, uint16_t seq, uint8_t status)
{
    return ota_link_ack_ex(cmd, seq, status, 0, 0);
}

int ota_link_ack_ex(uint16_t cmd, uint16_t seq, uint8_t status,
                    const uint8_t *extra, uint16_t extra_len)
{
    uint8_t buf[OTA_FRAME_MAX_PAYLOAD - 1];
    uint16_t n = 1u;

    if (extra_len > (uint16_t)sizeof(buf))
    {
        extra_len = (uint16_t)sizeof(buf);
    }
    buf[0] = status;
    if ((extra_len > 0u) && (extra != 0))
    {
        memcpy(&buf[1], extra, extra_len);
        n = (uint16_t)(n + extra_len);
    }
    return send_frame(cmd, seq, buf, n, 1u);
}

void ota_link_set_ack_extra(const uint8_t *p, uint16_t n)
{
    if (n > (uint16_t)sizeof(s_ack_extra))
    {
        n = (uint16_t)sizeof(s_ack_extra);
    }
    if ((n > 0u) && (p != 0))
    {
        memcpy(s_ack_extra, p, n);
    }
    s_ack_extra_len = n;
}

int ota_link_report(uint16_t cmd, uint16_t seq,
                    const uint8_t *payload, uint16_t len)
{
    return send_frame(cmd, seq, payload, len, 1u);
}

int ota_link_log(const char *s)
{
    uint16_t n;
    if (s == 0)
    {
        return 0;
    }
    n = (uint16_t)strlen(s);
    if (n > OTA_FRAME_MAX_PAYLOAD)
    {
        n = OTA_FRAME_MAX_PAYLOAD;
    }
    return send_frame(CMD_LOG, 0u, (const uint8_t *)s, n, 1u);
}

/* ------------------------------------------------------------ parser -- */
static void dispatch(void)
{
    uint16_t ln, total, cmd, seq;
    uint32_t crc, got;
    uint8_t flag, st;

    ln   = rd16(&s_rx[8]);
    total = (uint16_t)(OTA_FRAME_HEADER_LEN + ln);
    got  = (uint32_t)s_rx[total + 0]
         | ((uint32_t)s_rx[total + 1] << 8)
         | ((uint32_t)s_rx[total + 2] << 16)
         | ((uint32_t)s_rx[total + 3] << 24);
    crc  = ota_crc32(s_rx, (uint32_t)total);

    if (crc == got)
    {
        ++s_rx_ok;
        flag = s_rx[3];
        cmd  = rd16(&s_rx[4]);
        seq  = rd16(&s_rx[6]);
        s_last_frame = ota_link_now();

        /* Never ACK a reply: ESP32 echoes device reports back on USB only,
         * but a miswired bridge could loop them. */
        if ((flag & OTA_FLAG_RESP) == 0u)
        {
            s_ack_extra_len = 0u;
            st = (s_cb != 0)
               ? s_cb(cmd, seq, &s_rx[OTA_FRAME_HEADER_LEN], ln)
               : (uint8_t)ST_BAD_CMD;
            if (s_ack_extra_len > 0u)
            {
                (void)ota_link_ack_ex(cmd, seq, st,
                                      s_ack_extra, s_ack_extra_len);
            }
            else
            {
                (void)ota_link_ack(cmd, seq, st);
            }
            s_ack_extra_len = 0u;
        }
    }
    /* CRC mismatch: just resync, no error frame (noise is expected). */
}

void ota_link_feed_byte(uint8_t b)
{
    if (s_need == 0u)
    {
        /* hunting for AA 55 */
        if (s_pos == 0u)
        {
            if (b == OTA_MAGIC0) { s_rx[0] = b; s_pos = 1u; }
        }
        else
        {
            if (b == OTA_MAGIC1)
            {
                s_rx[1] = b;
                s_pos   = 2u;
                s_need  = (uint16_t)OTA_FRAME_HEADER_LEN;
            }
            else
            {
                s_pos = (b == OTA_MAGIC0) ? 1u : 0u;
                if (s_pos) { s_rx[0] = b; }
            }
        }
        return;
    }

    s_rx[s_pos++] = b;
    s_rx_last_ms = ota_link_now();

    if (s_pos == OTA_FRAME_HEADER_LEN)
    {
        uint16_t ln = rd16(&s_rx[8]);
        if ((s_rx[2] != OTA_PROTO_VER) || (ln > OTA_FRAME_MAX_PAYLOAD))
        {
            /* bad header: keep the last byte as a possible new MAGIC0 */
            s_pos  = (b == OTA_MAGIC0) ? 1u : 0u;
            s_need = 0u;
            if (s_pos) { s_rx[0] = b; }
            return;
        }
        s_need = (uint16_t)(OTA_FRAME_HEADER_LEN + ln + OTA_FRAME_CRC_LEN);
    }

    if (s_pos >= s_need)
    {
        dispatch();
        s_pos  = 0u;
        s_need = 0u;
    }
}

void ota_link_feed(const uint8_t *p, uint16_t n)
{
    uint16_t i;
    for (i = 0u; i < n; ++i)
    {
        ota_link_feed_byte(p[i]);
    }
}

void ota_link_poll(void)
{
    uint8_t b;
    uint16_t budget = 2048u;   /* do not starve the rest of the firmware */

    if (s_io.rx == 0)
    {
        return;
    }
    while (budget > 0u)
    {
        if (s_io.rx(&b) == 0)
        {
            break;
        }
        ota_link_feed_byte(b);
        --budget;
    }
    /* 半帧超时重同步。
     *
     * 为什么必须有：擦写 flash 时 CPU 取指会被长时间挂起（擦 7 个 128KB 扇区
     * 要 7~14 秒），这期间 UART 还在收字节，DMA 环形缓冲会覆盖、丢字节。
     * 一旦丢的位置正好在帧头之后，s_need 就会指向一个永远凑不齐的长度，
     * 之后所有正常帧都被当成这半截帧的 payload 吃掉 —— 现象就是
     * 「TX 心跳照发、RX 永远不回 ACK」，只能靠复位恢复。
     * 帧内字节间隔远小于 1ms（921600 波特率），半帧停顿 500ms 一定是丢了，
     * 直接丢弃重来。 */
    if (((s_pos != 0u) || (s_need != 0u)) &&
        ((ota_link_now() - s_rx_last_ms) > 500u))
    {
        s_pos  = 0u;
        s_need = 0u;
    }

}
