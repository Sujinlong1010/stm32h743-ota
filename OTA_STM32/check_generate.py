# -*- coding: utf-8 -*-
"""
每次在 CubeMX 里点完 Generate Code 之后运行本脚本，检查：
  - 让屏幕正常显示的那批修复是否还在；
  - USART1(PB14/PB15) / DMA / FreeRTOS 任务是否还在；
  - 协议副本下沉后，三端协议头与两个工程的 ota_link.c 是否仍一致；
  - 两个 Keil 工程是否仍把 ota_link.c 编进去。

用法：  python check_generate.py
输出：  每条 [OK] / [FAIL]

分组含义：
  A = USER CODE 块 + .ioc 锚点。这些本应被 CubeMX 保留，
      出现 FAIL 说明用户代码被覆盖/工程被换过，需要从备份恢复。
  B = CubeMX 生成区。出现 FAIL 说明 .ioc / GUI 里没配到位，
      按提示去 CubeMX 对应界面补配，再 Generate Code 一次。
  C = 工程组织 / 协议副本一致性。出现 FAIL 说明 CubeMX 重建 .uvprojx
      时把手加的源文件冲掉了，或某端协议头被单独改过。

脚本只做只读检查，不修改任何文件。
"""

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
PARENT = os.path.dirname(ROOT)

FILES = {
    'ioc':  os.path.join(ROOT, 'OTA_STM32.ioc'),
    'main': os.path.join(ROOT, 'Core', 'Src', 'main.c'),
    'ltdc': os.path.join(ROOT, 'Core', 'Src', 'ltdc.c'),
    'fmc':  os.path.join(ROOT, 'Core', 'Src', 'fmc.c'),
}

text = {}
for key, path in FILES.items():
    try:
        with open(path, 'rb') as f:
            text[key] = f.read().decode('utf-8')
    except Exception as exc:
        print('[FATAL] cannot read %s : %s' % (path, exc))
        sys.exit(2)

# (file key, regex, label, repair hint)
GROUP_A = [
    ('ioc',  r'^PI1\.Signal=LTDC_G6\s*$',
     'LTDC_G6 assigned to PI1 (root-cause fix)',
     'Pinout view: assign LTDC_G6 to PI1, NOT PI11'),
    ('ioc',  r'^PG5\.Signal=(FMC_A15_BA1|FMC_BA1)\s*$',
     'FMC BA1 on PG5 (4-bank SDRAM)',
     'Pinout view: right-click PG5 -> FMC_BA1 (NOT FMC_A15)'),
    ('ioc',  r'^ProjectManager\.KeepUserCode=true\s*$',
     'CubeMX keeps USER CODE',
     'Project Manager -> Keep User Code when regenerating = true'),
    ('ltdc', r'PI1\s*-+>\s*LTDC_G6',
     'Generated code really drives PI1 as LTDC_G6',
     'Pinout view: PI1 must be LTDC_G6 (root-cause fix)'),
    ('main', r'#define FB_W\s+1024U',
     'Full-screen 1024x600 pattern in main.c USER CODE 2',
     'Restore main.c USER CODE BEGIN/END 2 block from backup'),
    ('ltdc', r'634UL << LTDC_TWCR_TOTALH_Pos',
     'VFP / TotalHeigh fixup in ltdc.c USER CODE',
     'Restore ltdc.c USER CODE BEGIN/END LTDC_Init 2 block'),
    ('ltdc', r'IdPin\.Pull\s*=\s*GPIO_PULLUP',
     'RGB high-bit pre-bias in ltdc.c USER CODE',
     'Restore ltdc.c USER CODE LTDC_MspInit 0'),
    ('fmc',  r'HAL_SDRAM_ProgramRefreshRate\(&hsdram1,\s*839\)',
     'SDRAM refresh count 839 in fmc.c USER CODE',
     'Restore fmc.c USER CODE FMC_Init 2'),
    ('fmc',  r'FMC_SDRAM_CMD_CLK_ENABLE',
     'SDRAM power-up command sequence in fmc.c USER CODE',
     'Restore fmc.c USER CODE FMC_Init 2'),
]

