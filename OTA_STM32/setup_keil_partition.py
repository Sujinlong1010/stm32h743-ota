# -*- coding: utf-8 -*-
"""
setup_keil_partition.py —— 给两个 Keil 工程配置 A/B 分区与目标。

CubeMX 每次 Generate Code 都会重建 .uvprojx，把内存布局打回默认，
所以这个脚本设计成【幂等】：每次点完 Generate Code 后跑一次即可。

    python OTA_STM32/setup_keil_partition.py

它做四件事（改之前自动备份成 .uvprojx.bak，只备份一次）：
  1. OTA_Bootloader : IROM1 = 0x08000000 / 0x20000
  2. OTA_STM32      : 生成/规范 APP_A Target = 0x08020000 / 0xE0000
                      生成/规范 APP_B Target = 0x08100000 / 0xE0000
  3. 两个 Target 各自加 USER_VECT_TAB_ADDRESS + VECT_TAB_OFFSET +
     OTA_ACTIVE_SLOT 宏，编译出的 .bin 才能放到对应槽运行
  4. AfterMake 调 fromelf 生成 .bin，直接给上位机当固件用

为什么需要两份 APP：STM32 固件是按绝对地址链接的，Slot A 的 .bin
搬到 Slot B 跑不起来。升级到哪个槽，就必须在上位机里选对应的 .bin：
    APP_A.bin -> 目标槽 A   /   APP_B.bin -> 目标槽 B
"""
from __future__ import annotations

import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
PARENT = os.path.dirname(ROOT)
APP_PATH = os.path.join(ROOT, 'MDK-ARM', 'OTA_STM32.uvprojx')
BOOT_PATH = os.path.join(PARENT, 'OTA_Bootloader', 'MDK-ARM',
                         'OTA_Bootloader.uvprojx')

BOOT_FROMELF = r'fromelf --bin -o .\OTA_Bootloader\@L.bin .\OTA_Bootloader\@L.axf'
APP_FROMELF = r'fromelf --bin -o .\OTA_STM32\@L.bin .\OTA_STM32\@L.axf'


def read(path: str) -> str:
    with open(path, 'r', encoding='utf-8') as f:
        return f.read()


def write(path: str, text: str):
    bak = path + '.bak'
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
        print('[backup] %s' % bak)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def set_mem(text: str, tags, typ: str, start: str, size: str) -> str:
    """改写一个 Target 里的存储器映射。

    Keil 同一个"片上存储器"会在 XML 里出现两份：
      <OnChipMemories>
        <IRAM>...</IRAM>          <-- 旧式写法
        <IROM>...</IROM>
        <OCR_RVCT4>...</OCR_RVCT4>  <-- Keil 实际用于链接的那份（IROM1）
        <OCR_RVCT9>...</OCR_RVCT9>  <-- IRAM1
      </OnChipMemories>

    只改 <IROM> 是不够的：链接器读的是 OCR_RVCT4，所以以前脚本"跑成功了"
    但编译出来的 .bin 仍然按 0x08000000 链接，配合 VECT_TAB_OFFSET=0x00020000
    会让 SCB->VTOR 指到空区域，一启动就 HardFault（屏幕全黑）。
    这里两份都改，缺一份就报错，避免静默失败。
    """
    if isinstance(tags, str):
        tags = [tags]
    for tag in tags:
        pat = (r'(<' + tag + r'>\s*<Type>' + typ + r'</Type>\s*'
               r'<StartAddress>)[^<]*(</StartAddress>\s*<Size>)[^<]*(</Size>)')
        text, n = re.subn(pat, lambda m: m.group(1) + start + m.group(2)
                          + size + m.group(3), text, count=1)
        if n != 1:
            raise RuntimeError('cannot locate <%s> memory block' % tag)
    return text


