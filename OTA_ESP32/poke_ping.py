# -*- coding: utf-8 -*-
"""往 COM50 敲一个小写 p，触发 ESP32 的 localPingTest。

不依赖云、不依赖上位机，直接判定 ESP32 -> STM32 方向是否还活着，
同时看 STM32 有没有在持续发心跳（rx 计数会涨）。
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM50"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

ser = serial.Serial(port, 115200, timeout=0.2)
ser.dtr = False
ser.rts = False
time.sleep(0.05)
# 先清掉开机日志的残留
while ser.in_waiting:
    ser.read(4096)

print("---- send 'p' to %s ----" % port, flush=True)
ser.write(b"p")
ser.flush()

buf = b""
t0 = time.time()
while time.time() - t0 < secs:
    chunk = ser.read(4096)
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        print(line.decode("utf-8", "replace").rstrip("\r"), flush=True)
ser.close()
print("---- end ----", flush=True)
