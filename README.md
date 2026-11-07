# STM32H743 双槽 OTA

板子上是 STM32H743IIT6 加一片 ESP32-S3，两个芯片用 UART 连起来。这个工程做的事挺单一：
不拆机、不插 ST-Link，从 OneNET 把新固件推到板子上，写进备用槽，校验通过再重启切过去，
起不来就自己退回旧版本。

A→B 和 B→A 两个方向都跑通了，896 KB 的镜像大概 60~80 秒。板子上还有一块 1024×600 的
RGB 屏，升级进度直接画在屏上，不接串口也能看出来卡在哪一步。

没走 OneNET 自带的远程升级，是因为那条路要开增值服务、要把固件传到平台上去。我需要的
只是一个能过 1 KB 以内小数据的透传通道，用「同步设备命令」接口就够，固件始终在自己手里。
平台只负责把 HTTP body 原样塞进 MQTT payload，再把 STM32 的应答原样带回来。

## 链路

```
下发：
  PC 上位机 --HTTPS 同步命令--> OneNET --MQTT--> ESP32-S3 --UART1 921600--> STM32H743
              base64(0xAA55 帧)                    解 base64 还原帧

回传：
  PC 上位机 <--HTTP cmd_resp--- OneNET <--MQTT--- ESP32-S3 <--UART1-------- STM32H743
              base64(0xAA55 帧)                    封帧再 base64
```

为什么要两跳：H743 这块板子既没网口也没 WiFi，加 ESP32 当网关是最省事的做法。ESP32 在
本地模式下还能当纯粹的电平桥——USB CDC 进来、UART1 出去，不连云也能调。

几个器件：

| | |
|---|---|
| MCU | STM32H743IIT6，2 MB flash，双 bank，每 bank 8×128 KB 扇区 |
| 网关 | ESP32-S3-N16R8，16 MB flash |
| 屏 | 1024×600 RGB565，SDRAM 里开双缓冲 |
| 云 | OneNET 物联网平台，同步命令接口 |

## Flash 分区

| 区域 | 起始地址 | 大小 | 位置 |
|---|---|---|---|
| Bootloader | `0x08000000` | 128 KB | bank1 sector 0 |
| Slot A | `0x08020000` | 896 KB | bank1 sector 1~7 |
| Slot B | `0x08100000` | 896 KB | bank2 sector 0~6 |
| 元数据 | `0x081E0000` | 128 KB | bank2 sector 7 |

两个地方是特意这么设计的：

**A、B 各占一个 bank。** 擦 B 的时候不能影响正在跑的 A。如果两个槽挤在同一个 bank 里，
擦除期间 CPU 取指会被阻塞，升级过程中程序直接卡死。H743 正好是 2 MB 双 bank，一个 bank
给一个槽，天然合适。

**固件头放在槽尾，不放在槽首。** APP 的向量表必须正好落在槽起始地址上，`SCB->VTOR`
才能设成对齐的槽基址。如果槽首塞 256 字节的头，VTOR 就得偏移到 `slot+0x100`，Cortex-M7
有一百多个中断向量，这个偏移量风险太大。所以裸二进制从槽首开始写，头最后单独写进去。

> 顺带记一个 H7 HAL 的坑：`FLASH_EraseInitTypeDef.Sector` 是**所选 bank 内部的扇区号**
> （0~7），不是全局 0~15。`FLASH_SECTOR_TOTAL` 在 H743 上是 8，不是 16。

## 一次升级的流程

1. 上位机读 `APP_B.bin`，算 CRC32，发 `CMD_OTA_START`（带长度、CRC、版本号、目标槽）。
2. STM32 检查目标槽不是当前运行槽、空间够用，回 `ST_OK`，进入接收态。
3. 上位机把固件切成 360 字节一片，每片封成 `0xAA55` 帧，base64 之后走云端的同步命令。
4. ESP32 收到 MQTT 消息，base64 解码还原成帧，从 UART1 转发给 STM32，等应答再回填。
5. STM32 按偏移顺序写入。**擦除是惰性的**——写到哪个扇区才擦哪个，不预先擦整槽。
6. 分片发完，上位机发 `CMD_FW_END`。STM32 在槽尾写 256 字节固件头，然后整镜像再算一遍
   CRC32 核对。
7. 校验通过就往元数据区追加一条记录：`active_slot = 新槽`、`trial_slot = 新槽`、
   `boot_try = 0`，然后回应答。
8. 上位机发 `CMD_REBOOT`。Bootloader 上电扫描元数据，选出试用槽，严格校验后跳过去。

