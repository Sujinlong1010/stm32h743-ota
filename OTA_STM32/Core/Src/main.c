/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "dma.h"
#include "dma2d.h"
#include "ltdc.h"
#include "usart.h"
#include "gpio.h"
#include "fmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ota_uart.h"
#include "ota_core.h"
#include "ota_ui.h"
#include "ota_link.h"
#include "ota_flash.h"
#include <string.h>          /* memset，仅开机自检用 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void app_reset_now(void)
{
    NVIC_SystemReset();
}

/* 开机自报身份：把「我是哪个槽的固件、链接基址是多少」通过 UART 发一行。
 *
 * 为什么要这个：屏幕上的 ACTIVE SLOT 字母来自编译期宏 OTA_ACTIVE_SLOT，
 * 它只能证明「这版固件是给哪个槽编译的」，证明不了「它现在被放在哪个槽跑」。
 * 而 Bootloader 在某个槽校验不过时会自动切到另一个槽，很容易出现
 * 「我烧的是 A，屏幕上却是 B」这种对不上账的情况。
 * 这一行带基址，串口那头一眼就能对账：
 *     [STM32] APP: slot A (base 0x08020000)
 * OTA 升级完成后也靠它确认到底跳到了哪个槽。
 * 手写十六进制而不用 snprintf：本工程没开 Microlib，为一行日志拖进
 * 整个 printf 家族不划算。 */
