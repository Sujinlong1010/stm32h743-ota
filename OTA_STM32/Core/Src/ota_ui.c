/**
 * @file  ota_ui.c
 * @brief Direct framebuffer status screen for the STM32 APP (no LVGL).
 *        Strict C90 for Keil AC5.
 */
#include "ota_ui.h"
#include "ota_core.h"
#include "ota_uart.h"
#include "ota_link.h"
#include "ltdc.h"
#include <stdint.h>

#ifndef OTA_APP_VERSION
#define OTA_APP_VERSION "1.0.0-DEV"
#endif

/* 双缓冲：CPU 只画后台缓冲，画完把 LTDC 的图层基址切过去。
 * 单缓冲时 CPU 写 SDRAM 与 LTDC 读 SDRAM 抢同一块区域，用户会看到
 * 每 300 ms 一次的"重绘闪烁"。两块缓冲各占 1024*600*2 = 1.2 MB，
 * 放在 SDRAM 0xC0000000 起的 2 MB 对齐处，互不重叠。 */
#define UI_FB_ADDR0     0xC0000000u
#define UI_FB_ADDR1     0xC0200000u
#define UI_W            1024u
#define UI_H            600u

#define COL_BG          0x1083u
#define COL_HEAD        0x1A5Fu
#define COL_CARD        0x2124u
#define COL_BAR_BG      0x39C7u
#define COL_WHITE       0xFFFFu
#define COL_GREY        0x8410u
#define COL_GREEN       0x07E0u
#define COL_RED         0xF800u
#define COL_YELLOW      0xFFE0u
#define COL_CYAN        0x07FFu

typedef struct
{
    char    ch;
    uint8_t col[5];
} ui_glyph_t;

/* Standard column-major 5x7 bitmap font, subset actually used by the UI. */
static const ui_glyph_t FONT[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'0', {0x3E,0x51,0x49,0x45,0x3E}},
    {'1', {0x00,0x42,0x7F,0x40,0x00}},
    {'2', {0x42,0x61,0x51,0x49,0x46}},
    {'3', {0x21,0x41,0x45,0x4B,0x31}},
    {'4', {0x18,0x14,0x12,0x7F,0x10}},
    {'5', {0x27,0x45,0x45,0x45,0x39}},
    {'6', {0x3C,0x4A,0x49,0x49,0x30}},
    {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}},
    {'9', {0x06,0x49,0x49,0x29,0x1E}},
    {'A', {0x7E,0x11,0x11,0x11,0x7E}},
    {'B', {0x7F,0x49,0x49,0x49,0x36}},
    {'C', {0x3E,0x41,0x41,0x41,0x22}},
    {'D', {0x7F,0x41,0x41,0x22,0x1C}},
    {'E', {0x7F,0x49,0x49,0x49,0x41}},
    {'F', {0x7F,0x09,0x09,0x09,0x01}},
    {'G', {0x3E,0x41,0x49,0x49,0x7A}},
    {'H', {0x7F,0x08,0x08,0x08,0x7F}},
    {'I', {0x00,0x41,0x7F,0x41,0x00}},
    {'J', {0x20,0x40,0x41,0x3F,0x01}},
    {'K', {0x7F,0x08,0x14,0x22,0x41}},
    {'L', {0x7F,0x40,0x40,0x40,0x40}},
    {'M', {0x7F,0x02,0x0C,0x02,0x7F}},
    {'N', {0x7F,0x04,0x08,0x10,0x7F}},
    {'O', {0x3E,0x41,0x41,0x41,0x3E}},
    {'P', {0x7F,0x09,0x09,0x09,0x06}},
    {'Q', {0x3E,0x41,0x51,0x21,0x5E}},
    {'R', {0x7F,0x09,0x19,0x29,0x46}},
    {'S', {0x46,0x49,0x49,0x49,0x31}},
    {'T', {0x01,0x01,0x7F,0x01,0x01}},
    {'U', {0x3F,0x40,0x40,0x40,0x3F}},
    {'V', {0x1F,0x20,0x40,0x20,0x1F}},
    {'W', {0x3F,0x40,0x38,0x40,0x3F}},
    {'X', {0x63,0x14,0x08,0x14,0x63}},
    {'Y', {0x07,0x08,0x70,0x08,0x07}},
    {'Z', {0x61,0x51,0x49,0x45,0x43}},
    {'.', {0x00,0x60,0x60,0x00,0x00}},
    {',', {0x00,0x50,0x30,0x00,0x00}},
    {':', {0x00,0x36,0x36,0x00,0x00}},
    {'/', {0x20,0x10,0x08,0x04,0x02}},
    {'-', {0x08,0x08,0x08,0x08,0x08}},
    {'_', {0x40,0x40,0x40,0x40,0x40}},
    {'%', {0x23,0x13,0x08,0x64,0x62}},
    {'=', {0x14,0x14,0x14,0x14,0x14}},
    {'+', {0x08,0x08,0x3E,0x08,0x08}},
    {'>', {0x41,0x22,0x14,0x08,0x00}},
    {'<', {0x08,0x14,0x22,0x41,0x00}},
    {'(', {0x00,0x1C,0x22,0x41,0x00}},
    {')', {0x00,0x41,0x22,0x1C,0x00}},
    {'!', {0x00,0x00,0x5F,0x00,0x00}},
    {'?', {0x02,0x01,0x51,0x09,0x06}},
    {'*', {0x14,0x08,0x3E,0x08,0x14}}
};