第 5 步的惰性擦除是被逼出来的。原来是一次性擦整槽，H743 擦 896 KB 要 7~14 秒，而云端
同步接口只等 5 秒就超时，必然失败。改成按扇区擦之后单次擦除约 1.3 秒，落在窗口内。
代价是分片命令的 ACK 等待要放宽到 4.5 秒——某一帧刚好赶上擦一个 128 KB 扇区，得给它时间。

## 回滚是怎么做的

Bootloader 启动后先扫元数据扇区，**从后往前**找最后一条 magic 和 CRC 都正确的记录。
元数据区是当追加日志用的，64 字节一条记录，状态变了就往后写一条，不用「擦-写」，
所以升级中途掉电最坏是重来一次，不会变砖。

选槽逻辑：优先 `trial_slot`（有待确认的试用槽），没有就用 `active_slot`。选定之后做严格
校验——固件头的 magic、长度、slot 字段，栈指针是否合法，`Reset_Handler` 是否落在镜像内
且是 Thumb 地址，最后整个镜像算 CRC32。任何一项不过就换另一个槽；两个槽都不合法时进
无限等待的抢救窗口，串口一直开着接命令，远程还能救回来。

APP 启动后第一件事是 `ota_core_confirm_app()`：如果试用槽就是自己，把这个槽确认掉、
清空试用标记。之后 Bootloader 不会再动它。

如果带着试用槽启动但一直没确认，每次启动 `boot_try` 加一，累计到 3 次就把 `active_slot`
退回另一个槽并写一条 `ST_ROLLBACK`。所以「新固件一启动就 HardFault」这种情况会自己回到
旧版本，不用手动救。

## 目录

```
OTA升级/
├── OTA_Bootloader/          Bootloader（CubeMX + Keil MDK）
│   ├── OTA_Bootloader.ioc
│   ├── Core/Inc|Src/
│   └── MDK-ARM/
├── OTA_STM32/               APP（CubeMX + Keil MDK，APP_A / APP_B 两个 target）
│   ├── OTA_STM32.ioc
│   ├── Core/Inc|Src/
│   ├── MDK-ARM/
│   ├── setup_keil_partition.py
│   └── check_generate.py
├── OTA_ESP32/               ESP32-S3 固件（PlatformIO）
│   ├── platformio.ini
│   ├── include/
│   ├── src/
│   ├── capture_uart.py
│   └── poke_ping.py
└── OTA_PC_Tool/             上位机
    ├── quick_ota.py         一键升级（主入口）
    ├── ota_tool.py          高级模式
    ├── ota_console.py       命令行版
    ├── cloud_transport.py
    ├── onenet_api.py
    ├── ota_framer.py
    ├── diag_http.py
    └── diag_boot.py
```

STM32 侧的 OTA 代码按职责拆成几个文件：

| 文件 | 干什么 |
|---|---|
| `ota_protocol.h` | 协议定义，帧格式、命令码、状态码、分区地址、结构体、CRC32 全在这 |
| `ota_flash.c` | flash 读写擦、固件头、槽校验、惰性擦除 |
| `ota_core.c` | 升级状态机，收片→校验→写头→打试用标记→重启，以及确认和回滚 |
| `ota_link.c` | 组帧、解帧、超时重试、ACK 匹配 |
| `ota_uart.c` | USART1 921600 + DMA 循环收 + IDLE 中断 + 环形缓冲 |
| `ota_ui.c` | 屏上那个状态页，只在 APP 里编译 |

## 协议

完整定义在 `OTA_STM32/Core/Inc/ota_protocol.h`，三端共用同一份，必须字节一致。

```
偏移  长度  字段
 0     2   MAGIC   0xAA 0x55
 2     1   VER     0x01
 3     1   FLAG    bit0 = 应答帧
 4     2   CMD     命令码
 6     2   SEQ     序号 / 分片号
 8     2   LEN     payload 长度，最大 1024
10   LEN  PAYLOAD
10+L   4   CRC32   覆盖前面所有字节，标准 zlib CRC32
```

所有多字节字段小端，总长 `14 + LEN`。

常用的几条命令：

