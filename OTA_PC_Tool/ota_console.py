# -*- coding: utf-8 -*-
"""控制台版 A->B / B->A 升级驱动。

复用 quick_ota.py 里同一套函数（load_cfg / make_client / ping / query_status /
run_ota / wait_slot），只是把 tkinter 界面换成打印，方便在命令行里跑和记录日志。

用法：
    python ota_console.py            # 升级到 B 槽（当前在 A 时必须）
    python ota_console.py 0          # 升级到 A 槽
"""
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from cloud_transport import CloudTransport          # noqa: E402
from quick_ota import (                              # noqa: E402
    SLOT_LETTER, SLOT_MAX_SIZE, ST_TIMEOUT,
    find_firmware, load_cfg, make_client, ping, query_status,
    run_ota, wait_slot,
)


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def post(kind, val=None):
    if kind == "log":
        log(val)
    elif kind == "progress":
        pct, txt = val
        log("  %3d%%  %s" % (pct, txt))
    elif kind == "fail":
        log("失败: %s" % val)
    else:
        log("%s %s" % (kind, val if val is not None else ""))


def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 1

    cfg = load_cfg()
    path = find_firmware(target, cfg)
    if not path:
        path = os.path.join(HERE, "..", "OTA_STM32", "MDK-ARM", "OTA_STM32",
                            "APP_B.bin" if target == 1 else "APP_A.bin")
        path = os.path.abspath(path)
    if not os.path.exists(path):
        log("找不到固件: %s" % path)
        return 2

    data = open(path, "rb").read()
    log("固件: %s  (%d B)" % (os.path.basename(path), len(data)))
    if len(data) > SLOT_MAX_SIZE:
        log("固件超过槽位容量")
        return 2

    client = make_client(cfg)

    # Keil 的 -f（Flash Download）下载完会把内核 halt 住，必须外部复位才开始跑。
    # 所以这里先等设备上线，用户随时按复位都能被接住。
    wait_s = 0
    if "--wait" in sys.argv:
        try:
            wait_s = int(sys.argv[sys.argv.index("--wait") + 1])
        except (IndexError, ValueError):
            wait_s = 90

    log("等待设备上线（最多 %d 秒，请按 STM32 复位）..." % wait_s)
    deadline = time.time() + wait_s
    while True:
        ok, st = ping(client)
        if ok:
            log("设备在线")
            break
        if time.time() >= deadline:
            log("ping 失败 st=0x%02X%s"
                % (st, " (0x09 是上级 HTTP 请求失败，不是 STM32 的状态)"
                   if st == ST_TIMEOUT else ""))
            return 2
        time.sleep(3)

    info = query_status(client)
    if info:
        log("当前槽位: %s  版本: %s" %
            (SLOT_LETTER.get(info.get("active_slot"), "?"),
             info.get("version", "?")))
        if info.get("active_slot") == target:
            log("设备已经在 %s 槽，换个目标槽" % SLOT_LETTER[target])
            return 2

    tr = CloudTransport(client, log=lambda m, t="": log(m),
                        retry=3, interval=0.05, ack_timeout=8)

    # 上一次失败很可能把 STM32 留在 busy（stage=WRITING 等分片），
    # 那样再发 START 只会得到 ST_BUSY。先无条件 ABORT 一次清干净。
    try:
        from ota_framer import CMD_OTA_ABORT
        log("清理上次残留状态（ABORT）...")
        tr.request(CMD_OTA_ABORT, 1, b"", timeout=10)
    except Exception as e:
        log("  ABORT 忽略: %s" % e)
    time.sleep(0.5)

    t0 = time.time()
    ok, msg = run_ota(data, target, cfg.get("version", "2.0.0"), tr,
                      lambda pct, txt: post("progress", (pct, txt)),
                      log, threading.Event())
    log("run_ota -> ok=%s msg=%s  用时 %.0f s" % (ok, msg, time.time() - t0))
    if not ok:
        return 1

    log("等待设备重启并确认新槽位 ...")
    if wait_slot(client, target, post):
        log("=== 升级成功：已切换到 %s 槽 ===" % SLOT_LETTER[target])
        return 0
    log("重启确认超时，重开界面查一下槽位")
    return 1


if __name__ == "__main__":
    sys.exit(main())
