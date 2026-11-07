# -*- coding: utf-8 -*-
"""
STM32H743 + ESP32-S3 远程 OTA 升级上位机（方案 B：OneNET 只做中转）

两条通道，一套协议、一套固件发送流程：

  本地：PC --USB CDC--> ESP32 --UART1 921600--> STM32
  云端：PC --同步命令API--> OneNET --MQTT--> ESP32 --UART1--> STM32
        OneNET 不托管固件、不建升级任务，只透传 < 1KB 的自定义命令体。

启动：双击「启动上位机.vbs」或「启动上位机.bat」，或直接 python ota_tool.py。
"""
from __future__ import annotations

import base64
import hashlib
import json
import math
import os
import queue
import threading
import time
import tkinter as tk
import webbrowser
from tkinter import filedialog, messagebox, ttk

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

from ota_framer import (
    CMD_ERROR, CMD_FW_CHUNK, CMD_FW_END, CMD_GET_STATUS, CMD_LOG,
    CMD_OTA_ABORT, CMD_OTA_PROGRESS, CMD_OTA_RESULT, CMD_OTA_START,
    CMD_PING, CMD_REBOOT, CMD_SWITCH_SLOT, CMD_WIFI_STATE,
    MAX_CHUNK_DATA, ST_OK, ST_TIMEOUT, FrameParser, MAX_PAYLOAD,
    build_frame, crc32, pack_chunk, pack_start, status_byte,
    unpack_status, unpack_wifi,
)
from onenet_api import (
    CONSOLE_URLS, OneNetClient, device_properties_payload, make_token,
    topic_map,
)
from cloud_transport import CLOUD_MAX_BODY, CloudTransport

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "config.json")

# 云端分片上限：b64(14 + 4 + chunk) 必须 < 1KB
CLOUD_MAX_CHUNK = 700

DEFAULT_CONFIG = {
    "port": "", "baud": 921600,
    "firmware": "", "version": "2.0.0", "target_slot": 1,
    "chunk_size": 512, "ack_timeout": 1.5, "retry": 3,
    "final_timeout": 30.0,
    "product_id": "", "device_name": "", "device_id": "", "device_key": "",
    "api_base": "https://iot-api.heclouds.com",
    "command_path": "/v1/synccmds",
    "token_method": "sha1", "token_scope": "device",
    "token_sign_order": "et_method_res_version",
    "cloud_chunk_size": 640, "cloud_interval": 0.05,
    "cloud_cmd_timeout": 8,
}

CMD_NAME_LOCAL = {
    CMD_PING: "PING", CMD_GET_STATUS: "GET_STATUS", CMD_REBOOT: "REBOOT",
    CMD_SWITCH_SLOT: "SWITCH_SLOT",
}