def set_aftermake(text: str, cmd: str) -> str:
    m = re.search(r'<AfterMake>.*?</AfterMake>', text, re.S)
    if m is None:
        raise RuntimeError('cannot locate <AfterMake>')
    b = m.group(0)
    b = re.sub(r'<RunUserProg2>\d+</RunUserProg2>',
               '<RunUserProg2>1</RunUserProg2>', b, count=1)
    # Use a lambda replacement: the command contains backslashes, which a
    # plain replacement string would interpret as regex escapes.
    b, n = re.subn(r'<UserProg2Name\s*/>|<UserProg2Name>.*?</UserProg2Name>',
                   lambda mm: '<UserProg2Name>' + cmd + '</UserProg2Name>',
                   b, count=1)
    if n != 1:
        raise RuntimeError('cannot locate <UserProg2Name> in AfterMake')
    return text[:m.start()] + b + text[m.end():]


def standardize_target(block: str, name: str, rom_start: str,
                       rom_size: str, vect: str, slot: int) -> str:
    block = re.sub(r'<TargetName>.*?</TargetName>',
                   '<TargetName>' + name + '</TargetName>', block, count=1)
    block = re.sub(r'<OutputName>.*?</OutputName>',
                   '<OutputName>' + name + '</OutputName>', block, count=1)
    block = set_mem(block, ['IROM', 'OCR_RVCT4'], '1', rom_start, rom_size)
    defines = ('USE_PWR_LDO_SUPPLY,USE_HAL_DRIVER,STM32H743xx,'
               'USER_VECT_TAB_ADDRESS,VECT_TAB_OFFSET=' + vect +
               ',OTA_ACTIVE_SLOT=' + str(slot))
    block, n = re.subn(r'<Define>.*?</Define>',
                       '<Define>' + defines + '</Define>', block, count=1)
    if n != 1:
        raise RuntimeError('cannot locate <Define> in target ' + name)
    block = set_aftermake(block, APP_FROMELF)
    return block


def do_boot():
    t = read(BOOT_PATH)
    t = set_mem(t, ['IROM', 'OCR_RVCT4'], '1', '0x8000000', '0x20000')
    t = set_mem(t, ['IRAM', 'OCR_RVCT9'], '0', '0x20000000', '0x20000')
    t = set_aftermake(t, BOOT_FROMELF)
    write(BOOT_PATH, t)
    print('[OK] OTA_Bootloader  IROM1 = 0x08000000 / 0x20000, fromelf .bin')


def do_app():
    t = read(APP_PATH)
    m = re.search(r'(<Targets>)(.*?)(</Targets>)', t, re.S)
    if m is None:
        raise RuntimeError('cannot locate <Targets>')
    blocks = re.findall(r'    <Target>.*?\n    </Target>', m.group(2), re.S)
    if not blocks:
        raise RuntimeError('no <Target> block found')
    base = blocks[0]
    # 大小写成小写 0xe0000，和 uVision 自己保存时的写法一致。
    # 大写的 0xE0000 数值相同，但 Keil 每次保存都会改回小写，会白白产生一条 diff。
    a = standardize_target(base, 'APP_A', '0x8020000', '0xe0000',
                           '0x00020000', 0)
    b = standardize_target(base, 'APP_B', '0x8100000', '0xe0000',
                           '0x00100000', 1)
    new = t[:m.start(2)] + '\n' + a + '\n' + b + '\n  ' + t[m.end(2):]
    write(APP_PATH, new)
    print('[OK] APP_A  IROM1 = 0x08020000 / 0xE0000  (VECT_TAB_OFFSET=0x00020000)')
    print('[OK] APP_B  IROM1 = 0x08100000 / 0xE0000  (VECT_TAB_OFFSET=0x00100000)')
    print('[OK] both targets run fromelf --bin -> MDK-ARM/OTA_STM32/APP_A.bin, APP_B.bin')


def main():
    do_boot()
    do_app()
    print('')
    print('Next: open both Keil projects, Build APP_A and APP_B separately,')
    print('      and feed the matching .bin to the PC tool for that slot.')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('[FATAL] %s' % exc)
        sys.exit(1)