/* 小工具：把十进制写进字符串，避免为一个日志行引入 snprintf。 */
static uint32_t app_put_u32(char *s, uint32_t i, uint32_t v)
{
    char t[12];
    uint32_t n = 0u, k;

    if (v == 0u)
    {
        s[i++] = '0';
        return i;
    }
    while (v > 0u)
    {
        t[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (k = 0u; k < n; ++k)
    {
        s[i++] = t[n - 1u - k];
    }
    return i;
}

/* 开机自检两个槽，把「能不能被 Bootloader 认下来」和「失败原因码」发出去。
 *
 * 这一行是为了解一个矛盾：升级结束时 APP 自己 verify 是通过的，
 * 但重启后 Bootloader 却回滚到旧槽。原因码含义：
 *   1=读不到固件头  2=头字段非法  3=SP 非法  4=PC 不在镜像内  5=CRC 不符 */
static void app_log_slot_verify(void)
{
    char s[40];
    uint32_t i = 0u;
    int va, vb;

    va = ota_flash_verify_slot(OTA_SLOT_A, 0);
    s[i++] = 'I'; s[i++] = 'M'; s[i++] = 'G'; s[i++] = ':';
    s[i++] = ' ';
    s[i++] = 'A'; s[i++] = '=';
    i = app_put_u32(s, i, (va) ? 1u : 0u);
    s[i++] = '/';
    i = app_put_u32(s, i, (uint32_t)ota_flash_verify_reason());

    vb = ota_flash_verify_slot(OTA_SLOT_B, 0);
    s[i++] = ' ';
    s[i++] = 'B'; s[i++] = '=';
    i = app_put_u32(s, i, (vb) ? 1u : 0u);
    s[i++] = '/';
    i = app_put_u32(s, i, (uint32_t)ota_flash_verify_reason());
    s[i] = '\0';

    (void)ota_link_log(s);
}

/* 追加一段字面量到 s。手写而不用 strcat，避免引入 string 库调用。 */
static uint32_t app_put_str(char *s, uint32_t i, const char *t)
{
    while (*t != '\0') { s[i++] = *t; ++t; }
    return i;
}

static uint32_t app_put_hex32(char *s, uint32_t i, uint32_t v)
{
    static const char HD[] = "0123456789ABCDEF";
    uint32_t k;

    for (k = 0u; k < 8u; ++k)
    {
        s[i++] = HD[(v >> ((7u - k) * 4u)) & 0xFu];
    }
    return i;
}

/* 把 B 槽固件头的原始字段和「重新算一遍」的 CRC 一起打出来。
 *
 * 原因码只能说明「5 = CRC 不符」，看不出根因，而三种根因的修法完全不同：
 *   - 头里 fw_size 记错了   -> 这里的 n 就是错的
 *   - 内容被擦坏/少写几片   -> n 对，但 calc != c
 *   - 头根本没写进去        -> m=FFFFFFFF n=4294967295
 */
static void app_log_b_slot_header(void)
{
    ota_fw_header_t hdr;
    char s[110];
    uint32_t i;
    uint32_t calc;
    int      same;

    memset(&hdr, 0, sizeof(hdr));
    (void)ota_flash_read_fw_header(OTA_SLOT_B, &hdr);

    i = 0u;
    i = app_put_str(s, i, "HDRB: m=");
    i = app_put_hex32(s, i, hdr.magic);
    i = app_put_str(s, i, " h=");
    i = app_put_u32(s, i, hdr.header_size);
    i = app_put_str(s, i, " n=");
    i = app_put_u32(s, i, hdr.fw_size);
    i = app_put_str(s, i, " s=");
    i = app_put_u32(s, i, (uint32_t)hdr.slot);
    i = app_put_str(s, i, " c=");
    i = app_put_hex32(s, i, hdr.fw_crc32);
    s[i] = '\0';
    (void)ota_link_log(s);

    /* 长度非法时算 CRC 会把整个 896KB 读一遍（要好几秒），跳过。 */
    i = 0u;
    i = app_put_str(s, i, "CALC: ");
    if ((hdr.fw_size == 0u) || (hdr.fw_size > OTA_FW_MAX_SIZE))
    {
        i = app_put_str(s, i, "skip (bad size)");
    }
    else
    {
        calc = ota_crc32((const uint8_t *)OTA_SLOT_ADDR(OTA_SLOT_B), hdr.fw_size);
        same = (calc == hdr.fw_crc32) ? 1 : 0;
        i = app_put_hex32(s, i, calc);
        i = app_put_str(s, i, (same != 0) ? " MATCH" : " MISMATCH");
    }
    s[i] = '\0';
    (void)ota_link_log(s);
}

/* 打出 Bootloader 会读到的元数据。
 *
 * 用来区分两种「重启后还停在 A」：
 *   1) 元数据说 active=A 且 trial=255(none) -> B 根本没被尝试，问题在元数据写入
 *   2) 元数据说 trial=B                      -> B 被尝试了但校验失败，问题在 verify
 * 这两种现象的串口表现一模一样，只能靠这行数据分开。 */
static void app_log_meta(void)
{
    ota_meta_t m;
    char s[64];
    uint32_t i;

    memset(&m, 0, sizeof(m));
    if (!ota_meta_load(&m))
    {
        (void)ota_link_log("META: load failed");
        return;
    }
    i = 0u;
    i = app_put_str(s, i, "META: act=");
    i = app_put_u32(s, i, (uint32_t)m.active_slot);
    i = app_put_str(s, i, " trial=");
    i = app_put_u32(s, i, (uint32_t)m.trial_slot);
    i = app_put_str(s, i, " try=");
    i = app_put_u32(s, i, (uint32_t)m.boot_try);
    i = app_put_str(s, i, " res=");
    i = app_put_u32(s, i, (uint32_t)m.last_result);
    s[i] = '\0';
    (void)ota_link_log(s);
}

/* 本镜像实际被链接到了哪个槽。
 *
 * 为什么不能只信 OTA_ACTIVE_SLOT 宏：APP_A 和 APP_B 两个 Keil 目标共用同一个
 * 输出目录（MDK-ARM\OTA_STM32\），增量构建时 APP_B 会直接复用 APP_A 编译出的
 * .o，于是「B 的镜像」里 OTA_ACTIVE_SLOT 仍然是 0。后果是固件明明跑在 B 槽，
 * 却自认为在 A：
 *   1) 上报 active_slot=0，上位机永远等不到「已切到 B」的确认；
 *   2) ota_core_confirm_app() 拿 trial=B 和 active=A 比较必然不相等，
 *      试用标记永远清不掉，3 次之后 Bootloader 就回滚。
 * 链接地址不会骗人：镜像被链接到 0x08100000 时，任何函数地址都 >= 0x08100000。
 */
static uint8_t app_link_slot(void)
{
    uint32_t here = (uint32_t)(void *)&app_link_slot;

    return (here >= OTA_SLOT_B_ADDR) ? (uint8_t)OTA_SLOT_B : (uint8_t)OTA_SLOT_A;
}

static void app_log_boot_identity(void)
{
    static const char HD[] = "0123456789ABCDEF";
    char s[44];
    uint8_t  slot = app_link_slot();
    uint32_t base = OTA_SLOT_ADDR(slot);
    uint32_t i = 0u, k;

    s[i++] = 'A'; s[i++] = 'P'; s[i++] = 'P'; s[i++] = ':';
    s[i++] = ' ';
    s[i++] = 's'; s[i++] = 'l'; s[i++] = 'o'; s[i++] = 't'; s[i++] = ' ';
    s[i++] = (slot == OTA_SLOT_B) ? 'B' : 'A';
    s[i++] = ' ';
    s[i++] = '(';
    s[i++] = 'b'; s[i++] = 'a'; s[i++] = 's'; s[i++] = 'e'; s[i++] = ' ';
    s[i++] = '0'; s[i++] = 'x';
    for (k = 0u; k < 8u; ++k)
    {
        s[i++] = HD[(base >> ((7u - k) * 4u)) & 0xFu];
    }
    s[i++] = ')';
    s[i]   = '\0';

    (void)ota_link_log(s);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* PLL2 由 fmc.c 的 HAL_FMC_MspInit() 通过 HAL_RCCEx_PeriphCLKConfig() 配置。
     不要在这里手配 PLL2：HAL 会用 PeriphClkInitStruct.PLL2 重新配置它，
     重复配置或传空结构体会导致 PLL2 锁不上而黑屏。 */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_DMA2D_Init();
  MX_FMC_Init();
  MX_LTDC_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* 正式显示：帧缓冲 = SDRAM 0xC0000000，满屏 1024x600 RGB565，8 色彩条。
     MX_LTDC_Init() 返回后 LTDC 已在读帧缓冲，因此先关 LTDC 再整帧写入，
     排空写缓冲后重新开 LTDC 并重配图层，避免第一帧半新半旧。
     注意：LTDC_G6 必须走 PI1(AF14)；CubeMX 默认的 PI11(AF9) 会让屏端 G6 浮空。 */
  CLEAR_BIT(LTDC->GCR, LTDC_GCR_LTDCEN);

  #define FB_W   1024U
  #define FB_H   600U
  #define NBARS  8U
  {
    const uint16_t bars[NBARS] =
      {0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xFFFF, 0x0000};
    volatile uint16_t *fb = (volatile uint16_t *)(0xC0000000u);

    for (uint32_t y = 0U; y < FB_H; y++)
    {
      volatile uint16_t *row = fb + y * FB_W;
      for (uint32_t x = 0U; x < FB_W; x++)
      {
        uint32_t idx = x / (FB_W / NBARS);
        if (idx >= NBARS) { idx = NBARS - 1U; }
        row[x] = bars[idx];
      }
    }

    /* 图层参数与 MX_LTDC_Init() 一致，显式重配一次确保基址/窗口/格式正确 */
    LTDC_LayerCfgTypeDef lc = {0};
    lc.WindowX0 = 0; lc.WindowX1 = FB_W;
    lc.WindowY0 = 0; lc.WindowY1 = FB_H;
    lc.PixelFormat = LTDC_PIXEL_FORMAT_RGB565;
    lc.Alpha = 255; lc.Alpha0 = 0;
    lc.BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA;
    lc.BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA;
    lc.FBStartAdress = (uint32_t)fb;
    lc.ImageWidth = FB_W; lc.ImageHeight = FB_H;
    lc.Backcolor.Blue = 0; lc.Backcolor.Green = 0; lc.Backcolor.Red = 0;

    /* 帧缓冲就在 0xC0000000：volatile 读 + DSB 保证 CPU 写全部落地后再开 LTDC */
    (void)*(volatile uint16_t *)0xC0000000u;
    __DSB();

    SET_BIT(LTDC->GCR, LTDC_GCR_LTDCEN);
    HAL_LTDC_ConfigLayer(&hltdc, &lc, 0);
  }
  #undef FB_W
  #undef FB_H
  #undef NBARS

  /* OTA stack, still before osKernelStart() so flash writes are not
     interrupted by the scheduler. OTA_ACTIVE_SLOT comes from the Keil
     target defines (APP_A = 0, APP_B = 1). */
  {
    ota_core_cfg_t cfg;

    cfg.active_slot    = app_link_slot();
    cfg.millis         = ota_uart_millis;
    cfg.reset_now      = app_reset_now;
    cfg.reset_delay_ms = 250u;

    ota_core_init(&cfg);
    /* 宏与链接地址不一致 = 镜像被增量构建污染了，尽早喊出来。 */
    if (app_link_slot() != (uint8_t)OTA_ACTIVE_SLOT)
    {
        (void)ota_link_log("WARN: OTA_ACTIVE_SLOT macro != link address, "
                           "using link address (stale .o? rebuild with -r)");
    }
    (void)ota_core_confirm_app();  /* clear trial flag after a good boot */
    ota_uart_init();
    ota_ui_init();
    app_log_boot_identity();
    app_log_slot_verify();
    app_log_meta();
    app_log_b_slot_header();
  }
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 128;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV4;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV4;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV4;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0xC0000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
