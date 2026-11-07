/**
 * @file  ota_uart.h
 * @brief CubeMX huart1 + DMA-Circular + IDLE adapter for the frame layer.
 *
 *  Shared byte-identical copy in OTA_STM32 and OTA_Bootloader. Both projects
 *  have CubeMX-generated huart1 (PB14/PB15, AF4, 921600) and a circular
 *  DMA1_Stream0 RX, so the very same adapter works for bare metal and RTOS.
 *
 *  ISR only copies bytes into a software ring; frame parsing / flash writes
 *  happen in ota_uart_poll() from the main loop or OtaTask.
 *
 *  NOTE: neither project enables D-Cache. If you ever call
 *  SCB_EnableDCache(), invalidate s_dma_buf before reading it in the ISR.
 */
#ifndef OTA_UART_H
#define OTA_UART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     ota_uart_init(void);
void     ota_uart_deinit(void);
void     ota_uart_poll(void);
uint32_t ota_uart_millis(void);
/* 累计从 DMA 搬出的字节数（纯电气层证据，屏幕显示用）。 */
void     ota_uart_get_rx_bytes(uint32_t *bytes);

/* Call from USART1_IRQHandler USER CODE when UART_FLAG_IDLE is set. */
void     ota_uart_idle_isr(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_UART_H */