#define UI_FB0 ((volatile uint16_t *)UI_FB_ADDR0)
#define UI_FB1 ((volatile uint16_t *)UI_FB_ADDR1)

/* ui_fb 指向"正在画的那块"，ui_back 也指向它；画完 flip 到另一块。 */
static volatile uint16_t *ui_fb   = UI_FB0;
static volatile uint16_t *ui_back = UI_FB1;
static uint32_t           ui_last_key = 0xFFFFFFFFu;

static void ota_ui_paint(uint8_t force);

/* 内容指纹：只有真正变化时才重画，静止时一个字节都不写显存。 */
static uint32_t ui_state_key(const ota_status_t *st)
{
    uint32_t k = 2166136261u;

    k = (k ^ (uint32_t)st->stage)       * 16777619u;
    k = (k ^ (uint32_t)st->result)      * 16777619u;
    k = (k ^ (uint32_t)st->progress)    * 16777619u;
    k = (k ^ (uint32_t)st->active_slot) * 16777619u;
    k = (k ^ st->detail)                * 16777619u;
    return k;
}

static void ui_pixel(uint32_t x, uint32_t y, uint16_t c)
{
    if ((x < UI_W) && (y < UI_H))
    {
        ui_fb[y * UI_W + x] = c;
    }
}

static void ui_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t ix, iy;
    for (iy = 0u; iy < h; ++iy)
    {
        for (ix = 0u; ix < w; ++ix)
        {
            ui_pixel(x + ix, y + iy, c);
        }
    }
}

static const uint8_t *ui_glyph(char ch)
{
    uint32_t i;
    char c = ch;

    if ((c >= 'a') && (c <= 'z'))
    {
        c = (char)(c - ('a' - 'A'));
    }
    for (i = 0u; i < sizeof(FONT) / sizeof(FONT[0]); ++i)
    {
        if (FONT[i].ch == c)
        {
            return FONT[i].col;
        }
    }
    return FONT[0].col;
}

static uint32_t ui_text(uint32_t x, uint32_t y, const char *s,
                        uint16_t c, uint32_t scale)
{
    uint32_t cx = x;
    uint32_t col, row, sx, sy;
    const uint8_t *g;

    if (s == 0)
    {
        return x;
    }
    while (*s != '\0')
    {
        g = ui_glyph(*s);
        for (col = 0u; col < 5u; ++col)
        {
            for (row = 0u; row < 7u; ++row)
            {
                if ((g[col] & (uint8_t)(1u << row)) != 0u)
                {
                    for (sy = 0u; sy < scale; ++sy)
                    {
                        for (sx = 0u; sx < scale; ++sx)
                        {
                            ui_pixel(cx + col * scale + sx,
                                     y + row * scale + sy, c);
                        }
                    }
                }
            }
        }
        cx += 6u * scale;
        ++s;
    }
    return cx;
}

static void ui_itoa(uint32_t v, char *out)
{
    char tmp[12];
    uint32_t i = 0u, j = 0u;

    if (v == 0u)
    {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v > 0u)
    {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0u)
    {
        out[j++] = tmp[--i];
    }
    out[j] = '\0';
}

