# -*- coding: utf-8 -*-
"""直接发一条 PING，把 send_command 的原始返回值打出来。

ota_console / quick_ota 在失败时只报告 st=0x09，看不到平台到底说了什么
（401 鉴权失败 / 429 限频 / 10411 设备应答超时，三种情况的处理完全不同）。
"""
import base64
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ota_framer import CMD_PING, build_frame
from quick_ota import load_cfg, make_client


def main():
    cfg = load_cfg()
    c = make_client(cfg)
    print("channel =", getattr(c, "channel", "?"))
    print("pid/dev =", getattr(c, "pid", "?"), "/", getattr(c, "dev", "?"))
    wire = base64.b64encode(build_frame(CMD_PING, 0x1234, b""))
    print("body len =", len(wire))
    ok, resp = c.send_command(wire, api_timeout=10)
    print("ok =", ok)
    print("resp =", repr(resp)[:600])


if __name__ == "__main__":
    main()
