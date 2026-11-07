/**
 * @file  ota_ui.h
 * @brief Lightweight status screen drawn directly into the LTDC framebuffer.
 *
 *  APP-only. No LVGL dependency: the project already owns a 1024x600
 *  RGB565 framebuffer at 0xC0000000 in SDRAM, so a tiny 5x7 bitmap font
 *  and a few rectangles are enough, and it always builds with AC5/C90.
 */
#ifndef OTA_UI_H
#define OTA_UI_H

#ifdef __cplusplus
extern "C" {
#endif

void ota_ui_init(void);
void ota_ui_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_UI_H */
