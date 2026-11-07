# -*- coding: utf-8 -*-
"""抓 ESP32 的 UART 口（CH343，COM50）日志。

用法:
    python capture_uart.py [端口] [秒数]
默认 COM50 / 25 秒。

注意：板子有两个 COM 口，只有 CH343（1A86）这个「UART」口会输出 Serial 日志，
原生 USB 口（303A:1001）只在烧录时有用，日志不从这里出。
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM50"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

ser = serial.Serial(port, 115200, timeout=0.2)
# 打开 CH343 时 DTR/RTS 会被拉高，等价于按住 EN/BOOT，ESP32 会一直复位。
# 这里显式放开，让板子正常跑起来。
ser.dtr = False
ser.rts = False
time.sleep(0.05)

print("---- %s @115200 for %.0fs ----" % (port, secs))
buf = b""
t0 = time.time()
try:
    while time.time() - t0 < secs:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(line.decode("utf-8", "replace").rstrip("\r"))
finally:
    ser.close()
if buf.strip():
    print(buf.decode("utf-8", "replace"))
print("---- end ----")