static const char *ui_stage(uint8_t s)
{
    switch (s)
    {
    case OTA_STAGE_RECEIVING:       return "RECEIVING";
    case OTA_STAGE_VERIFYING:       return "VERIFYING";
    case OTA_STAGE_WRITING:         return "WRITING";
    case OTA_STAGE_PENDING_REBOOT:  return "PENDING REBOOT";
    case OTA_STAGE_SUCCESS:         return "SUCCESS";
    case OTA_STAGE_FAILED:          return "FAILED";
    case OTA_STAGE_ROLLED_BACK:     return "ROLLED BACK";
    default:                        return "IDLE";
    }
}

static const char *ui_result(uint8_t r)
{
    switch (r)
    {
    case ST_OK:           return "OK";
    case ST_BUSY:         return "BUSY";
    case ST_BAD_CRC:      return "BAD CRC";
    case ST_BAD_SEQ:      return "BAD SEQ";
    case ST_BAD_CMD:      return "BAD CMD";
    case ST_ERASE_FAIL:   return "ERASE FAIL";
    case ST_WRITE_FAIL:   return "WRITE FAIL";
    case ST_VERIFY_FAIL:  return "VERIFY FAIL";
    case ST_SLOT_BUSY:    return "RUNNING SLOT";
    case ST_TIMEOUT:      return "TIMEOUT";
    case ST_NO_SPACE:     return "NO SPACE";
    case ST_ROLLBACK:     return "ROLLBACK";
    case ST_BAD_HEADER:   return "BAD HEADER";
    default:              return "UNKNOWN";
    }
}

/* 画到"当前 ui_fb 指向的缓冲"：底色、标题栏、8 色彩条。 */
static void ui_paint_static(void)
{
    static const uint16_t bars[8] =
        {0xF800u, 0x07E0u, 0x001Fu, 0xFFE0u,
         0x07FFu, 0xF81Fu, 0xFFFFu, 0x0000u};
    uint32_t b;

    ui_rect(0u, 0u, UI_W, UI_H, COL_BG);
    ui_rect(0u, 0u, UI_W, 78u, COL_HEAD);
    ui_rect(0u, 76u, UI_W, 2u, COL_CYAN);
    (void)ui_text(40u, 14u, "STM32H743 REMOTE OTA", COL_WHITE, 4u);
    (void)ui_text(42u, 50u, "ONENET PASS-THROUGH / SELF-BUILT PROTOCOL",
                  COL_GREY, 2u);

    /* Keep the 8-colour test strip: it is the on-screen proof that the
       LTDC_G6 / PI1 pin fix is still good after every CubeMX regeneration. */
    for (b = 0u; b < 8u; ++b)
    {
        ui_rect(b * 128u, 78u, 128u, 20u, bars[b]);
    }
}

void ota_ui_init(void)
{
    /* 两块缓冲都画一份静态部分，之后 flip 到哪一块顶部都是完整的。 */
    ui_fb = UI_FB0;
    ui_paint_static();
    ui_fb = UI_FB1;
    ui_paint_static();

    ui_back = UI_FB1;
    ota_ui_paint(1u);          /* 首次强制完整绘制并切到 1 号缓冲 */
}

void ota_ui_refresh(void)
{
    ota_ui_paint(0u);
}