| 命令 | 值 | 方向 | 说明 |
|---|---|---|---|
| `CMD_PING` | `0x0001` | 上位机→STM32 | 心跳 |
| `CMD_GET_STATUS` | `0x0002` | 上位机→STM32 | 查槽位、版本、阶段 |
| `CMD_OTA_START` | `0x0020` | 上位机→STM32 | 带 `ota_start_t` |
| `CMD_FW_CHUNK` | `0x0021` | 上位机→STM32 | 偏移(4B) + 原始数据 |
| `CMD_FW_END` | `0x0022` | 上位机→STM32 | 分片结束，开始整体校验 |
| `CMD_OTA_ABORT` | `0x0023` | 上位机→STM32 | 中止，当前槽不动 |
| `CMD_OTA_RESULT` | `0x0030` | STM32→上位机 | 最终结果 |
| `CMD_OTA_PROGRESS` | `0x0031` | STM32→上位机 | 进度，同时转成物模型属性上报 |
| `CMD_REBOOT` | `0x0041` | 上位机→STM32 | 软复位，触发切槽 |

状态码 `ST_*` 也在同一个头文件里：`0x00` 成功、`0x02` CRC 错、`0x07` 校验失败、
`0x08` 槽忙、`0x09` 超时、`0x0B` 回滚……

## 编译和烧录

全部在 Keil 里做。三个 target：

| 在哪 | target | 产物 |
|---|---|---|
| `OTA_STM32/MDK-ARM/OTA_STM32.uvprojx` | `APP_A` | `APP_A.bin`，链接到 `0x08020000` |
| 同上 | `APP_B` | `APP_B.bin`，链接到 `0x08100000` |
| `OTA_Bootloader/MDK-ARM/OTA_Bootloader.uvprojx` | `OTA_Bootloader` | 链接到 `0x08000000` |

**务必用 `Project → Rebuild all target files`，不要按 F7 的 Build。** 原因在下面「踩过的坑」
第一条，这里是最容易出事的地方——两个 target 共用同一个输出目录，增量构建会出串味镜像。

`APP_*.bin` 是工程里配好的 After-Build 命令生成的：

```
fromelf --bin -o .\OTA_STM32\APP_A.bin .\OTA_STM32\APP_A.axf
```

所以不用额外转格式，编完就在 `OTA_STM32/MDK-ARM/OTA_STM32/` 下。上位机也是从这个路径找。

首次烧录顺序是 **先烧 Bootloader，再烧 APP_A**。Bootloader 的下载选项里带了整片擦除
（`-FO` 的 `0x10` 位），烧完 A/B 槽和元数据都空了，必须补一次 APP_A。

ESP32 侧：

```bash
cd OTA_ESP32/include
cp onenet_config.example.h onenet_config.h   # 然后填 WiFi 和 OneNET 参数
cd ../..
pio run -t upload
```

`onenet_config.h` 里有 WiFi 密码和设备 Token，不进版本库，所以你得自己填一份。

接线（必须共地，TX/RX 交叉）：

| ESP32-S3 | STM32H743 |
|---|---|
| GPIO18 (TX) | PB15 (USART1_RX) |
| GPIO17 (RX) | PB14 (USART1_TX) |
| GND | GND |

上位机：

```bash
cd OTA_PC_Tool
cp config.example.json config.json
python quick_ota.py
```

或者直接双击 `启动上位机.vbs`。第一次进来在「OneNET 云端」页填 产品ID / 设备名称 /
设备密钥，点「生成 Token」，程序会把这些写回 `config.json`，也能一键同步到 ESP32 的
`onenet_config.h` 里，省得手工复制。

升级时分片数大概是这样：

```
固件: APP_B.bin  (180000 B)
设备在线
当前槽位: A  版本: 2.0.0
 12%  分片 12/110
100%  校验通过，写入固件头
=== 升级成功：已切换到 B 槽 ===
```

## 改了 CubeMX 配置之后

CubeMX 的 `ProjectManager.KeepUserCode=true`，所以 `/* USER CODE BEGIN */` 和
`/* USER CODE END */` 之间的代码是安全的。但它**会重新生成整个 `.uvprojx`**，下面这些
手加的东西会被冲掉：

- 手加进工程的源文件（`ota_link.c`、`ota_flash.c`、`ota_core.c`、`ota_uart.c`、`ota_ui.c`）
- `APP_A` / `APP_B` 两个 target，以及它们的 IROM1 地址
- 预定义宏 `OTA_ACTIVE_SLOT`、`OTA_APP_VERSION`
- 优化等级、C90 之类的编译选项

所以每次点完 Generate Code，我都是先跑这两条：

```bash
python OTA_STM32/setup_keil_partition.py    # 幂等，重建 target、分区地址、宏、源文件
python OTA_STM32/check_generate.py          # 只读校验
```

`check_generate.py` 输出分三组：

- **A 组**：USER CODE 块和 `.ioc` 里的锚点。FAIL 说明用户代码被覆盖了，得从备份恢复。
- **B 组**：CubeMX 生成区。FAIL 说明 `.ioc` 里没配到位，回 CubeMX 补配再生成。
- **C 组**：工程组织、三端协议副本一致性。FAIL 说明 `.uvprojx` 被重建冲掉了，或者某端
  协议头被单独改过。

