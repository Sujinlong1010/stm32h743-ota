/**
 * @file  onenet_config.example.h
 * @brief 配置模板。复制为 onenet_config.h 后填入你自己的参数即可。
 *
 *        cp include/onenet_config.example.h include/onenet_config.h
 *
 *  onenet_config.h 含 WiFi 密码与设备 Token，已被 .gitignore 排除，
 *  请勿提交到版本库。
 *
 *  TOKEN 怎么来：
 *    1. 打开上位机 →「OneNET 云端」页，填 产品ID / 设备名称 / 设备密钥；
 *    2. 点「生成 Token」，把生成结果里的 "token" 字符串整段复制过来；
 *    3. 粘到下面 ONENET_TOKEN，有效期默认 10 年，不用每次换。
 *
 *  接线（ESP32-S3 <-> STM32H743，必须共地，TX/RX 交叉）：
 *    ESP32 GPIO18 (TX) --> STM32 PB15 (USART1_RX)
 *    ESP32 GPIO17 (RX) <-- STM32 PB14 (USART1_TX)
 *    ESP32 GND        --- STM32 GND
 */
#ifndef ONENET_CONFIG_H
#define ONENET_CONFIG_H

/* ---------------------------------------------------------------- WiFi -- */
#define WIFI_SSID              "your-wifi-ssid"
#define WIFI_PASSWORD          "your-wifi-password"

/* -------------------------------------------------------------- OneNET -- */
#define ONENET_PRODUCT_ID      "000000"            /* 产品 ID，例如 "123456"            */
#define ONENET_DEVICE_NAME     "your_device"       /* 设备名称，例如 "stm32_ota_01"     */
#define ONENET_TOKEN           "version=2018-10-31&res=...&method=sha1&sign=..."  /* 上位机生成的 token 整段 */
#define ONENET_MQTT_HOST       "mqtts.heclouds.com"
#define ONENET_MQTT_PORT       1883

/* 只用于物模型属性上报的展示版本号；STM32 固件真版本在槽尾固件头里 */
#define ONENET_DEFAULT_VERSION "1.0.0"

/* ------------------------------------------------------- STM32 UART1 -- */
#define STM32_UART_RX_PIN      17
#define STM32_UART_TX_PIN      18
#define STM32_UART_BAUD        921600

/* ---------------------------------------------------------- 运行参数 -- */
/* OneNET 同步命令超时最长 30 s，这里给 STM32 3 s 应答时间，超时立刻回一个
   ST_TIMEOUT 伪 ACK，避免 HTTP 请求一直挂到 30 s。 */
#define CMD_ACK_WAIT_MS        3000
#define CLOUD_FRAME_MAX        1100     /* base64 前最大帧长 */
#define MQTT_KEEPALIVE_S       60
#define MQTT_RECONNECT_MS      3000

#endif /* ONENET_CONFIG_H */
