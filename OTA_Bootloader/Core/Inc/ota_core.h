/**
 * @file  ota_core.h
 * @brief Shared OTA session state machine for STM32 APP and STM32 Bootloader.
 *
 *  Both sides speak exactly the same 0xAA55 frames. The APP receives an
 *  upgrade while running and writes the INACTIVE slot; the Bootloader offers
 *  the same handler during its rescue window so a bricked APP can be fixed.
 *  This file is a byte-identical copy in both projects.
 *
 *  Keep all comments ASCII: Keil AC5 is used by both projects.
 */
#ifndef OTA_CORE_H
#define OTA_CORE_H

#include <stdint.h>
#include "ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* APP builds define OTA_ACTIVE_SLOT=0 / 1 through the Keil target's
 * preprocessor defines. Bootloader passes OTA_SLOT_NONE explicitly. */
#ifndef OTA_ACTIVE_SLOT
#define OTA_ACTIVE_SLOT   OTA_SLOT_A
#endif

#define OTA_SESSION_TIMEOUT_MS   60000u
#define OTA_STATUS_PERIOD_MS     2000u

typedef struct
{
    uint8_t  active_slot;                 /* OTA_SLOT_NONE for Bootloader */
    uint32_t (*millis)(void);
    void     (*reset_now)(void);          /* called from ota_core_poll()  */
    uint32_t reset_delay_ms;              /* let the ACK go out first     */
} ota_core_cfg_t;

void     ota_core_init(const ota_core_cfg_t *cfg);

/* ota_link handler: returns ST_*, the link layer sends the matching ACK. */
uint8_t  ota_core_on_frame(uint16_t cmd, uint16_t seq,
                           const uint8_t *payload, uint16_t len);

/* Call from the main loop / OtaTask: deferred reset, session timeout,
 * periodic status reports. */
void     ota_core_poll(void);

void     ota_core_get_status(ota_status_t *st);
uint8_t  ota_core_active_slot(void);
uint8_t  ota_core_is_idle(void);

/* APP only: clear the trial flag once this firmware booted successfully. */
int      ota_core_confirm_app(void);

/* Bootloader only: choose a validated slot, apply trial / rollback rules.
 * Returns OTA_SLOT_A / OTA_SLOT_B, or OTA_SLOT_NONE if nothing is bootable. */
uint8_t  ota_core_pick_boot_slot(void);

/* Copy running firmware version into out[] (NUL terminated). */
void     ota_core_copy_version(char *out, uint32_t cap);

uint32_t ota_core_slot_base(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CORE_H */