static void ota_ui_paint(uint8_t force)
{
    ota_status_t st;
    char ver[24];
    char buf[40];
    char rxbuf[32];
    uint16_t accent;
    uint32_t bar_w, k, key;
    uint32_t rxb = 0u;
    uint32_t rxf;
    const char *shown;

    ota_core_get_status(&st);
    ota_uart_get_rx_bytes(&rxb);       /* DMA 真正搬出来的字节数 */
    rxf = ota_link_rx_ok();            /* CRC 通过的帧数 */

    /* 接收计数必须参与 key，否则它涨了屏幕也不重画（内容是"没变"）。 */
    key = (ui_state_key(&st) ^ rxb) * 16777619u;
    key = (key ^ rxf) * 16777619u;
    if ((force == 0u) && (key == ui_last_key))
    {
        return;                /* 内容没变：一个字节都不写，屏幕完全静止 */
    }
    ui_last_key = key;

    ui_fb = ui_back;           /* 只画后台缓冲，前台继续稳定显示旧帧 */

    ota_core_copy_version(ver, (uint32_t)sizeof(ver));
    shown = (ver[0] != '\0') ? (const char *)ver : OTA_APP_VERSION;

    accent = (st.result == ST_OK) ? COL_GREEN : COL_RED;
    if ((st.stage == OTA_STAGE_RECEIVING) ||
        (st.stage == OTA_STAGE_WRITING) ||
        (st.stage == OTA_STAGE_VERIFYING))
    {
        accent = COL_YELLOW;
    }

    ui_rect(40u, 100u, 944u, 450u, COL_CARD);
    ui_rect(40u, 100u, 6u, 450u, accent);

    (void)ui_text(70u, 126u, "FIRMWARE VERSION", COL_GREY, 2u);
    (void)ui_text(70u, 160u, shown, COL_WHITE, 4u);

    (void)ui_text(70u, 226u, "ACTIVE SLOT", COL_GREY, 2u);
    buf[0] = (ota_core_active_slot() == OTA_SLOT_B) ? 'B' : 'A';
    buf[1] = '\0';
    (void)ui_text(70u, 258u, buf, COL_CYAN, 6u);

    (void)ui_text(560u, 226u, "LINK / BAUD", COL_GREY, 2u);
    (void)ui_text(560u, 258u, "USART1 921600", COL_WHITE, 2u);
    (void)ui_text(560u, 296u, "PB14 TX / PB15 RX", COL_GREY, 2u);

    /* 实时接收计数。这一行是判断「PB15 到底有没有进来数据」的唯一现场证据，
     * 也是本次 UART 排查的关键仪器：
     *   RX 一直是 0 且不涨  -> PB15 上从来没有电平（线没接/接错针/线断）
     *   字节涨但帧数 0      -> 有电平但解码不出来（波特率或干扰）
     *   两个都涨            -> STM32 收发都正常，问题不在这里 */
    {
        uint32_t p = 0u, q = 0u;

        ui_itoa(rxb, rxbuf);
        while ((rxbuf[p] >= '0') && (rxbuf[p] <= '9')) { ++p; }
        rxbuf[p++] = 'B';
        rxbuf[p++] = ' ';
        rxbuf[p++] = '/';
        rxbuf[p++] = ' ';
        rxbuf[p] = '\0';
        ui_itoa(rxf, &rxbuf[p]);
        while ((rxbuf[p + q] >= '0') && (rxbuf[p + q] <= '9')) { ++q; }
        rxbuf[p + q] = 'F';
        rxbuf[p + q + 1u] = '\0';
        (void)ui_text(560u, 314u, rxbuf, COL_WHITE, 2u);
    }

    (void)ui_text(70u, 340u, "OTA STAGE", COL_GREY, 2u);
    (void)ui_text(70u, 372u, ui_stage(st.stage), accent, 3u);

    (void)ui_text(560u, 340u, "RESULT", COL_GREY, 2u);
    (void)ui_text(560u, 372u, ui_result(st.result), accent, 3u);

    ui_rect(70u, 436u, 884u, 40u, COL_BAR_BG);
    bar_w = ((uint32_t)st.progress * 884u) / 100u;
    if (bar_w > 0u)
    {
        ui_rect(70u, 436u, bar_w, 40u, accent);
    }

    ui_itoa((uint32_t)st.progress, buf);
    k = 0u;
    while ((buf[k] >= '0') && (buf[k] <= '9'))
    {
        ++k;
    }
    buf[k] = '%';
    buf[k + 1u] = '\0';
    (void)ui_text(70u, 490u, buf, COL_WHITE, 4u);
    (void)ui_text(240u, 490u, "BYTES", COL_GREY, 2u);
    ui_itoa(st.detail, buf);
    (void)ui_text(340u, 490u, buf, COL_WHITE, 3u);

    (void)ui_text(70u, 560u, "A/B DUAL BANK  +  TRIAL BOOT  +  AUTO ROLLBACK",
                  COL_GREY, 2u);

    /* 画完再切基址：LTDC 下一帧开始从新缓冲取数，看到的永远是完整画面。 */
    __DSB();
    (void)HAL_LTDC_SetAddress(&hltdc, (uint32_t)ui_back, 0u);
    ui_back = (ui_back == UI_FB1) ? UI_FB0 : UI_FB1;
}