GROUP_B = [
    ('fmc',  r'FMC_SDRAM_INTERN_BANKS_NUM_4',
     'SDRAM internal banks = 4',
     'FMC -> SDRAM1 -> Internal Bank Number = 4 banks'),
    ('fmc',  r'RCC_FMCCLKSOURCE_PLL2',
     'FMC clock source = PLL2',
     'Clock Configuration -> FMC clock mux -> PLL2R'),
    ('fmc',  r'PLL2N\s*=\s*440',
     'PLL2 M=25 / N=440 / R=2 -> PLL2_R = 220MHz',
     'Clock Configuration -> enable PLL2, set DIVM2=25 DIVN2=440 DIVR2=2'),
    ('ltdc', r'PLL3M\s*=\s*25',
     'PLL3 M = 25',
     'Clock Configuration -> PLL3 DIVM3=25'),
    ('ltdc', r'PLL3N\s*=\s*300',
     'PLL3 N = 300',
     'Clock Configuration -> PLL3 DIVN3=300, VCO = WIDE'),
    ('ltdc', r'PLL3R\s*=\s*6',
     'PLL3 R = 6 -> 50MHz pixel clock',
     'Clock Configuration -> PLL3 DIVR3=6, input range 1-2MHz (RGE=0)'),
    ('ltdc', r'RCC_PLL3VCIRANGE_0',
     'PLL3 input range = 1-2MHz (RGE=0)',
     'Clock Configuration -> PLL3 input must be 1MHz (DIVM3=25)'),
    ('ltdc', r'RCC_PLL3VCOWIDE',
     'PLL3 VCO = wide range',
     'Clock Configuration -> PLL3 VCO = Wide (192-836MHz)'),
    ('ltdc', r'LTDC_PCPOLARITY_IPC',
     'Pixel clock polarity = IPC',
     'LTDC -> Pixel Clock Polarity = IPC (default)'),
    ('main', r'RCC_HCLK_DIV2',
     'HCLK = SYSCLK/2 = 200MHz',
     'Clock Configuration -> AHB prescaler = /2'),
    ('main', r'FLASH_LATENCY_2',
     'Flash latency = 2',
     'Clock Configuration -> Flash Latency = 2'),
    ('fmc',  r'SdramTiming\.SelfRefreshTime\s*=\s*7',
     'SDRAM TRAS = 7 (110MHz needs at least 5 cycles)',
     'FMC -> SDRAM1 -> Self Refresh Time = 7'),
    ('fmc',  r'SdramTiming\.RCDDelay\s*=\s*5',
     'SDRAM TRCD = 5 (keeps CubeMX TWR=2 valid; GUI-only)',
     'FMC -> SDRAM1 -> Row to column delay = 5'),
    ('fmc',  r'SdramTiming\.WriteRecoveryTime\s*=\s*2',
     'CubeMX-generated SDRAM TWR = 2 (no runtime fixup needed)',
     'FMC -> SDRAM1 -> Write recovery time = 2, then Generate Code'),
    ('fmc',  r'FMC_SDRAM_RBURST_ENABLE',
     'SDRAM read burst enabled',
     'FMC -> SDRAM1 -> Read Burst = Enable'),
]


