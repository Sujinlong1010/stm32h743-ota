# -*- coding: utf-8 -*-
"""
cloud_transport.py —— 方案 B 的云端「帧传输层」

把本地串口那套 request_ack() 抽象成同样的接口，让固件发送流程完全复用：

    上位机 build_frame(cmd, seq, payload)
      -> base64 编码成纯 ASCII（因为 OneNET 命令 body 要求 < 1KB）
      -> POST /v1/synccmds?device_id=...&timeout=...  （同步阻塞）
      -> OneNET 原样 publish 到 $sys/{pid}/{dev}/cmd/request/{cmdid}
      -> ESP32 解码成 0xAA55 帧，UART 转给 STM32
      -> STM32 ACK 帧由 ESP32 publish 到 cmd/response/{cmdid}
      -> 平台把应答 base64 放进 data.cmd_resp 返回给这次 HTTP 调用
      -> 这里解出 ACK 帧，返回 (ok, ST_*) 给 send_worker

平台从头到尾不知道「固件」是什么，只搬运 < 1KB 的字节。
"""
from __future__ import annotations

import base64
import time

from ota_framer import (
    CMD_FW_END, CMD_OTA_START, FrameParser, MAGIC, ST_BAD_CMD,
    ST_NO_SPACE, ST_OK, ST_TIMEOUT, build_frame, status_byte,
)

# 走物模型属性通道时，单次能塞的字符串长度受【属性数据长度】限制，
# OneNET 自定义属性最长 512 字符。帧 = 14 + 4 + chunk，base64 后 *4/3：
#   360B chunk -> 378B 帧 -> 504 字符，留 8 字符余量。
# （旧的 /v1/synccmds 通道限制是 1KB，本可以用 640B chunk，现已不可用。）
CLOUD_MAX_BODY = 512


class CloudTransport:
    def __init__(self, client, log=print, retry: int = 3,
                 interval: float = 0.05, ack_timeout: float = 1.5):
        self.client = client
        self.log = log
        self.retry = max(1, int(retry))
        self.interval = max(0.0, float(interval))
        self.ack_timeout = float(ack_timeout)
        self._first = True
        self.stats = {"sent": 0, "retries": 0}

    @staticmethod
    def _as_frame(resp: bytes) -> bytes:
        """兼容两种 ESP32 实现：直接回原始帧，或回帧的 base64 文本。"""
        if not isinstance(resp, (bytes, bytearray)):
            return b""
        data = bytes(resp)
        if data[:2] == MAGIC:
            return data
        try:
            dec = base64.b64decode(data.strip(), validate=False)
            if dec[:2] == MAGIC:
                return dec
        except Exception:
            pass
        return data

    def _api_timeout(self, cmd: int, timeout: float) -> int:
        # 擦除 896KB 和整片 CRC 校验都很慢，给这两条 25 s；
        # 其余分片给 ack_timeout + 5 s，始终落在 OneNET 要求的 5..30 s。
        if cmd in (CMD_OTA_START, CMD_FW_END):
            return 25
        return max(5, min(30, int(timeout) + 5))

    def request(self, cmd: int, seq: int, payload: bytes = b"",
                timeout: float | None = None) -> tuple[bool, int]:
        frame = build_frame(cmd, seq, payload)
        wire = base64.b64encode(frame)
        if len(wire) >= CLOUD_MAX_BODY:
            self.log(f"命令体 {len(wire)} B 超过 1KB，请减小云端分片", "err")
            return False, ST_NO_SPACE

        t = self._api_timeout(cmd, self.ack_timeout if timeout is None
                              else timeout)
        # 平台对命令有频率限制，两片之间留一点间隔；调大 interval 可避险。
        if self._first:
            self._first = False
        elif self.interval > 0:
            time.sleep(self.interval)

        for k in range(self.retry):
            ok, resp = self.client.send_command(wire, api_timeout=t)
            if ok:
                self.stats["sent"] += 1
                frames = FrameParser().feed(self._as_frame(resp))
                for fr in frames:
                    if fr.is_resp and fr.cmd == cmd and fr.seq == seq:
                        st = status_byte(fr.payload)
                        return st == ST_OK, st
                self.log(f"云端应答不是匹配的 ACK 帧: {bytes(resp)[:24].hex()}", "warn")
                return False, ST_BAD_CMD
            self.stats["retries"] += 1
            self.log(f"命令 0x{cmd:04X} seq={seq} 第 {k+1}/{self.retry} 次失败: {resp}",
                     "warn")
        # 注意：这里的 ST_TIMEOUT 是【上位机 -> OneNET 的 HTTP 请求失败】，
        # 与 STM32 的状态无关（STM32 的 ST_TIMEOUT 是它自己回的应答字节）。
        # 两者都是 0x09，历史上曾因此把网络问题误判成 UART 问题。
        self.log("传输失败：%d 次 HTTP 下发都没拿到平台应答（不是 STM32 的问题）"
                 % self.retry, "err")
        return False, ST_TIMEOUT

    def send_only(self, cmd: int, seq: int, payload: bytes = b"") -> bool:
        """发完不等结果：用于 REBOOT / ABORT，设备可能立刻复位。"""
        frame = build_frame(cmd, seq, payload)
        wire = base64.b64encode(frame)
        if len(wire) >= CLOUD_MAX_BODY:
            return False
        ok, resp = self.client.send_command(wire, api_timeout=5)
        if not ok:
            # 设备复位导致应答超时是预期行为，不算失败。
            self.log(f"fire-and-forget 0x{cmd:04X}: {resp}", "warn")
        return True

    def close(self):
        pass
