# -*- coding: utf-8 -*-
"""重启 STM32 并抓 Bootloader 的启动决策日志。

一边读 ESP32 的串口，一边从云端发 CMD_REBOOT，把 Bootloader 打的
"BOOT: ..." / "APP: slot X" 全部抓下来。

用法:
    python diag_boot.py [端口]
默认 COM50（ESP32 的 CH343 UART 口）。
"""
import os
import sys
import threading
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from cloud_transport import CloudTransport            # noqa: E402
from ota_framer import CMD_REBOOT                     # noqa: E402
from quick_ota import load_cfg, make_client           # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM50"

ser = serial.Serial(PORT, 115200, timeout=0.2)
ser.dtr = False
ser.rts = False
time.sleep(0.1)

stop = threading.Event()
mqtt_ready = threading.Event()


def reader():
    buf = b""
    while not stop.is_set():
        c = ser.read(4096)
        if not c:
            continue
        buf += c
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = line.decode("utf-8", "replace").rstrip("\r")
            if t:
                print("  [ESP] " + t, flush=True)
                if "MQTT] ready" in t:
                    mqtt_ready.set()


threading.Thread(target=reader, daemon=True).start()

print("等 ESP32 连上 MQTT（最多 40s）...", flush=True)
if not mqtt_ready.wait(40):
    print("!! 没等到 MQTT ready，仍然继续", flush=True)
time.sleep(1)

cfg = load_cfg()
cl = make_client(cfg)
tr = CloudTransport(cl, log=lambda m, t="": print("  [云] " + str(m), flush=True),
                    retry=1, interval=0.0, ack_timeout=5)
print(">>> 发 CMD_REBOOT", flush=True)
tr.send_only(CMD_REBOOT, 0x7777)
time.sleep(18)
tr.close()
stop.set()
time.sleep(0.3)
ser.close()
print("---- end ----")