def read_txt(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8', 'replace')
    except Exception:
        return ''


def digest(path):
    try:
        with open(path, 'rb') as f:
            return hashlib.md5(f.read()).hexdigest()
    except Exception:
        return None


def has(path, pattern):
    return re.search(pattern, read_txt(path), re.M) is not None


def group_c():
    """工程组织 + 协议副本一致性检查（跨三个工程，只读）。"""
    boot = os.path.join(PARENT, 'OTA_Bootloader')
    esp = os.path.join(PARENT, 'OTA_ESP32')
    pc = os.path.join(PARENT, 'OTA_PC_Tool')

    inc_app = os.path.join(ROOT, 'Core', 'Inc')
    inc_boot = os.path.join(boot, 'Core', 'Inc')
    src_app = os.path.join(ROOT, 'Core', 'Src')
    src_boot = os.path.join(boot, 'Core', 'Src')

    shared = [
        ('ota_protocol.h', [os.path.join(inc_app, 'ota_protocol.h'),
                            os.path.join(inc_boot, 'ota_protocol.h'),
                            os.path.join(esp, 'include', 'ota_protocol.h')]),
        ('ota_link.h',     [os.path.join(inc_app, 'ota_link.h'),
                            os.path.join(inc_boot, 'ota_link.h')]),
        ('ota_flash.h',    [os.path.join(inc_app, 'ota_flash.h'),
                            os.path.join(inc_boot, 'ota_flash.h')]),
        ('ota_core.h',     [os.path.join(inc_app, 'ota_core.h'),
                            os.path.join(inc_boot, 'ota_core.h')]),
        ('ota_uart.h',     [os.path.join(inc_app, 'ota_uart.h'),
                            os.path.join(inc_boot, 'ota_uart.h')]),
        ('ota_link.c',     [os.path.join(src_app, 'ota_link.c'),
                            os.path.join(src_boot, 'ota_link.c')]),
        ('ota_flash.c',    [os.path.join(src_app, 'ota_flash.c'),
                            os.path.join(src_boot, 'ota_flash.c')]),
        ('ota_core.c',     [os.path.join(src_app, 'ota_core.c'),
                            os.path.join(src_boot, 'ota_core.c')]),
        ('ota_uart.c',     [os.path.join(src_app, 'ota_uart.c'),
                            os.path.join(src_boot, 'ota_uart.c')]),
    ]
    uvproj = [
        os.path.join(ROOT, 'MDK-ARM', 'OTA_STM32.uvprojx'),
        os.path.join(boot, 'MDK-ARM', 'OTA_Bootloader.uvprojx'),
    ]

    items = []
    infos = []      # 只提示、不计入失败：这些字段会被 uVision 自己改写
    for name, paths in shared:
        ds = [digest(p) for p in paths]
        ok = all(d is not None for d in ds) and len(set(ds)) == 1
        items.append(('%d copies of %s byte-identical' % (len(paths), name),
                      ok, 'Re-copy the master from OTA_STM32/Core into the '
                          'other STM32 project (and ESP32 for the protocol header)'))

    for p in uvproj:
        for name in ('ota_link.c', 'ota_flash.c', 'ota_core.c', 'ota_uart.c'):
            items.append(('%s compiles %s' % (os.path.basename(p), name),
                          has(p, r'\.\./Core/Src/' + re.escape(name)),
                          'CubeMX rewrote .uvprojx: re-add ../Core/Src/%s to '
                          'group "Application/User/Core" in Keil' % name))
    items.append(('APP project compiles ota_ui.c',
                  has(uvproj[0], r'\.\./Core/Src/ota_ui\.c'),
                  'Re-add ../Core/Src/ota_ui.c to OTA_STM32 Keil project'))
    items.append(('Keil has APP_A + APP_B partition targets',
                  has(uvproj[0], r'<TargetName>APP_A</TargetName>')
                  and has(uvproj[0], r'<TargetName>APP_B</TargetName>'),
                  'CubeMX rewrote .uvprojx: run '
                  'python OTA_STM32/setup_keil_partition.py'))

    # 链接地址只认 <OCR_RVCT4>。uVision 自己生成 .sct 时读的就是它
    # （umfTarg=1，.sct 里 LR_IROM1 的地址与它一致），改这里就够了。
    app_text = read_txt(uvproj[0])

    def tblock(text, name):
        m = re.search(r'<Target>\s*<TargetName>' + name +
                      r'</TargetName>.*?</Target>', text, re.S)
        return m.group(0) if m else ''

    for tname, addr in (('APP_A', '0x8020000'), ('APP_B', '0x8100000')):
        blk = tblock(app_text, tname)
        body = (r'<Type>1</Type>\s*<StartAddress>' + addr +
                r'</StartAddress>\s*<Size>0x[eE]0000</Size>')
        items.append(('%s OCR_RVCT4 = %s / 0xE0000' % (tname, addr),
                      re.search(r'<OCR_RVCT4>\s*' + body, blk, re.S) is not None,
                      'run python OTA_STM32/setup_keil_partition.py '
                      '(<OCR_RVCT4> is what armlink actually reads)'))
        # <IROM> 只是 Target 对话框里 IROM1 那一栏的显示副本，uVision 每次保存
        # 工程都会把它按器件默认值（0x08000000/0x200000）重写回去，脚本改了也
        # 存不住。实测把它改成 0x09000000/0x1000，生成的 .sct 仍是 0x08020000、
        # 固件 Reset_Handler 仍是 0x080203F9 —— 对链接没有任何影响，所以这里只提示。
        infos.append(('%s IROM1 显示值（仅供参考，不参与链接）' % tname,
                      re.search(r'<IROM>\s*' + body, blk, re.S) is not None,
                      'cosmetic only: linking follows <OCR_RVCT4>, see the '
                      'generated .sct'))
    infos.append(('Bootloader IROM1 显示值（仅供参考，不参与链接）',
                  has(uvproj[1],
                      r'<IROM>\s*<Type>1</Type>\s*'
                      r'<StartAddress>0x8000000</StartAddress>\s*'
                      r'<Size>0x20000</Size>'),
                  'cosmetic only: linking follows <OCR_RVCT4> (0x08000000/0x20000)'))

    items.append(('no top-level Common/ folder',
                  not os.path.isdir(os.path.join(PARENT, 'Common')),
                  'Shared protocol files must live inside each project'))
    items.append(('legacy Common archive is not compiled',
                  not has(os.path.join(pc, 'ota_tool.py'), r'_legacy_Common'),
                  'Do not import from OTA_PC_Tool/_legacy_Common'))

    app_usart = os.path.join(ROOT, 'Core', 'Src', 'usart.c')
    app_it = os.path.join(ROOT, 'Core', 'Src', 'stm32h7xx_it.c')
    boot_usart = os.path.join(boot, 'Core', 'Src', 'usart.c')
    boot_conf = os.path.join(boot, 'Core', 'Inc', 'stm32h7xx_hal_conf.h')

    items.append(('APP USART1 RX DMA is Circular',
                  has(app_usart, r'hdma_usart1_rx\.Init\.Mode\s*=\s*DMA_CIRCULAR'),
                  'CubeMX -> USART1 -> DMA Settings -> Mode = Circular'))
    items.append(('APP USART1_IRQHandler generated',
                  has(app_it, r'void\s+USART1_IRQHandler'),
                  'CubeMX -> USART1 -> NVIC Settings -> enable USART1 global interrupt'))
    items.append(('APP FreeRTOS has LvglTask + OtaTask',
                  has(os.path.join(ROOT, 'OTA_STM32.ioc'),
                      r'FREERTOS\.Tasks01=.*LvglTask.*OtaTask'),
                  'CubeMX -> FreeRTOS -> Tasks and Queues -> add LvglTask / OtaTask'))
    items.append(('Bootloader USART1 uses PB14/PB15 AF4',
                  has(boot_usart, r'GPIO_AF4_USART1'),
                  'Bootloader .ioc must match APP: PB14=TX, PB15=RX, AF4, 921600'))
    items.append(('Bootloader USART1 RX DMA is Circular',
                  has(boot_usart, r'hdma_usart1_rx\.Init\.Mode\s*=\s*DMA_CIRCULAR'),
                  'CubeMX -> Bootloader USART1 -> DMA Settings -> Circular'))
    items.append(('Bootloader HAL_FLASH enabled',
                  has(boot_conf, r'#define\s+HAL_FLASH_MODULE_ENABLED'),
                  'CubeMX -> Project Manager -> Advanced Settings / .ioc: enable FLASH HAL'))
    return items, infos


def run(group, title):
    print('')
    print('=== %s ===' % title)
    bad = 0
    for key, pattern, label, hint in group:
        ok = re.search(pattern, text[key], re.M) is not None
        print('[%s] %s' % ('OK  ' if ok else 'FAIL', label))
        if not ok:
            print('       -> %s' % hint)
            bad += 1
    return bad


def run_c(title):
    print('')
    print('=== %s ===' % title)
    items, infos = group_c()
    bad = 0
    for label, ok, hint in items:
        print('[%s] %s' % ('OK  ' if ok else 'FAIL', label))
        if not ok:
            print('       -> %s' % hint)
            bad += 1
    for label, ok, hint in infos:
        print('[%s] %s' % ('OK  ' if ok else 'note', label))
        if not ok:
            print('       -> %s' % hint)
    return bad


failed_a = run(GROUP_A, 'A. USER CODE / .ioc anchors (must survive regeneration)')
failed_b = run(GROUP_B, 'B. CubeMX generated area (configure these in the GUI)')
failed_c = run_c('C. Project layout / protocol copies (re-check after every Generate Code)')

print('')
print('Group A failures : %d   (any > 0 => USER CODE lost, restore from backup)' % failed_a)
print('Group B failures : %d   (configure in CubeMX GUI, then Generate Code again)' % failed_b)
print('Group C failures : %d   (CubeMX rewrote .uvprojx, or protocol copies drifted)' % failed_c)
print('')
print('Note: CubeMX generating TotalHeigh=624 is harmless:')
print('      the ltdc.c USER CODE LTDC_Init 2 fixup overwrites TWCR.TOTALH with 634.')
print('      TWR=2 must now come from the generated area (checked in group B);')
print('      there is no runtime TWR fixup anymore.')
print('      GPIO Pull/Speed, MPU C/B and APB prescalers are NOT root fixes,')
print('      so their regeneration to CubeMX defaults is harmless.')
print('      Bootloader clock tree (HCLK=100MHz) does NOT need to match APP:')
print('      APP SystemClock_Config() reconfigures PLL / dividers / flash latency.')
print('      Both sides feed USART1 from D2PCLK2=50MHz, so 921600 baud is identical.')

sys.exit(0 if (failed_a + failed_b + failed_c) == 0 else 1)