三组都是 0 才算干净。

有件事一开始把我绕进去了，记下来：`.uvprojx` 里同一个「片上存储器」存了两份，`<IROM>`
和 `<OCR_RVCT4>`。前者是 Target 对话框那一栏的显示副本，后者才是 uVision 生成 `.sct` 时
真正读的。我试过把 `<IROM>` 改成 `0x09000000 / 0x1000` 这种完全不存在的地址，生成的
`.sct` 仍然是 `LR_IROM1 0x08020000 0x000E0000`，固件的 `Reset_Handler` 也照样是
`0x080203F9`——对链接没有任何影响。

而且 uVision 每次保存工程都会把 `<IROM>` 按器件默认值 `0x08000000 / 0x200000` 重写回去，
脚本改了也存不住。所以编完一次 Keil，`git status` 里 `*.uvprojx` 变成已修改是正常的，
不用管；`check_generate.py` 对它只打 `[note]`，不计入失败。

有个细节值得说明：`ota_protocol.h` 在三处各存了一份（`OTA_STM32/Core/Inc`、
`OTA_Bootloader/Core/Inc`、`OTA_ESP32/include`），必须字节一致。之所以不放一个公共目录
让三边都引用，是因为 Keil 工程和 PlatformIO 工程各有独立的 include 路径，跨工程引用会
让「clone 下来直接能编」这件事失效。以 `OTA_STM32/Core/Inc/ota_protocol.h` 为主本，改完
手动同步另外两份，C 组检查就是用来抓漏同步的。

## 踩过的坑

这部分留个记录，万一以后又撞上。

**1. 两个 target 共用一个输出目录，编出串味镜像**

