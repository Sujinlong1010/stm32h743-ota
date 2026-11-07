/**
 * @file  ota_link.h
 * @brief Shared frame layer for STM32 Bootloader and STM32 APP.
 *
 *  One implementation, two users. Byte I/O is injected through ota_link_io_t
 *  so Bootloader (polled UART) and APP (DMA + IDLE UART) stay independent.
 *
 *  Rules that MUST match OTA_PC_Tool/ota_framer.py:
 *   - ACK uses the SAME cmd and the SAME seq as the request, FLAG |= RESP.
 *   - ACK payload[0] is the ST_* status code.
 *   - Frames with FLAG.RESP already set are never ACKed (loop prevention).
 *   - Any non-frame byte stream (ESP32 USB debug text) is skipped by
 *     re-synchronising on 0xAA 0x55.
 *
 *  Keep all comments ASCII: Keil AC5 is used by both STM32 projects.
 */
#ifndef OTA_LINK_H
#define OTA_LINK_H

#include <stdint.h>
#include "ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    void     (*tx)(const uint8_t *p, uint16_t n);
    int      (*rx)(uint8_t *b);        /* 1 = one byte returned, 0 = empty */
    uint32_t (*millis)(void);
} ota_link_io_t;

/* Return the ST_* code. The link layer sends the matching ACK afterwards.
 * A handler may additionally call ota_link_report()/ota_link_log(). */
typedef uint8_t (*ota_link_handler_t)(uint16_t cmd, uint16_t seq,
                                      const uint8_t *payload, uint16_t len);

void     ota_link_init(const ota_link_io_t *io, ota_link_handler_t cb);
void     ota_link_poll(void);
uint32_t ota_link_now(void);
uint32_t ota_link_last_frame_time(void);
/* 累计收到的合法帧数（CRC 通过才算）。屏幕显示用。 */
uint32_t ota_link_rx_ok(void);

int ota_link_ack(uint16_t cmd, uint16_t seq, uint8_t status);
int ota_link_ack_ex(uint16_t cmd, uint16_t seq, uint8_t status,
                    const uint8_t *extra, uint16_t extra_len);

/* A handler may attach data to the ACK that the link layer will send after
 * it returns. Data is COPIED, so stack-local structs are safe. Cleared
 * automatically after each dispatch. */
void ota_link_set_ack_extra(const uint8_t *p, uint16_t n);
#define OTA_LINK_ACK_EXTRA_MAX   64u
int ota_link_report(uint16_t cmd, uint16_t seq,
                    const uint8_t *payload, uint16_t len);
int ota_link_log(const char *s);

/* Feed one byte manually (used when a DMA/IDLE ISR already has a buffer). */
void ota_link_feed_byte(uint8_t b);
void ota_link_feed(const uint8_t *p, uint16_t n);

#ifdef __cplusplus
}
#endif

#endif /* OTA_LINK_H */
