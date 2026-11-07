# -*- coding: utf-8 -*-
"""
ota_framer.py —— STM32 <-> ESP32 <-> 上位机 统一帧编解码

与 OTA_STM32/Core/Inc/ota_protocol.h（协议主本）严格对应（小端、zlib CRC32）。
只依赖 Python 标准库，可直接被 ota_tool.py 引用，也可单独用来自测。
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from typing import Iterable, List

MAGIC = b"\xaa\x55"
PROTO_VER = 0x01
FLAG_RESP = 0x01
HEADER_LEN = 10
CRC_LEN = 4
OVERHEAD = HEADER_LEN + CRC_LEN          # 14
MAX_PAYLOAD = 1024
MAX_FRAME_LEN = OVERHEAD + MAX_PAYLOAD   # 1038
CHUNK_OFFSET_LEN = 4
MAX_CHUNK_DATA = MAX_PAYLOAD - CHUNK_OFFSET_LEN   # 1020

# ---- 命令码（与 ota_protocol.h 一致）----
CMD_PING = 0x0001
CMD_GET_STATUS = 0x0002
CMD_WIFI_STATE = 0x0010
CMD_CLOUD_CMD = 0x0011
CMD_CLOUD_STATE = 0x0012
CMD_OTA_START = 0x0020
CMD_FW_CHUNK = 0x0021
CMD_FW_END = 0x0022
CMD_OTA_ABORT = 0x0023
CMD_OTA_RESULT = 0x0030
CMD_OTA_PROGRESS = 0x0031
CMD_SWITCH_SLOT = 0x0040
CMD_REBOOT = 0x0041
CMD_SET_WIFI = 0x0042
CMD_LOG = 0x00E0
CMD_ERROR = 0x00FF

CMD_NAME = {
    CMD_PING: "PING", CMD_GET_STATUS: "GET_STATUS",
    CMD_WIFI_STATE: "WIFI_STATE", CMD_CLOUD_CMD: "CLOUD_CMD",
    CMD_CLOUD_STATE: "CLOUD_STATE", CMD_OTA_START: "OTA_START",
    CMD_FW_CHUNK: "FW_CHUNK", CMD_FW_END: "FW_END",
    CMD_OTA_ABORT: "OTA_ABORT", CMD_OTA_RESULT: "OTA_RESULT",
    CMD_OTA_PROGRESS: "OTA_PROGRESS", CMD_SWITCH_SLOT: "SWITCH_SLOT",
    CMD_REBOOT: "REBOOT", CMD_SET_WIFI: "SET_WIFI",
    CMD_LOG: "LOG", CMD_ERROR: "ERROR",
}

# ---- 状态码 ----
ST_OK = 0x00
ST_BUSY = 0x01
ST_BAD_CRC = 0x02
ST_BAD_SEQ = 0x03
ST_BAD_CMD = 0x04
ST_ERASE_FAIL = 0x05
ST_WRITE_FAIL = 0x06
ST_VERIFY_FAIL = 0x07
ST_SLOT_BUSY = 0x08
ST_TIMEOUT = 0x09
ST_NO_SPACE = 0x0A
ST_ROLLBACK = 0x0B
ST_NO_WIFI = 0x0C
ST_BAD_HEADER = 0x0D

ST_NAME = {
    ST_OK: "OK", ST_BUSY: "BUSY", ST_BAD_CRC: "BAD_CRC",
    ST_BAD_SEQ: "BAD_SEQ", ST_BAD_CMD: "BAD_CMD",
    ST_ERASE_FAIL: "ERASE_FAIL", ST_WRITE_FAIL: "WRITE_FAIL",
    ST_VERIFY_FAIL: "VERIFY_FAIL", ST_SLOT_BUSY: "SLOT_BUSY",
    ST_TIMEOUT: "TIMEOUT", ST_NO_SPACE: "NO_SPACE",
    ST_ROLLBACK: "ROLLBACK", ST_NO_WIFI: "NO_WIFI",
    ST_BAD_HEADER: "BAD_HEADER",
}

STAGE_NAME = {
    0: "IDLE", 1: "RECEIVING", 2: "VERIFYING", 3: "WRITING",
    4: "PENDING_REBOOT", 5: "SUCCESS", 6: "FAILED", 7: "ROLLED_BACK",
}

SLOT_NAME = {0: "A", 1: "B", 0xFF: "NONE"}


def crc32(data: bytes) -> int:
    """标准 zlib CRC32，与 C 侧 ota_crc32() 完全一致。"""
    return zlib.crc32(data) & 0xFFFFFFFF


@dataclass
class Frame:
    cmd: int
    seq: int
    flag: int
    payload: bytes

    @property
    def is_resp(self) -> bool:
        return bool(self.flag & FLAG_RESP)

    @property
    def cmd_name(self) -> str:
        return CMD_NAME.get(self.cmd, f"0x{self.cmd:04X}")

    def __str__(self) -> str:
        return (f"[{self.cmd_name}{'/R' if self.is_resp else ''}] "
                f"seq={self.seq} len={len(self.payload)}")


def build_frame(cmd: int, seq: int, payload: bytes = b"",
                resp: bool = False) -> bytes:
    """组装一帧。payload 超过 MAX_PAYLOAD 会抛 ValueError。"""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too big: {len(payload)} > {MAX_PAYLOAD}")
    flag = FLAG_RESP if resp else 0
    head = struct.pack(
        "<2sBBHHH", MAGIC, PROTO_VER, flag,
        cmd & 0xFFFF, seq & 0xFFFF, len(payload)
    )
    body = head + payload
    return body + struct.pack("<I", crc32(body))


class FrameParser:
    """
    流式解析器。ESP32 的 USB 串口会混入 WiFi 日志等纯文本，
    这里按 0xAA55 重新同步，非帧字节一律跳过。
    """

    def __init__(self) -> None:
        self.buf = bytearray()

    def reset(self) -> None:
        self.buf.clear()

    def feed(self, data: bytes) -> List[Frame]:
        self.buf.extend(data)
        out: List[Frame] = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                # 保留最后 1 字节，防止 MAGIC 被拆在两个 read 里
                if len(self.buf) > 1:
                    del self.buf[:-1]
                break
            if i > 0:
                del self.buf[:i]
            if len(self.buf) < OVERHEAD:
                break

            ver, flag = self.buf[2], self.buf[3]
            cmd, seq, ln = struct.unpack_from("<HHH", self.buf, 4)
            if ver != PROTO_VER or ln > MAX_PAYLOAD:
                del self.buf[0]
                continue

            total = OVERHEAD + ln
            if len(self.buf) < total:
                break

            raw = bytes(self.buf[:total])
            recv_crc = struct.unpack_from("<I", raw, HEADER_LEN + ln)[0]
            if crc32(raw[:HEADER_LEN + ln]) != recv_crc:
                del self.buf[0]      # CRC 错，重新同步
                continue

            out.append(Frame(cmd, seq, flag, raw[HEADER_LEN:HEADER_LEN + ln]))
            del self.buf[:total]
        return out


# ---------------------------------------------------------------- payload --
def pack_start(fw_size: int, fw_crc: int, version: str,
               target_slot: int) -> bytes:
    """ota_start_t: u32 size + u32 crc + char[24] + u8 slot + u8 reserved[3] = 36B"""
    v = version.encode("ascii", "replace")[:23].ljust(24, b"\x00")
    return struct.pack("<II24sBBBB", fw_size & 0xFFFFFFFF,
                       fw_crc & 0xFFFFFFFF, v, target_slot & 0xFF, 0, 0, 0)


def unpack_start(p: bytes) -> dict:
    if len(p) < 36:
        return {}
    size, crc, v, slot, _, _, _ = struct.unpack("<II24sBBBB", p[:36])
    return {"fw_size": size, "fw_crc32": crc,
            "version": v.split(b"\x00")[0].decode("ascii", "replace"),
            "target_slot": slot}


def pack_chunk(offset: int, data: bytes) -> bytes:
    if len(data) > MAX_CHUNK_DATA:
        raise ValueError("chunk too big")
    return struct.pack("<I", offset & 0xFFFFFFFF) + data


def unpack_chunk(p: bytes) -> tuple[int, bytes]:
    if len(p) < CHUNK_OFFSET_LEN:
        raise ValueError("short chunk")
    return struct.unpack_from("<I", p, 0)[0], p[CHUNK_OFFSET_LEN:]


def unpack_status(p: bytes) -> dict:
    if len(p) < 8:
        return {}
    stage, result, progress, slot, detail = struct.unpack("<BBBBI", p[:8])
    return {"stage": stage, "stage_name": STAGE_NAME.get(stage, "?"),
            "result": result, "result_name": ST_NAME.get(result, "?"),
            "progress": progress, "active_slot": slot,
            "slot_name": SLOT_NAME.get(slot, "?"), "detail": detail}


def unpack_wifi(p: bytes) -> dict:
    if len(p) < 6:
        return {}
    connected, rssi, a, b, c, d = struct.unpack("<BbBBBB", p[:6])
    return {"connected": connected, "rssi": rssi, "ip": f"{a}.{b}.{c}.{d}"}


def status_byte(p: bytes) -> int:
    """ACK / ERROR 帧的 payload[0] 是状态码。"""
    return p[0] if p else ST_BAD_CMD


# ------------------------------------------------------------------- 自测 --
if __name__ == "__main__":
    f = build_frame(CMD_OTA_START, 7, pack_start(12345, 0xDEADBEEF, "2.0.0", 1))
    parser = FrameParser()
    frames = parser.feed(b"noise text\r\n" + f[:5])
    assert frames == [], "半帧不应产出"
    frames += parser.feed(f[5:])
    assert len(frames) == 1 and frames[0].seq == 7
    assert unpack_start(frames[0].payload)["version"] == "2.0.0"
    st = build_frame(CMD_OTA_RESULT, 7, struct.pack("<BBBBI", 5, ST_OK, 100, 0, 12345), resp=True)
    frames = parser.feed(st)
    assert unpack_status(frames[0].payload)["result_name"] == "OK"
    print("ota_framer self-test PASSED")
