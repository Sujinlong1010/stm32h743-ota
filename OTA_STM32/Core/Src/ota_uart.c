/**
 * @file  ota_uart.c
 * @brief huart1 + DMA-Circular + IDLE adapter, shared by APP and Bootloader.
 *        Strict C90 for Keil AC5.
 */
#include "ota_uart.h"
#include "ota_link.h"
#include "ota_core.h"
#include "usart.h"
#include "stm32h7xx_hal.h"

extern DMA_HandleTypeDef hdma_usart1_rx;

#define OTA_UART_DMA_BUF_SIZE   2048u
#define OTA_UART_RING_SIZE      4096u

/* 32-byte aligned so a future D-Cache invalidation stays legal. */
static uint8_t  s_dma_buf[OTA_UART_DMA_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t  s_ring[OTA_UART_RING_SIZE];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;
static volatile uint16_t s_last_pos;

/* 接收统计：给屏幕用。s_rx_bytes 是「DMA 里真正搬出来的字节数」——
 * 它能不涨，就说明 PB15 这根线上一个电平都没来过，是纯物理层问题；
 * 它涨而帧数不涨，才是波特率/干扰/协议问题。 */
static volatile uint32_t s_rx_bytes;

void ota_uart_get_rx_bytes(uint32_t *bytes)
{
    if (bytes != 0)
    {
        *bytes = s_rx_bytes;
    }
}

uint32_t ota_uart_millis(void)
{
    return HAL_GetTick();
}

static void uart_tx(const uint8_t *p, uint16_t n)
{
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)p, n, 300u);
}

static void ring_push_byte(uint8_t b)
{
    uint16_t next = (uint16_t)((s_head + 1u) & (OTA_UART_RING_SIZE - 1u));
    if (next == s_tail)
    {
        return;                     /* ring full: drop oldest byte */
    }
    s_ring[s_head] = b;
    s_head = next;
}

static void ring_push(const uint8_t *p, uint16_t n)
{
    uint16_t i;
    for (i = 0u; i < n; ++i)
    {
        ring_push_byte(p[i]);
    }
}

static int ring_pop(uint8_t *b)
{
    if (s_head == s_tail)
    {
        return 0;
    }
    *b = s_ring[s_tail];
    s_tail = (uint16_t)((s_tail + 1u) & (OTA_UART_RING_SIZE - 1u));
    return 1;
}

void ota_uart_idle_isr(void)
{
    uint16_t ndtr = (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
    uint16_t pos  = (uint16_t)(OTA_UART_DMA_BUF_SIZE - ndtr);

    if (pos >= OTA_UART_DMA_BUF_SIZE)
    {
        pos = 0u;
    }
    if (pos == s_last_pos)
    {
        return;
    }
    if (pos > s_last_pos)
    {
        ring_push(&s_dma_buf[s_last_pos], (uint16_t)(pos - s_last_pos));
        s_rx_bytes += (uint32_t)(pos - s_last_pos);
    }
    else
    {
        ring_push(&s_dma_buf[s_last_pos],
                  (uint16_t)(OTA_UART_DMA_BUF_SIZE - s_last_pos));
        ring_push(&s_dma_buf[0], pos);
        s_rx_bytes += (uint32_t)(OTA_UART_DMA_BUF_SIZE - s_last_pos) + (uint32_t)pos;
    }
    s_last_pos = pos;
}

void ota_uart_init(void)
{
    ota_link_io_t io;

    s_head     = 0u;
    s_tail     = 0u;
    s_last_pos = 0u;
    s_rx_bytes = 0u;

    io.tx     = uart_tx;
    io.rx     = ring_pop;
    io.millis = ota_uart_millis;
    ota_link_init(&io, ota_core_on_frame);

    __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
    (void)HAL_UART_Receive_DMA(&huart1, s_dma_buf, OTA_UART_DMA_BUF_SIZE);
}

void ota_uart_deinit(void)
{
    (void)HAL_UART_DMAStop(&huart1);
    __HAL_UART_DISABLE_IT(&huart1, UART_IT_IDLE);
    (void)HAL_UART_DeInit(&huart1);
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    HAL_NVIC_DisableIRQ(DMA1_Stream0_IRQn);
}

void ota_uart_poll(void)
{
    ota_link_poll();
}