class SerialTransport:
    """本地串口：写帧后等待同 cmd/seq 的 ACK。"""

    def __init__(self, app, retry: int, timeout: float):
        self.app = app
        self.retry = max(1, int(retry))
        self.timeout = float(timeout)

    def request(self, cmd, seq, payload=b"", timeout=None):
        t = self.timeout if timeout is None else float(timeout)
        st = ST_TIMEOUT
        for _ in range(self.retry):
            if not self.app.write_frame(cmd, seq, payload):
                return False, ST_TIMEOUT
            ok, st = self.app.wait_ack(cmd, seq, t)
            if ok:
                return True, st
        return False, st

    def send_only(self, cmd, seq, payload=b""):
        return self.app.write_frame(cmd, seq, payload)

    def close(self):
        pass


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("STM32H743 远程 OTA 升级工具  |  OneNET 中转 / 自研协议")
        self.root.geometry("1000x820")
        self.root.minsize(920, 720)

        self.cfg = self.load_config()
        self.ser = None
        self.parser = FrameParser()
        self.rx_q: queue.Queue = queue.Queue()
        self.ui_q: queue.Queue = queue.Queue()
        self.stop_evt = threading.Event()
        self.abort_evt = threading.Event()
        self.ack_evt = threading.Event()
        self.ack_data: dict = {}
        self.tx_lock = threading.Lock()
        self.sending = False
        self.mode = "local"
        self.reader: threading.Thread | None = None
        self.sender: threading.Thread | None = None
        self.fw_bytes: bytes | None = None

        self.build_ui()
        self.refresh_ports()
        self.root.after(60, self.poll)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.log("就绪。本地页走 USB 串口；云端页走 OneNET 命令透传，两条通道共用同一套帧协议。")
        self.log("这是高级模式。日常升级请双击『启动上位机.vbs』打开一键版（两个按钮）；"
                 "本界面从一键版右下角『高级模式』进入。云端参数与一键版共用 config.json。")
        if not HAS_SERIAL:
            self.log("未检测到 pyserial，本地串口页不可用；云端页仍可正常升级。", "warn")

    # ------------------------------------------------------------ 配置 --
    def load_config(self) -> dict:
        c = dict(DEFAULT_CONFIG)
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                c.update(json.load(f))
        except Exception:
            pass
        return c

    def save_config(self):
        self.collect_config()
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(self.cfg, f, ensure_ascii=False, indent=2)
            self.log(f"配置已保存：{CONFIG_PATH}")
        except Exception as e:
            self.log(f"配置保存失败：{e}", "err")

    def collect_config(self):
        g = self.v
        self.cfg.update({
            "port": g["port"].get(), "baud": int(g["baud"].get() or 921600),
            "firmware": g["firmware"].get(), "version": g["version"].get().strip(),
            "target_slot": 0 if str(g["target_slot"].get()).upper() == "A" else 1,
            "chunk_size": int(g["chunk_size"].get() or 512),
            "ack_timeout": float(g["ack_timeout"].get() or 1.5),
            "retry": int(g["retry"].get() or 3),
            "final_timeout": float(g["final_timeout"].get() or 30),
            "product_id": g["product_id"].get().strip(),
            "device_name": g["device_name"].get().strip(),
            "device_id": g["device_id"].get().strip(),
            "device_key": g["device_key"].get().strip(),
            "api_base": g["api_base"].get().strip(),
            "command_path": g["command_path"].get().strip(),
            "token_method": g["token_method"].get(),
            "token_scope": g["token_scope"].get(),
            "token_sign_order": g["token_sign_order"].get(),
            "cloud_chunk_size": int(g["cloud_chunk_size"].get() or 640),
            "cloud_interval": float(g["cloud_interval"].get() or 0.05),
            "cloud_cmd_timeout": int(g["cloud_cmd_timeout"].get() or 8),
        })

    # -------------------------------------------------------------- UI --
    def build_ui(self):
        self.v = {}
        nb = ttk.Notebook(self.root)
        nb.pack(fill=tk.BOTH, expand=True, padx=8, pady=6)
        local = ttk.Frame(nb)
        cloud = ttk.Frame(nb)
        nb.add(local, text="  本地串口升级  ")
        nb.add(cloud, text="  OneNET 云端升级（中转）  ")
        self.build_local(local)
        self.build_cloud(cloud)

        prog = ttk.LabelFrame(self.root, text="升级进度（两条通道共用）")
        prog.pack(fill=tk.X, padx=8, pady=(2, 4))
        self.pbar = ttk.Progressbar(prog, orient=tk.HORIZONTAL,
                                    mode="determinate", maximum=100)
        self.pbar.pack(fill=tk.X, padx=8, pady=(8, 4))
        line = ttk.Frame(prog)
        line.pack(fill=tk.X, padx=8, pady=(0, 8))
        self.lbl_prog = ttk.Label(line, text="0%  |  —")
        self.lbl_prog.pack(side=tk.LEFT)
        self.lbl_result = ttk.Label(line, text="未开始",
                                    font=("Microsoft YaHei", 10, "bold"))
        self.lbl_result.pack(side=tk.RIGHT)

        logf = ttk.LabelFrame(self.root, text="运行日志 / 设备状态反馈")
        logf.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 4))
        self.txt = tk.Text(logf, height=11, wrap=tk.WORD, font=("Consolas", 9))
        sb = ttk.Scrollbar(logf, orient=tk.VERTICAL, command=self.txt.yview)
        self.txt.configure(yscrollcommand=sb.set)
        self.txt.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        for tag, color in (("ok", "#137333"), ("err", "#C5221F"),
                           ("warn", "#B06000"), ("info", "#1A73E8")):
            self.txt.tag_config(tag, foreground=color)

        bar = ttk.Frame(self.root)
        bar.pack(fill=tk.X, padx=8, pady=(0, 8))
        ttk.Button(bar, text="保存配置", command=self.save_config).pack(side=tk.RIGHT, padx=4)
        ttk.Button(bar, text="清空日志",
                   command=lambda: self.txt.delete("1.0", tk.END)).pack(side=tk.RIGHT, padx=4)
        ttk.Button(bar, text="协议自测", command=self.selftest).pack(side=tk.RIGHT, padx=4)
        ttk.Button(bar, text="启动说明", command=self.show_help).pack(side=tk.RIGHT, padx=4)

    def row(self, parent, r, label, key, width=34, values=None, default=""):
        ttk.Label(parent, text=label,
                  font=("Microsoft YaHei", 9)).grid(row=r, column=0, sticky=tk.W,
                                                    padx=6, pady=4)
        var = tk.StringVar(value=str(self.cfg.get(key, default)))
        if values:
            w = ttk.Combobox(parent, textvariable=var, values=values,
                             width=width - 2,
                             state="readonly" if len(values) <= 4 else "normal")
        else:
            w = ttk.Entry(parent, textvariable=var, width=width)
        w.grid(row=r, column=1, sticky=tk.W, padx=4, pady=4)
        self.v[key] = var
        return w

    def build_local(self, p):
        top = ttk.LabelFrame(p, text="① 串口（连 ESP32-S3 的 USB 口，ESP32 透明转发到 STM32 USART1）")
        top.pack(fill=tk.X, padx=6, pady=6)
        self.row(top, 0, "串口", "port", 22)
        self.row(top, 1, "波特率", "baud", 22,
                 values=["115200", "230400", "460800", "921600"])
        f = ttk.Frame(top)
        f.grid(row=0, column=2, rowspan=2, padx=10)
        ttk.Button(f, text="刷新端口", command=self.refresh_ports).pack(side=tk.LEFT, padx=3)
        self.btn_conn = ttk.Button(f, text="打开串口", command=self.toggle_port)
        self.btn_conn.pack(side=tk.LEFT, padx=3)

        fw = ttk.LabelFrame(p, text="② 固件与升级参数（云端页共用这里的文件 / 版本 / 槽位）")
        fw.pack(fill=tk.X, padx=6, pady=6)
        self.row(fw, 0, "固件 .bin", "firmware", 52)
        ttk.Button(fw, text="浏览…", command=self.pick_firmware).grid(row=0, column=2, padx=4)
        info = ttk.Frame(fw)
        info.grid(row=1, column=0, columnspan=3, sticky=tk.W, padx=6, pady=2)
        self.lbl_fw = ttk.Label(info, text="未选择文件", foreground="#555")
        self.lbl_fw.pack(side=tk.LEFT)
        self.row(fw, 2, "目标版本", "version", 16)
        ttk.Label(fw, text="目标槽位").grid(row=2, column=2, sticky=tk.W, padx=6)
        slot = tk.StringVar(value="A" if self.cfg.get("target_slot") == 0 else "B")
        ttk.Combobox(fw, textvariable=slot, values=["A", "B"], width=6,
                     state="readonly").grid(row=2, column=3, sticky=tk.W)
        self.v["target_slot"] = slot
        self.row(fw, 3, "本地分片字节", "chunk_size", 16,
                 values=["256", "512", "768", "1020"])
        ttk.Label(fw, text="ACK 超时(s)").grid(row=3, column=2, sticky=tk.W, padx=6)
        self.v["ack_timeout"] = tk.StringVar(value=str(self.cfg["ack_timeout"]))
        ttk.Entry(fw, textvariable=self.v["ack_timeout"], width=8).grid(
            row=3, column=3, sticky=tk.W)
        self.row(fw, 4, "重试次数", "retry", 16, values=["1", "2", "3", "5"])
        ttk.Label(fw, text="结果超时(s)").grid(row=4, column=2, sticky=tk.W, padx=6)
        self.v["final_timeout"] = tk.StringVar(value=str(self.cfg["final_timeout"]))
        ttk.Entry(fw, textvariable=self.v["final_timeout"], width=8).grid(
            row=4, column=3, sticky=tk.W)

        act = ttk.LabelFrame(p, text="③ 操作")
        act.pack(fill=tk.X, padx=6, pady=6)
        self.btn_send = ttk.Button(act, text="开始本地升级",
                                   command=lambda: self.start_send(False))
        self.btn_send.pack(side=tk.LEFT, padx=5, pady=6)
        ttk.Button(act, text="中止", command=self.abort).pack(side=tk.LEFT, padx=5)
        ttk.Button(act, text="Ping",
                   command=lambda: self.quick_cmd(CMD_PING)).pack(side=tk.LEFT, padx=5)
        ttk.Button(act, text="查询状态",
                   command=lambda: self.quick_cmd(CMD_GET_STATUS)).pack(side=tk.LEFT, padx=5)
        ttk.Button(act, text="切换 A/B 槽",
                   command=lambda: self.quick_cmd(CMD_SWITCH_SLOT)).pack(side=tk.LEFT, padx=5)
        ttk.Button(act, text="重启设备",
                   command=lambda: self.quick_cmd(CMD_REBOOT)).pack(side=tk.LEFT, padx=5)

        ttk.Label(p, text="提示：STM32 只擦写「非运行槽」，写完回读 CRC32 校验通过才写 trial 元数据；"
                         "Bootloader 试启动 3 次失败自动回滚。串口日志里的 ESP32 纯文本会被自动过滤。",
                  foreground="#666", wraplength=920, justify=tk.LEFT).pack(
                      anchor=tk.W, padx=8, pady=4)

    def build_cloud(self, p):
        c = ttk.LabelFrame(p, text="① OneNET 参数（不使用『远程升级 / 升级包管理』页面）")
        c.pack(fill=tk.X, padx=6, pady=6)
        self.row(c, 0, "产品 ID (pid)", "product_id", 40)
        self.row(c, 1, "设备名称 (topic 用)", "device_name", 40)
        self.row(c, 2, "设备 ID (数字，命令 API 用)", "device_id", 40)
        self.row(c, 3, "设备密钥 / access_key (Base64)", "device_key", 58)
        self.row(c, 4, "API Base", "api_base", 40)
        self.row(c, 5, "命令路径", "command_path", 40)
        self.row(c, 6, "签名方法", "token_method", 16,
                 values=["sha1", "sha256", "md5"])
        ttk.Label(c, text="签名作用域").grid(row=6, column=2, sticky=tk.W, padx=6)
        self.v["token_scope"] = tk.StringVar(value=self.cfg["token_scope"])
        ttk.Combobox(c, textvariable=self.v["token_scope"],
                     values=["device", "product", "user"], width=12,
                     state="readonly").grid(row=6, column=3, sticky=tk.W)
        self.row(c, 7, "签名串顺序", "token_sign_order", 30,
                 values=["et_method_res_version", "version_et_method_res"])

        opt = ttk.LabelFrame(p, text="② 云端分片（OneNET 命令体必须 < 1KB，故上限 700 B）")
        opt.pack(fill=tk.X, padx=6, pady=6)
        self.row(opt, 0, "分片字节", "cloud_chunk_size", 14,
                 values=["256", "384", "512", "640", "700"])
        ttk.Label(opt, text="片间隔(s)").grid(row=0, column=2, sticky=tk.W, padx=6)
        self.v["cloud_interval"] = tk.StringVar(value=str(self.cfg["cloud_interval"]))
        ttk.Entry(opt, textvariable=self.v["cloud_interval"], width=8).grid(
            row=0, column=3, sticky=tk.W)
        self.row(opt, 1, "命令超时(s)", "cloud_cmd_timeout", 14,
                 values=["5", "8", "10", "15", "25"])

        act = ttk.LabelFrame(p, text="③ 操作")
        act.pack(fill=tk.X, padx=6, pady=6)
        self.btn_cloud = ttk.Button(act, text="开始云端升级",
                                    command=lambda: self.start_send(True))
        self.btn_cloud.pack(side=tk.LEFT, padx=5, pady=6)
        for txt, cb in [
            ("测试连通 (PING)", self.do_cloud_ping),
            ("生成 Token", self.do_token),
            ("查看 Topic / 物模型", self.do_show_model),
            ("打开控制台", lambda: webbrowser.open(CONSOLE_URLS["console"])),
        ]:
            ttk.Button(act, text=txt, command=cb).pack(side=tk.LEFT, padx=4)

        self.txt_cloud = tk.Text(p, height=12, wrap=tk.WORD,
                                 font=("Consolas", 9))
        self.txt_cloud.pack(fill=tk.BOTH, expand=True, padx=6, pady=4)

        ttk.Label(p, text="链路：上位机把每片封成 0xAA55 帧 → base64 → 同步命令 API → "
                         "OneNET 只透传 → ESP32 解码后 UART 转给 STM32 → ACK 原路由返回。"
                         "平台不托管固件、不建升级任务、不解析内容。",
                  foreground="#666", wraplength=920, justify=tk.LEFT).pack(
                      anchor=tk.W, padx=8, pady=4)

    # ------------------------------------------------------------ 工具 --
    def log(self, msg: str, tag: str = ""):
        ts = time.strftime("%H:%M:%S")
        self.txt.insert(tk.END, f"[{ts}] {msg}\n", tag)
        self.txt.see(tk.END)

    def ui(self, kind: str, *args):
        self.ui_q.put((kind, args))

    def cloud_out(self, text):
        self.txt_cloud.delete("1.0", tk.END)
        self.txt_cloud.insert("1.0", text)

    def refresh_ports(self):
        if not HAS_SERIAL:
            self.log("未发现 pyserial，跳过串口枚举", "warn")
            return
        try:
            ports = list(serial.tools.list_ports.comports())
        except Exception as e:
            self.log(f"串口枚举失败：{e}", "err")
            return
        self.port_list = [p.device for p in ports]
        cur = self.v["port"].get().split(" - ")[0].strip()
        if not self.port_list:
            self.log("未发现串口，请检查 ESP32 USB 线", "warn")
            self.v["port"].set("")
        else:
            self.v["port"].set(cur if cur in self.port_list else self.port_list[0])
        self.log("可用串口：" + (", ".join(self.port_list) or "无"))

    def pick_firmware(self):
        path = filedialog.askopenfilename(
            title="选择固件 .bin",
            filetypes=[("固件二进制", "*.bin"), ("所有文件", "*.*")])
        if not path:
            return
        try:
            data = open(path, "rb").read()
        except Exception as e:
            self.log(f"读取固件失败：{e}", "err")
            return
        self.v["firmware"].set(path)
        c = crc32(data)
        md5 = hashlib.md5(data).hexdigest()
        self.fw_bytes = data
        self.lbl_fw.config(
            text=f"{os.path.basename(path)}  |  {len(data):,} B  |  "
                 f"CRC32=0x{c:08X}  |  MD5={md5}", foreground="#137333")
        self.log(f"固件已载入：{len(data):,} B, CRC32=0x{c:08X}")

    # ------------------------------------------------------------ 串口 --
    def toggle_port(self):
        if self.ser and self.ser.is_open:
            self.close_port()
            return
        if not HAS_SERIAL:
            messagebox.showerror("缺少依赖",
                                 "未安装 pyserial。\n命令行执行：\n"
                                 "python -m pip install pyserial "
                                 "-i https://mirrors.aliyun.com/pypi/simple")
            return
        port = self.v["port"].get().split(" - ")[0].strip()
        if not port:
            messagebox.showwarning("提示", "请先选择串口")
            return
        try:
            baud = int(self.v["baud"].get())
            self.ser = serial.Serial(port, baud, timeout=0.05, write_timeout=2)
            self.parser.reset()
            self.stop_evt.clear()
            self.reader = threading.Thread(target=self.read_loop, daemon=True)
            self.reader.start()
            self.btn_conn.config(text="关闭串口")
            self.log(f"串口已打开：{port} @ {baud}", "ok")
        except Exception as e:
            self.ser = None
            self.log(f"打开串口失败：{e}", "err")
            messagebox.showerror("串口错误", str(e))

    def close_port(self):
        self.stop_evt.set()
        if self.reader:
            self.reader.join(timeout=0.6)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.btn_conn.config(text="打开串口")
        self.log("串口已关闭")

    def read_loop(self):
        while not self.stop_evt.is_set() and self.ser and self.ser.is_open:
            try:
                data = self.ser.read(512)
                if data:
                    for fr in self.parser.feed(data):
                        self.rx_q.put(fr)
            except Exception:
                break

    def write_frame(self, cmd: int, seq: int, payload: bytes = b"") -> bool:
        if not (self.ser and self.ser.is_open):
            return False
        try:
            raw = build_frame(cmd, seq, payload)
            with self.tx_lock:
                self.ser.write(raw)
                self.ser.flush()
            return True
        except Exception as e:
            self.ui("log", f"串口写入失败：{e}", "err")
            return False

    def wait_ack(self, cmd: int, seq: int, timeout: float) -> tuple[bool, int]:
        self.ack_evt.clear()
        self.ack_data = {}
        self.wait_cmd, self.wait_seq = cmd, seq
        if not self.ack_evt.wait(timeout):
            return False, ST_TIMEOUT
        return self.ack_data.get("ok", False), self.ack_data.get("status", ST_TIMEOUT)

    def handle_frame(self, fr):
        name = fr.cmd_name
        if fr.cmd == CMD_LOG:
            self.log("设备日志: " + fr.payload.decode("utf-8", "replace").strip())
        elif fr.cmd == CMD_WIFI_STATE:
            w = unpack_wifi(fr.payload)
            self.log(f"WiFi 状态: {'已连接' if w.get('connected') else '断开'}  "
                     f"RSSI={w.get('rssi')}dBm  IP={w.get('ip')}", "info")
        elif fr.cmd in (CMD_OTA_RESULT, CMD_OTA_PROGRESS):
            s = unpack_status(fr.payload)
            self.log(f"[{name}] 阶段={s.get('stage_name')} 结果={s.get('result_name')} "
                     f"进度={s.get('progress')}% 当前槽={s.get('slot_name')}",
                     "ok" if s.get("result") == ST_OK else "warn")
            self.ui("progress", s.get("progress", 0),
                    f"{s.get('stage_name')} / {s.get('result_name')}")
        elif fr.cmd == CMD_ERROR:
            self.log(f"设备错误帧: {status_byte(fr.payload)}", "err")
        else:
            self.log(f"收到 {fr} payload={fr.payload[:16].hex()}")

        if fr.is_resp and getattr(self, "wait_cmd", None) == fr.cmd \
                and getattr(self, "wait_seq", None) == fr.seq:
            st = status_byte(fr.payload)
            self.ack_data = {"ok": st == ST_OK, "status": st}
            self.ack_evt.set()

    def poll(self):
        try:
            while True:
                self.handle_frame(self.rx_q.get_nowait())
        except queue.Empty:
            pass
        try:
            while True:
                kind, args = self.ui_q.get_nowait()
                if kind == "log":
                    self.log(args[0], args[1] if len(args) > 1 else "")
                elif kind == "progress":
                    self.pbar["value"] = args[0]
                    self.lbl_prog.config(text=f"{args[0]}%  |  {args[1]}")
                elif kind == "result":
                    ok, name = args
                    self.lbl_result.config(
                        text=("升级成功  " + name) if ok else ("升级失败  " + name),
                        foreground="#137333" if ok else "#C5221F")
                elif kind == "cloud":
                    self.cloud_out(args[0])
                elif kind == "enable":
                    self.btn_send.config(state=tk.NORMAL)
                    self.btn_cloud.config(state=tk.NORMAL)
                    self.sending = False
        except queue.Empty:
            pass
        self.root.after(60, self.poll)

    # ------------------------------------------------------- 升级流程 --
    def quick_cmd(self, cmd: int):
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("提示", "请先打开串口")
            return
        self.write_frame(cmd, int(time.time()) & 0xFFFF)
        self.log(f"已发送命令 {CMD_NAME_LOCAL.get(cmd, hex(cmd))}")

    def abort(self):
        if not self.sending:
            return
        self.abort_evt.set()
        self.log("已请求中止（下一片发送前生效）", "warn")

    def start_send(self, cloud: bool):
        if self.sending:
            return
        try:
            self.collect_config()
            path = self.cfg["firmware"]
            if not path or not os.path.isfile(path):
                messagebox.showwarning("提示", "请先在『本地串口升级』页选择固件 .bin")
                return
            data = open(path, "rb").read()
            version = self.cfg["version"] or "1.0.0"
            slot = int(self.cfg["target_slot"])
            retry = int(self.cfg["retry"])
            timeout = float(self.cfg["ack_timeout"])
            if cloud:
                chunk = min(int(self.cfg["cloud_chunk_size"]), CLOUD_MAX_CHUNK)
                transport = CloudTransport(
                    self.client(), log=lambda m, t="": self.ui("log", m, t),
                    retry=retry, interval=float(self.cfg["cloud_interval"]),
                    ack_timeout=float(self.cfg["cloud_cmd_timeout"]))
                if not self.check_cloud_cfg():
                    return
            else:
                if not (self.ser and self.ser.is_open):
                    messagebox.showwarning("提示", "请先打开串口")
                    return
                chunk = min(int(self.cfg["chunk_size"]), MAX_CHUNK_DATA)
                transport = SerialTransport(self, retry, timeout)
        except Exception as e:
            messagebox.showerror("参数错误", str(e))
            return
        if len(data) > 896 * 1024 - 256:
            messagebox.showerror("固件过大", "超过 896KB 槽位容量（预留 256B 固件头）")
            return
        if not 1 <= chunk <= MAX_CHUNK_DATA:
            messagebox.showerror("分片错误", "分片字节数不合法")
            return

        # STM32 固件是按绝对地址链接的：APP_A.bin 放到 B 槽跑不起来。
        base = os.path.basename(path).upper()
        want = "APP_A" if slot == 0 else "APP_B"
        wrong = "APP_B" if slot == 0 else "APP_A"
        if wrong in base:
            if not messagebox.askyesno(
                    "槽位与固件可能不匹配",
                    f"目标槽位 = {want[-1]}，但文件名看起来是 {wrong}.bin。\n"
                    "STM32 固件按绝对地址链接，APP_A.bin 不能放到 B 槽运行。\n"
                    f"请在 Keil 里切到 {want} Target 重新编译，并选择 {want}.bin。\n\n"
                    "仍要继续吗？"):
                return
        elif want not in base:
            self.log(f"提示：目标槽 {want[-1]}，建议选 {want}.bin；"
                     "若这是手工重定位过的固件可忽略。", "warn")

        self.mode = "cloud" if cloud else "local"
        self.sending = True
        self.abort_evt.clear()
        self.btn_send.config(state=tk.DISABLED)
        self.btn_cloud.config(state=tk.DISABLED)
        self.pbar["value"] = 0
        self.lbl_result.config(text="发送中…", foreground="#1A73E8")
        self.sender = threading.Thread(
            target=self.send_worker, daemon=True,
            args=(data, chunk, timeout, version, slot, transport))
        self.sender.start()

    def send_worker(self, data, chunk, timeout, version, slot, transport):
        c = crc32(data)
        total = len(data)
        nchunks = (total + chunk - 1) // chunk
        seq0 = int(time.time()) & 0xFF00
        mode = "云端(OneNET 中转)" if self.mode == "cloud" else "本地串口"
        try:
            self.ui("log", f"[{mode}] 开始升级：{total:,} B / {nchunks} 片 / "
                           f"每片 {chunk} B / CRC32=0x{c:08X} / "
                           f"版本 {version} / 目标槽 {'AB'[slot]}", "info")

            # 1) START —— 擦除 896KB 很慢，给足超时
            ok, st = transport.request(CMD_OTA_START, seq0,
                                       pack_start(total, c, version, slot),
                                       timeout * 10)
            if not ok:
                raise RuntimeError(f"OTA_START 被拒 (status=0x{st:02X})，"
                                   "目标槽不能是当前运行槽")
            self.ui("log", "设备已擦除非活动槽，开始发送分片", "ok")

            # 2) CHUNKs —— 严格顺序 offset，逐片 ACK + CRC + 重试
            for i in range(nchunks):
                if self.abort_evt.is_set():
                    raise RuntimeError("用户中止")
                off = i * chunk
                seq = (seq0 + i + 1) & 0xFFFF
                ok, st = transport.request(
                    CMD_FW_CHUNK, seq,
                    pack_chunk(off, data[off:off + chunk]), timeout)
                if not ok:
                    raise RuntimeError(f"第 {i} 片 ACK 失败 (status=0x{st:02X})")
                sent = min(off + chunk, total)
                self.ui("progress", min(99, sent * 100 // max(total, 1)),
                        f"已发送 {sent:,}/{total:,} B")

            # 3) END —— 写固件头 + 整片 CRC 校验，同样慢
            ok, st = transport.request(CMD_FW_END,
                                       (seq0 + nchunks + 1) & 0xFFFF,
                                       b"", timeout * 10)
            if not ok:
                raise RuntimeError(f"FW_END 校验失败 (status=0x{st:02X})")
            self.ui("progress", 100, "固件校验通过，等待重启切换")
            self.ui("log", "固件写入并回读校验通过，已写入 trial 元数据", "ok")

            # 4) REBOOT —— 设备立刻复位，不等 ACK
            transport.send_only(CMD_REBOOT, (seq0 + nchunks + 2) & 0xFFFF)
            self.ui("log", "已请求重启：Bootloader 将试启动新槽，"
                           "APP 启动成功后自动确认；失败 3 次回滚", "info")
            self.ui("result", True, "固件已下发 / 等待设备重启确认")
        except Exception as e:
            try:
                transport.send_only(CMD_OTA_ABORT, 0)
            except Exception:
                pass
            self.ui("log", f"升级中止：{e}", "err")
            self.ui("result", False, str(e))
        finally:
            transport.close()
            self.ui("enable", None)

    # ---------------------------------------------------------- 云端页 --
    def client(self) -> OneNetClient:
        self.collect_config()
        return OneNetClient(
            self.cfg["product_id"], self.cfg["device_name"],
            self.cfg["device_key"], device_id=self.cfg["device_id"],
            api_base=self.cfg["api_base"],
            command_path=self.cfg["command_path"],
            token_kwargs={"method": self.cfg["token_method"],
                          "scope": self.cfg["token_scope"],
                          "sign_order": self.cfg["token_sign_order"]})

    def check_cloud_cfg(self) -> bool:
        self.collect_config()
        need = ("product_id", "device_name", "device_id", "device_key")
        miss = [k for k in need if not self.cfg[k]]
        if miss:
            messagebox.showwarning("缺少参数",
                                   "请补全：" + ", ".join(miss) +
                                   "\n注意 device_id 是设备详情里的数字 ID，不是设备名称。")
            return False
        return True

    def do_token(self):
        if not self.check_cloud_cfg():
            return
        t = make_token(self.cfg["product_id"], self.cfg["device_name"],
                       self.cfg["device_key"], method=self.cfg["token_method"],
                       scope=self.cfg["token_scope"],
                       sign_order=self.cfg["token_sign_order"])
        text = json.dumps(t, ensure_ascii=False, indent=2)
        text += ("\n\n把上面 \"token\" 的整段字符串粘贴到：\n"
                 "OTA_ESP32/include/onenet_config.h  ->  ONENET_TOKEN\n"
                 "然后重新编译烧录 ESP32。")
        self.cloud_out(text)
        self.log("Token 已生成；ESP32 也使用同一个 token 作为 MQTT password", "info")

    def do_cloud_ping(self):
        if not self.check_cloud_cfg():
            return
        self.log("正在通过 OneNET 同步命令下发 PING …", "info")
        threading.Thread(target=self._cloud_ping_worker, daemon=True).start()

    def _cloud_ping_worker(self):
        tr = CloudTransport(self.client(),
                            log=lambda m, t="": self.ui("log", m, t),
                            retry=1, interval=0.0, ack_timeout=5)
        ok, st = tr.request(CMD_PING, int(time.time()) & 0xFFFF, b"", timeout=5)
        if ok:
            self.ui("log", "云端链路连通：STM32 已通过 OneNET 回 ACK", "ok")
            self.ui("cloud", "PING OK\n\n设备在线、命令 topic 订阅正常、"
                             "response topic 应答正常。可以开始云端升级。")
        else:
            self.ui("log", f"云端 PING 失败 (status=0x{st:02X})", "err")

    def do_show_model(self):
        self.collect_config()
        pid, dev = self.cfg["product_id"], self.cfg["device_name"]
        try:
            with open(os.path.join(HERE, "thingmodel.json"), "r",
                      encoding="utf-8") as f:
                model = json.load(f)
        except Exception as e:
            model = {"错误": str(e)}
        out = {
            "本方案用到的 topic": topic_map(pid or "{pid}", dev or "{dev}"),
            "同步命令 API": (f"POST {self.cfg['api_base']}"
                             f"{self.cfg['command_path']}"
                             f"?device_id={self.cfg['device_id'] or '{device_id}'}"
                             "&timeout={5..30}"),
            "物模型清单": model,
            "属性上报示例": device_properties_payload(),
        }
        self.cloud_out(json.dumps(out, ensure_ascii=False, indent=2))

    # ------------------------------------------------------------ 其它 --
    def selftest(self):
        import subprocess
        import sys
        try:
            r = subprocess.run([sys.executable, os.path.join(HERE, "ota_framer.py")],
                               capture_output=True, text=True, timeout=10)
            self.log(r.stdout.strip() or r.stderr.strip(),
                     "ok" if r.returncode == 0 else "err")
        except Exception as e:
            self.log(f"自测失败：{e}", "err")

    def show_help(self):
        self.log("双击启动：OTA_PC_Tool/启动上位机.vbs（一键版，无黑框）；"
                 "本高级模式从一键版『高级模式』按钮进入", "info")
        self.log("ESP32 参数：include/onenet_config.h（WiFi / 产品ID / 设备名 / token）", "info")
        self.log("STM32：Bootloader IROM 0x08000000/0x20000；"
                 "APP_A 0x08020000、APP_B 0x08100000，各 0xE0000", "info")
        self.log("物模型只要 6 个只读属性，不要碰『远程升级』页面", "info")

    def on_close(self):
        self.save_config()
        self.stop_evt.set()
        self.abort_evt.set()
        if self.ser:
            self.close_port()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    App(root)
    root.mainloop()
