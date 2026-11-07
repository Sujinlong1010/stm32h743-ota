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
#include "dma.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ota_uart.h"
#include "ota_core.h"
#include "ota_flash.h"
#include <string.h>          /* memset，仅启动日志用 */
#include "ota_link.h"
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
static uint8_t  s_boot_target;
static uint32_t s_rescue_deadline;
static uint8_t  s_rescue_forever;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
static void boot_reset_now(void);
static void boot_jump(uint32_t base);
static uint8_t boot_raw_image_ok(uint32_t base);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void boot_reset_now(void)
{
    NVIC_SystemReset();
}

/**
 * @brief Jump to an application image whose vector table sits at base.
 *
 *  Order matters: kill UART/DMA and SysTick first, clear every NVIC enable
 *  and pending bit, disable the MPU (the APP reconfigures it), point VTOR
 *  at the slot, load the APP stack pointer, then re-enable interrupts
 *  before the jump. Missing __enable_irq() leaves FreeRTOS/SysTick dead.
 */
static void boot_jump(uint32_t base)
{
    uint32_t sp = *(volatile uint32_t *)base;
    uint32_t pc = *(volatile uint32_t *)(base + 4u);
    void (*app_reset)(void);
    uint32_t i;

    ota_uart_deinit();

    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;
    for (i = 0u; i < 8u; ++i)
    {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    HAL_MPU_Disable();

    SCB->VTOR = base;
    __set_MSP(sp);
    app_reset = (void (*)(void))pc;
    __enable_irq();
    app_reset();

    while (1)
    {
    }
}

/**
 * @brief 开发/工厂兜底：槽里是不是一份"裸镜像"（没有 OTA 固件头）。
 *
 *  Keil/J-Link 直接下载的 .bin 只有向量表 + 代码，槽尾的 ota_fw_header_t
 *  是空的（0xFF），所以 ota_flash_verify_slot() 一定失败，
 *  ota_core_pick_boot_slot() 会返回 OTA_SLOT_NONE，板子就永远停在
 *  Bootloader 不再跳转。这里只做最少的安全检查：
 *    - 初始 MSP 落在 SRAM 内且 4 字节对齐（Cortex-M7 要求 8 字节，放宽到 4）
 *    - Reset_Handler 落在本槽范围内，且是 Thumb 地址（bit0 = 1）
 *  通过 OTA 正常写入的槽有合法固件头，走原本的严格校验，不受此函数影响。
 */
static uint8_t boot_raw_image_ok(uint32_t base)
{
    uint32_t sp = *(volatile uint32_t *)base;
    uint32_t pc = *(volatile uint32_t *)(base + 4u);

    if ((sp & 0x3u) != 0u)
    {
        return 0u;
    }
    /* H743: DTCM 0x2000_0000..0x2002_0000
     *       AXI  SRAM 0x2400_0000..0x2408_0000
     *       SRAM1..3  0x3000_0000..0x3004_8000 */
    if (!(((sp > 0x20000000u) && (sp <= 0x20020000u)) ||
          ((sp > 0x24000000u) && (sp <= 0x24080000u)) ||
          ((sp > 0x30000000u) && (sp <= 0x30048000u))))
    {
        return 0u;
    }
    if ((pc < (base + 8u)) || (pc >= (base + OTA_SLOT_SIZE)))
    {
        return 0u;
    }
    if ((pc & 1u) == 0u)
    {
        return 0u;
    }
    return 1u;
}
/* ---------------------------------------------------------- boot log ---- */
/* 这三个小工具是手写的：本工程没开 Microlib，printf 家族用不了，
   而 Bootloader 的启动决策是排查「升级成功却回到旧槽」唯一的现场证据。 */
static uint32_t bl_put_str(char *s, uint32_t i, const char *t)
{
    while (*t != '\0') { s[i++] = *t; ++t; }
    return i;
}

static uint32_t bl_put_u32(char *s, uint32_t i, uint32_t v)
{
    char t[12];
    uint32_t n = 0u, k;

    if (v == 0u) { s[i++] = '0'; return i; }
    while (v > 0u) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (k = 0u; k < n; ++k) { s[i++] = t[n - 1u - k]; }
    return i;
}

static uint32_t bl_put_hex(char *s, uint32_t i, uint32_t v)
{
    static const char HD[] = "0123456789ABCDEF";
    uint32_t k;

    for (k = 0u; k < 8u; ++k) { s[i++] = HD[(v >> ((7u - k) * 4u)) & 0xFu]; }
    return i;
}

/* 把「本次启动选了哪个槽」和「元数据当时长什么样」一起打出来。
   没有这一行，Bootloader 启动了哪个槽、为什么，在串口里完全不可见。 */
static void bl_log_boot_state(void)
{
    ota_meta_t m;
    char s[64];
    uint32_t i = 0u;

    memset(&m, 0, sizeof(m));
    if (!ota_meta_load(&m))
    {
        /* 读不到就别打 act=0/trial=0 —— trial_slot 的 0 是合法的槽 A，
           打出来会让人以为「元数据说 trial=A」，比没有信息更糟。 */
        i = bl_put_str(s, i, "BOOT: pick=");
        s[i++] = (s_boot_target == OTA_SLOT_B) ? 'B'
               : ((s_boot_target == OTA_SLOT_A) ? 'A' : 'N');
        i = bl_put_str(s, i, " meta=none");
        s[i] = '\0';
        (void)ota_link_log(s);
        return;
    }

    i = bl_put_str(s, i, "BOOT: pick=");
    s[i++] = (s_boot_target == OTA_SLOT_B) ? 'B'
           : ((s_boot_target == OTA_SLOT_A) ? 'A' : 'N');
    i = bl_put_str(s, i, " act=");
    i = bl_put_u32(s, i, (uint32_t)m.active_slot);
    i = bl_put_str(s, i, " trial=");
    i = bl_put_u32(s, i, (uint32_t)m.trial_slot);
    i = bl_put_str(s, i, " try=");
    i = bl_put_u32(s, i, (uint32_t)m.boot_try);
    s[i] = '\0';
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

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  {
    ota_core_cfg_t cfg;

    cfg.active_slot    = OTA_SLOT_NONE;
    cfg.millis         = ota_uart_millis;
    cfg.reset_now      = boot_reset_now;
    cfg.reset_delay_ms = 250u;

    ota_uart_init();
    ota_core_init(&cfg);
    s_boot_target = ota_core_pick_boot_slot();

    /* 首次烧录兜底：Keil 直烧的槽没有 OTA 固件头，上面的严格校验必然失败。
       没有这一步，烧完 Bootloader + APP_A 后会一直停在 Bootloader。 */
    if (s_boot_target == OTA_SLOT_NONE)
    {
      if (boot_raw_image_ok(OTA_SLOT_A_ADDR))
      {
        s_boot_target = OTA_SLOT_A;
        (void)ota_link_log("BOOT: raw image in A (no fw header, dev boot)");
      }
      else if (boot_raw_image_ok(OTA_SLOT_B_ADDR))
      {
        s_boot_target = OTA_SLOT_B;
        (void)ota_link_log("BOOT: raw image in B (no fw header, dev boot)");
      }
    }

    /* 3 s rescue window. With no bootable image, stay here forever so a
       bricked board is still recoverable over the same UART. */
    s_rescue_forever  = (uint8_t)((s_boot_target == OTA_SLOT_NONE) ? 1u : 0u);
    s_rescue_deadline = HAL_GetTick() + 3000u;
    (void)ota_link_log("BOOT: rescue window open");
    bl_log_boot_state();
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    ota_uart_poll();
    ota_core_poll();

    /* An active upgrade keeps the rescue window open (erase and CRC
       verification are slow, so give it 30 s after the last frame). */
    if (!ota_core_is_idle())
    {
      s_rescue_deadline = HAL_GetTick() + 30000u;
    }
    if (!s_rescue_forever && (HAL_GetTick() >= s_rescue_deadline))
    {
      /* 打印真正要跳的地址，以及该地址上的前两个字（初始 MSP / Reset_Handler）。
         这一行能一眼分辨「选错槽」和「选对了但镜像不对」。 */
      char s[72];
      uint32_t i = 0u;
      uint32_t base = ota_core_slot_base(s_boot_target);

      i = bl_put_str(s, i, "BOOT: jump 0x");
      i = bl_put_hex(s, i, base);
      i = bl_put_str(s, i, " sp=0x");
      i = bl_put_hex(s, i, *(volatile uint32_t *)base);
      i = bl_put_str(s, i, " pc=0x");
      i = bl_put_hex(s, i, *(volatile uint32_t *)(base + 4u));
      s[i] = '\0';
      (void)ota_link_log(s);
      /* 给 UART 一点时间把这一帧发完，否则 boot_jump 里的 deinit 会截断它。 */
      HAL_Delay(30);

      boot_jump(base);
    }
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
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
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
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

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