这个坑花的时间最长。`APP_A` 和 `APP_B` 是两个 target，但 `<OutputDirectory>` 都写成
`OTA_STM32\`，也就是共用同一批 `.o`。用增量构建（Build）时：

```
编 A  → 生成 main.o / ota_core.o ...（OTA_ACTIVE_SLOT = 0）
切到 B → Keil 看 .o 比源码新 → 跳过编译 → 只按 B 的地址重新链接
结果   → APP_B.bin = A 的代码 + B 的链接地址
```

拿到反汇编才确认：

```asm
0x0810640c:  2441   MOVS r4,#0x41     ; 'A'，不是 'B'
0x0810646c:  4b10   LDR  r3,[pc,#64]  ; = 0x8020000，A 槽基址
```

改回 Rebuild 之后变成 `MOVS r6,#0x42`（'B'）、常量 `0x8109685`。

这个状态很阴，因为它不崩，只是行为全错：

- 上报 `active_slot=0`，上位机永远等不到「已切到 B」的确认
- `ota_core_confirm_app()` 拿 trial=B 和 active=A 比，永远不相等，试用标记清不掉，
  3 次之后被回滚
- 最要命的：它以为自己在 A，就答应「升级到 B」，然后第一片触发惰性擦除时，**把正在
  执行的自己擦掉了**，CPU 当场死

兜底做法是 `main.c` 里那个 `app_link_slot()`，不信宏，取一个函数地址来判断：

```c
static uint8_t app_link_slot(void)
{
    uint32_t here = (uint32_t)(void *)&app_link_slot;
    return (here >= OTA_SLOT_B_ADDR) ? (uint8_t)OTA_SLOT_B : (uint8_t)OTA_SLOT_A;
}
```

镜像被链接到 `0x08100000` 时，任何函数地址必然 `>= 0x08100000`，链接地址不会骗人。
宏和链接地址对不上就打个 WARN，相当于给这个问题留了个警报。

**2. Bootloader 烧一次就把整片 flash 清了**

烧完发现 3 秒后串口什么都没了，日志里是 `Full Chip Erase Done`。原因是 ST-Link 的烧录
参数 `-FO23` 里 `0x10` 位是 Erase Full Chip。改成 `-FO31` 之后保留了原来的擦除模式，
只是加了 Reset and Run（`0x08` 位）。

顺带一提，`-FO` 加 `0x08` 就是 Reset and Run，改完烧录日志从「等按复位」变成
`Application running ...`，调试时省事很多。这个参数存在 `.uvoptx` 里，而 `.uvoptx` 是
µVision 的「用户选项」（当前选中的 target、断点、窗口布局、烧录器参数），切一次
APP_A/APP_B 就会变，所以没有纳入版本库——换机器重新 clone 之后，要在
`Options for Target → Debug → Settings → Flash Download` 里手动勾一次 Reset and Run。

**3. `st=0x09` 有两种完全不同的含义**

排查时被这个误导了很久。`ota_console.py` 里打 `ping 失败 st=0x09`，看着像 STM32 回了
超时，实际是 `cloud_transport.py` 里：

```python
for k in range(self.retry):
    ok, resp = self.client.send_command(wire, api_timeout=t)
    ...
return False, ST_TIMEOUT   # 上位机到 OneNET 的 HTTP 请求失败，跟 STM32 无关
```

两个都是 `0x09`，一个是 STM32 自己回的应答字节，一个是上位机的网络错误。现在错误消息
里写清楚了是哪一侧报的。

**4. MQTT 假死，平台说设备不在线，只能物理复位**

现象是串口从 `[MQTT] ready` 之后 120 秒一行命令都没有，直接打平台接口返回
`10411 属性设置失败:设备不在线`。

根因在 `mini_mqtt.cpp`：`ping()` 只发 PINGREQ、**从来不检查 PINGRESP**，`poll()` 又把
PINGRESP 静默 skip 掉。TCP 半开的时候 `write()` 照样成功、`_c->connected()` 照样 true，
于是固件一直往一个死 socket 里写，平台那边早就判离线了。

改法很简单，记一下上一次 PINGREQ 的时间，超过 10 秒没等到 PINGRESP 就主动 `stop()`
触发重连：

```c
if (_pongPending && ((uint32_t)(millis() - _pongSentMs) > MQTT_PONG_TIMEOUT_MS))
{
    stop();
    _pongPending = false;
    return false;
}
```

**5. `C3904U: Could not open via file 'OTA_STM32\xxx.__i'`**

这个错误会一次刷十几行，每行指向不同文件，看着像工程配置全坏了，其实只有一个原因：
armcc 写不出它的 via file。

`armcc` 收几十个源文件加二十多条 `-I` 之后，命令行会超过 Windows 的 8191 字符上限，
所以 µVision 把参数写进输出目录里的 `<名字>.__i`，再把文件名传给编译器。这个文件建不
出来就报 C3904U。

真正会触发它的是**输出目录不存在**——把 `MDK-ARM/OTA_STM32/` 删了之后再构建就是这样，
Keil 未必会替你重建。多开几个 Keil 窗口、老窗口还持有已经删掉的目录，也会复现。
另外杀软正在扫新建文件时偶尔也会撞上。

所以清过构建产物之后，动手编之前先把目录建上（或者打开
`Options for Target → Output → Select Folder for Objects` 重新选一次，它会建）。

处理顺序：关掉多余的 Keil 窗口重新打开工程 → 删掉输出目录里的 `*.o` `*.d` `*.crf`
`*.__i` → `Project → Clean Targets` 再 Rebuild。

**6. 分片命令的 ACK 窗口不能按「分片很快」来估**

一开始给 1.5 秒，随机在某一帧超时。原因是那一帧正好赶上一个 128 KB 扇区的惰性擦除，
H743 擦一个扇区最长要 4 秒。后来单独给 `CMD_FW_CHUNK` 加到 4.5 秒，仍然低于平台属性
接口大约 5 秒的上限。


## 还没解决的问题

**裸镜像槽不参与严格校验。** 用 Keil 直接烧进 A 槽的镜像没有固件头，`ota_flash_verify_slot()`
会失败（原因码 1）。如果这时候 B 槽是合法的 OTA 镜像，`pick_boot_slot()` 会一直选 B，
回不到 A。目前的办法是整片擦除后重烧，或者让那个槽走一次完整的 OTA 补上固件头。

**元数据的追加日志会写满。** 128 KB / 64 B = 2048 条记录，正常用绰绰有余，但反复高频
升级最终会耗尽，写满之后需要擦掉这个扇区，自动回收还没做。

**云端单帧上限决定了速度。** OneNET 的属性字符串上限 512 字符，base64 之后每片只能带
360 字节，896 KB 要两千多片。想更快只能换文件上传通道，或者自己做分块聚合。

**ESP32 的 MQTT 是个最小实现。** 只支持 QoS 0/1 的 PUBLISH、SUBSCRIBE、PINGREQ，
没有遗嘱消息、没有离线队列、没上 TLS（1883 明文）。够这个场景用，但别指望它替代成熟
的 MQTT 库。

## 许可

自己的代码随便参考。`Drivers/` 和 `Middlewares/` 下的 STM32 HAL、CMSIS、FreeRTOS
版权归 ST、ARM、Amazon，遵循各目录里的 LICENSE。
