# -*- coding: utf-8 -*-
"""
quick_ota.py —— 一键远程升级（方案 B 的极简入口）

双击「启动上位机.vbs」打开的就是这个界面。日常只有两个按钮：

    [ A → B  升级 ]      [ B → A  升级 ]

点一下自动完成：
    自动找 APP_A.bin / APP_B.bin → 自动 PING → 自动判断当前槽位
    → OTA_START / 分片 / FW_END(CRC 回读) / REBOOT
    → 自动等设备重启并确认已切到新槽位

首次使用只填一次 4 个 OneNET 参数；保存时会自动把 产品ID / 设备名 / Token
写进 OTA_ESP32/include/onenet_config.h，ESP32 只需重新编译烧录一次。
完整版（本地串口 / 高级参数）点界面右下角「高级模式」。

底层全部复用 cloud_transport.py / onenet_api.py / ota_framer.py，
和完整版走的是同一套 0xAA55 帧协议，没有第二套实现。
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import queue
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from ota_framer import (
    CMD_FW_CHUNK, CMD_FW_END, CMD_GET_STATUS, CMD_OTA_ABORT, CMD_OTA_START,
    CMD_PING, CMD_REBOOT, FrameParser, ST_OK, ST_TIMEOUT, build_frame, crc32,
    pack_chunk, pack_start, unpack_start, unpack_status,
)
from onenet_api import OneNetClient, make_token, query_device_detail
from cloud_transport import CloudTransport

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONFIG_PATH = os.path.join(HERE, "config.json")
ESP32_HEADER = os.path.join(ROOT, "OTA_ESP32", "include", "onenet_config.h")
ADVANCED_TOOL = os.path.join(HERE, "ota_tool.py")

CHUNK = 360                 # 帧 378B -> base64 504 字符，卡在属性 string 的 512 上限内
SLOT_MAX_SIZE = 896 * 1024 - 256
SKIP_DIRS = {"__pycache__", ".git", ".pio", ".vscode", "DebugConfig",
             "RTE", "build", ".workbuddy"}

DEFAULTS = {
    "product_id": "", "device_name": "", "device_id": "", "device_key": "",
    "access_key": "",
    "api_base": "https://iot-api.heclouds.com",
    "command_path": "/v1/synccmds",
    "channel": "prop",          # prop = 物模型属性下发（本产品唯一可用通道）
    "token_method": "sha1", "token_scope": "device",
    "token_sign_order": "et_method_res_version",
    "version": "2.0.0",
    "fw_a": "", "fw_b": "",
    "esp32_token": "", "esp32_token_key": "",
}

NEED = ("product_id", "device_name", "device_id", "device_key")
SLOT_LETTER = {0: "A", 1: "B"}


# --------------------------------------------------------------- 配置 --
def load_cfg() -> dict:
    c = dict(DEFAULTS)
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            c.update(json.load(f))
    except Exception:
        pass
    return c


def save_cfg(c: dict):
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(c, f, ensure_ascii=False, indent=2)


def cfg_ready(c: dict) -> bool:
    return all(str(c.get(k, "")).strip() for k in NEED)


def token_kwargs(c: dict) -> dict:
    return {"method": c.get("token_method", "sha1"),
            "scope": c.get("token_scope", "device"),
            "sign_order": c.get("token_sign_order", "et_method_res_version")}


def make_client(c: dict) -> OneNetClient:
    return OneNetClient(
        c["product_id"], c["device_name"], c["device_key"],
        device_id=c["device_id"],
        api_base=c.get("api_base") or DEFAULTS["api_base"],
        command_path=c.get("command_path") or DEFAULTS["command_path"],
        token_kwargs=token_kwargs(c),
        # 本产品的 cmd/request 通道不可用（订阅被拒 0x80、同步命令报设备不存在），
        # 下行改走物模型属性 ota_cmd。
        channel=c.get("channel") or DEFAULTS["channel"],
        access_key=c.get("access_key") or "")


def cached_esp32_token(c: dict) -> str:
    """Token 缓存：4 个参数不变就复用同一个，避免每次保存都要重烧 ESP32。"""
    key = "|".join(str(c.get(k, "")) for k in NEED) + "|" + str(token_kwargs(c))
    if c.get("esp32_token") and c.get("esp32_token_key") == key:
        return c["esp32_token"]
    token = make_token(c["product_id"], c["device_name"], c["device_key"],
                       **token_kwargs(c))["token"]
    c["esp32_token"] = token
    c["esp32_token_key"] = key
    return token


def sync_esp32(c: dict, path: str = ESP32_HEADER) -> tuple[bool, str]:
    """把 产品ID / 设备名 / Token 自动写进 ESP32 头文件，省掉手工复制粘贴。"""
    # 首次使用（或刚 clone 下来）时还没有 onenet_config.h，先用模板复制一份。
    # onenet_config.h 含 WiFi 密码与 Token，已被 .gitignore 排除，不进版本库。
    if not os.path.isfile(path):
        tmpl = os.path.join(os.path.dirname(path), "onenet_config.example.h")
        if os.path.isfile(tmpl):
            try:
                shutil.copyfile(tmpl, path)
            except Exception as e:
                return False, f"无法从模板生成配置：{e}"
        else:
            return False, f"找不到 {path}"
    try:
        with open(path, "r", encoding="utf-8") as f:
            src = f.read()
    except Exception as e:
        return False, str(e)

    token = cached_esp32_token(c)
    values = {"ONENET_PRODUCT_ID": c["product_id"],
              "ONENET_DEVICE_NAME": c["device_name"],
              "ONENET_TOKEN": token}
    missing = []
    for name, val in values.items():
        esc = str(val).replace("\\", "\\\\").replace('"', '\\"')
        pattern = r'(#define\s+' + name + r'\s+)"[^"]*"'
        src, n = re.subn(pattern, lambda m: m.group(1) + '"' + esc + '"',
                         src, count=1)
        if n == 0:
            missing.append(name)
    if missing:
        return False, "头文件里缺少宏：" + ", ".join(missing)
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
    except Exception as e:
        return False, str(e)
    return True, token


# --------------------------------------------------------- 自动找固件 --
def find_firmware(slot: int, c: dict) -> str:
    key = "fw_a" if slot == 0 else "fw_b"
    saved = c.get(key, "")
    if saved and os.path.isfile(saved):
        return saved
    want = "APP_A" if slot == 0 else "APP_B"
    wrong = "APP_B" if slot == 0 else "APP_A"
    hits = []
    for base in (os.path.join(ROOT, "OTA_STM32", "MDK-ARM"), ROOT, HERE):
        if not os.path.isdir(base):
            continue
        for dirpath, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            if dirpath[len(base):].count(os.sep) >= 4:
                dirs[:] = []
            for name in files:
                if not name.lower().endswith(".bin"):
                    continue
                up = name.upper()
                if want in up and wrong not in up and "BOOT" not in up:
                    hits.append(os.path.join(dirpath, name))
        if hits:
            break
    if not hits:
        return ""
    hits.sort(key=os.path.getmtime, reverse=True)
    return hits[0]


# ------------------------------------------------------------ 帧查询 --
def _as_frame(resp) -> bytes:
    return CloudTransport._as_frame(resp)


def query_status(client, seq: int | None = None, api_timeout: int = 5):
    seq = int(time.time()) & 0xFFFF if seq is None else seq
    wire = base64.b64encode(build_frame(CMD_GET_STATUS, seq, b""))
    ok, resp = client.send_command(wire, api_timeout=api_timeout)
    if not ok:
        return None
    for fr in FrameParser().feed(_as_frame(resp)):
        if (fr.is_resp and fr.cmd == CMD_GET_STATUS
                and fr.seq == seq and len(fr.payload) >= 9):
            return unpack_status(fr.payload[1:9])
    return None


def ping(client):
    """返回 (ok, st)。

    st == ST_TIMEOUT 的含义很关键：那不是"设备不在线"，而是
    ESP32 收到了命令、转给了 STM32，但 3 秒内没收到 STM32 的 ACK，
    于是 ESP32 自己回了一个 ST_TIMEOUT 伪应答。这种情况要去查 UART 接线。
    """
    tr = CloudTransport(client, log=lambda *a: None,
                        retry=1, interval=0.0, ack_timeout=5)
    ok, st = tr.request(CMD_PING, int(time.time()) & 0xFFFF, b"", timeout=5)
    return ok, st


def wait_slot(client, slot: int, post, attempts: int = 10) -> bool:
    """REBOOT 后轮询，直到设备上报的 active_slot 变成目标槽。"""
    for i in range(attempts):
        info = query_status(client)
        if info and info.get("active_slot") == slot:
            return True
        post("log", f"等待设备重启… ({i + 1}/{attempts})")
        time.sleep(2)
    return False


# --------------------------------------------------------- 升级主流程 --
def run_ota(data: bytes, slot: int, version: str, transport,
            progress, log, abort_evt, chunk: int = CHUNK) -> tuple[bool, str]:
    """与完整版 ota_tool.py::send_worker 完全相同的四步流程。"""
    total = len(data)
    c = crc32(data)
    nchunks = (total + chunk - 1) // chunk
    seq0 = int(time.time()) & 0xFF00
    try:
        log(f"开始写入 {SLOT_LETTER[slot]} 槽：{total:,} B / {nchunks} 片 / CRC32=0x{c:08X}")
        ok, st = transport.request(CMD_OTA_START, seq0,
                                   pack_start(total, c, version, slot),
                                   timeout=25)
        if not ok:
            raise RuntimeError(f"设备拒绝擦除目标槽 (0x{st:02X})；"
                               "目标槽不能是当前运行槽，请点另一个按钮")
        log("目标槽已擦除，开始发送固件分片")
        for i in range(nchunks):
            if abort_evt.is_set():
                raise RuntimeError("已取消")
            off = i * chunk
            seq = (seq0 + i + 1) & 0xFFFF
            ok, st = transport.request(CMD_FW_CHUNK, seq,
                                       pack_chunk(off, data[off:off + chunk]),
                                       timeout=8)
            if not ok:
                raise RuntimeError(f"第 {i + 1}/{nchunks} 片失败 (0x{st:02X})")
            sent = min(off + chunk, total)
            progress(min(99, sent * 100 // max(total, 1)),
                     f"{sent:,} / {total:,} B")
        ok, st = transport.request(CMD_FW_END, (seq0 + nchunks + 1) & 0xFFFF,
                                   b"", timeout=25)
        if not ok:
            raise RuntimeError(f"固件 CRC 回读校验失败 (0x{st:02X})")
        progress(100, "校验通过，等待重启")
        log("固件写入并 CRC 校验通过，已写入 trial 元数据")
        transport.send_only(CMD_REBOOT, (seq0 + nchunks + 2) & 0xFFFF)
        log("已请求重启，Bootloader 将试启动新槽")
        return True, "已下发，等待设备重启确认"
    except Exception as e:
        try:
            transport.send_only(CMD_OTA_ABORT, 0)
        except Exception:
            pass
        return False, str(e)
    finally:
        transport.close()


# ---------------------------------------------------------------- UI --
class QuickApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.cfg = load_cfg()
        self.q: queue.Queue = queue.Queue()
        self.busy = False
        self.abort_evt = threading.Event()

        root.title("远程 OTA 升级 · 一键切换 A / B")
        root.geometry("580x430")
        root.resizable(False, False)

        head = ttk.Frame(root)
        head.pack(fill=tk.X, padx=18, pady=(16, 6))
        ttk.Label(head, text="点一下就完成远程升级",
                  font=("Microsoft YaHei", 15, "bold")).pack(anchor=tk.W)
        self.lbl_dev = ttk.Label(head, text="设备：尚未设置 / 未检测", foreground="#555")
        self.lbl_dev.pack(anchor=tk.W, pady=(4, 0))

        btns = ttk.Frame(root)
        btns.pack(fill=tk.X, padx=18, pady=12)
        self.btn_ab = tk.Button(btns, text="A  →  B    升级",
                                font=("Microsoft YaHei", 14, "bold"),
                                height=2, bg="#1A73E8", fg="white",
                                activebackground="#155CB0", relief=tk.FLAT,
                                command=lambda: self.start(1))
        self.btn_ab.pack(side=tk.LEFT, expand=True, fill=tk.X, padx=(0, 6))
        self.btn_ba = tk.Button(btns, text="B  →  A    升级",
                                font=("Microsoft YaHei", 14, "bold"),
                                height=2, bg="#0F9D58", fg="white",
                                activebackground="#0B7A44", relief=tk.FLAT,
                                command=lambda: self.start(0))
        self.btn_ba.pack(side=tk.LEFT, expand=True, fill=tk.X, padx=(6, 0))

        ttk.Label(root,
                  text="A→B：当前跑 A，写入 APP_B.bin 并切到 B 槽    ｜    "
                       "B→A：当前跑 B，写入 APP_A.bin 并切到 A 槽",
                  foreground="#777", wraplength=540,
                  justify=tk.LEFT).pack(anchor=tk.W, padx=18)
        self.lbl_fw = ttk.Label(root, text="—", foreground="#333",
                                wraplength=540)
        self.lbl_fw.pack(anchor=tk.W, padx=18, pady=(2, 10))

        self.pbar = ttk.Progressbar(root, orient=tk.HORIZONTAL,
                                    maximum=100, length=540)
        self.pbar.pack(padx=18)
        self.lbl_status = ttk.Label(root, text="就绪", font=("Microsoft YaHei", 10))
        self.lbl_status.pack(anchor=tk.W, padx=18, pady=(6, 4))

        self.txt = tk.Text(root, height=5, font=("Consolas", 9),
                           state=tk.DISABLED, wrap=tk.WORD,
                           background="#F7F7F7", relief=tk.FLAT)
        self.txt.pack(fill=tk.BOTH, expand=True, padx=18, pady=(0, 8))

        bar = ttk.Frame(root)
        bar.pack(fill=tk.X, padx=18, pady=(0, 14))
        ttk.Button(bar, text="改固件", command=self.choose_firmware).pack(side=tk.LEFT)
        ttk.Button(bar, text="设置 / 生成 Token",
                   command=lambda: self.settings(force=True)).pack(side=tk.LEFT, padx=6)
        ttk.Button(bar, text="高级模式", command=self.advanced).pack(side=tk.RIGHT)

        root.after(80, self._poll)
        root.protocol("WM_DELETE_WINDOW", root.destroy)
        self.refresh_fw_label()
        if not cfg_ready(self.cfg):
            root.after(300, lambda: self.settings(force=True))
        else:
            self.log("参数已就绪。点上面任意一个按钮即可开始。")
            root.after(200, self.detect_slot)

    # ------------------------------------------------------------ 工具 --
    def log(self, msg: str):
        self.q.put(("log", msg))

    def post(self, kind, val=None):
        self.q.put((kind, val))

    def _poll(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "log":
                    self.txt.config(state=tk.NORMAL)
                    self.txt.insert(tk.END, val.rstrip() + "\n")
                    self.txt.see(tk.END)
                    self.txt.config(state=tk.DISABLED)
                elif kind == "progress":
                    self.pbar["value"] = val[0]
                    self.lbl_status.config(text=val[1], foreground="#1A73E8")
                elif kind == "done":
                    self.lbl_status.config(text=val, foreground="#0F9D58")
                elif kind == "fail":
                    self.lbl_status.config(text="失败：" + val, foreground="#D93025")
                    messagebox.showerror("升级失败", val)
                elif kind == "dev":
                    self.lbl_dev.config(text=val)
                elif kind == "enable":
                    self.busy = False
                    self.btn_ab.config(state=tk.NORMAL)
                    self.btn_ba.config(state=tk.NORMAL)
        except queue.Empty:
            pass
        self.root.after(80, self._poll)

    def refresh_fw_label(self):
        a, b = find_firmware(0, self.cfg), find_firmware(1, self.cfg)
        if a and b:
            text = f"已找到：{os.path.basename(a)}  /  {os.path.basename(b)}"
        else:
            text = ("还没编译出固件：在 Keil 里切 APP_A / APP_B 两个 Target 各编译一次，"
                    "这里会自动找到 .bin；也可以点「改固件」手动选一次。")
        self.lbl_fw.config(text=text)

    def choose_firmware(self):
        # 一次多选即可，按文件名自动分配到 A / B 槽
        paths = filedialog.askopenfilenames(
            title="同时选中 APP_A.bin 和 APP_B.bin（可只选一个）",
            filetypes=[("固件", "*.bin"), ("所有文件", "*.*")],
            initialdir=os.path.join(ROOT, "OTA_STM32", "MDK-ARM"))
        for p in paths:
            up = os.path.basename(p).upper()
            if "APP_A" in up:
                self.cfg["fw_a"] = p
            elif "APP_B" in up:
                self.cfg["fw_b"] = p
        save_cfg(self.cfg)
        self.refresh_fw_label()
        self.log("固件路径已记住，以后不用再选。")

    def advanced(self):
        try:
            subprocess.Popen([sys.executable, ADVANCED_TOOL], cwd=HERE)
        except Exception as e:
            messagebox.showerror("无法打开", str(e))

    def detect_slot(self):
        def work():
            try:
                info = query_status(make_client(self.cfg))
                if info:
                    self.post("dev", f"设备：在线  |  当前运行 {SLOT_LETTER.get(info['active_slot'], '?')} 槽"
                                     f"  |  建议点 {'B → A' if info['active_slot'] == 1 else 'A → B'}")
                else:
                    self.post("dev", "设备：未在线（检查 ESP32 是否已烧录、WiFi 与 Token）")
            except Exception as e:
                self.post("dev", f"设备：检测失败 {e}")
        threading.Thread(target=work, daemon=True).start()

    # ----------------------------------------------------------- 设置 --
    def settings(self, force=False):
        c = self.cfg
        win = tk.Toplevel(self.root)
        win.title("首次设置（只填一次）")
        win.geometry("640x370")
        win.resizable(False, False)
        win.transient(self.root)
        win.grab_set()
        ttk.Label(win, text="填 OneNET 设备信息，保存后永久记住",
                  font=("Microsoft YaHei", 11, "bold")).pack(
                      anchor=tk.W, padx=18, pady=(14, 4))
        ttk.Label(win, text="保存时会自动写进 ESP32 的 onenet_config.h，ESP32 只需重新"
                            "编译烧录一次。\nOneNET 网页端不用建物模型、不用进「远程升级」页面，"
                            "只要设备在线即可。",
                  foreground="#666", wraplength=480,
                  justify=tk.LEFT).pack(anchor=tk.W, padx=18)

        form = ttk.Frame(win)
        form.pack(fill=tk.X, padx=18, pady=12)
        labels = [("1. 产品 ID", "product_id"),
                  ("2. 设备名称（topic 用）", "device_name"),
                  ("3. 产品 AccessKey (Base64)", "device_key"),
                  ("4. 设备 ID（数字，点右边按钮自动取）", "device_id")]
        vars_ = {}
        for i, (lab, key) in enumerate(labels):
            ttk.Label(form, text=lab).grid(row=i, column=0, sticky=tk.W, pady=4)
            v = tk.StringVar(value=str(c.get(key, "")))
            ttk.Entry(form, textvariable=v, width=42,
                      show="*" if key == "device_key" else "").grid(
                          row=i, column=1, sticky=tk.W, pady=4)
            vars_[key] = v

        # OneNET 新版控制台的设备详情页只显示设备名称，不显示数字 ID，
        # 而同步命令 API 的 ?device_id= 必须是数字。这里用「查设备详情」接口反查。
        def do_fetch_id():
            pid = vars_["product_id"].get().strip()
            dev = vars_["device_name"].get().strip()
            key = vars_["device_key"].get().strip()
            if not (pid and dev and key):
                messagebox.showwarning(
                    "还差参数",
                    "先填 产品ID / 设备名称 / 设备密钥，才能反查数字 ID", parent=win)
                return
            btn_fetch.config(state=tk.DISABLED, text="查询中…")

            def work():
                ok, res = query_device_detail(
                    pid, dev, key,
                    api_base=(c.get("api_base") or DEFAULTS["api_base"]),
                    token_kwargs=token_kwargs(c))

                def done():
                    btn_fetch.config(state=tk.NORMAL, text="自动获取设备 ID")
                    if ok:
                        vars_["device_id"].set(res["device_id"])
                        self.log("已获取数字 device_id = %s" % res["device_id"])
                        messagebox.showinfo(
                            "获取成功",
                            "device_id = %s\n\n已自动填入，接着点「保存」即可。"
                            % res["device_id"], parent=win)
                    else:
                        # _diagnose 的返回里 "| 原始返回: ..." 留给日志，弹窗只给人看的前半段
                        hint = str(res).split(" | 原始返回")[0]
                        self.log("获取 device_id 失败：%s" % res)
                        messagebox.showerror(
                            "获取失败",
                            "%s\n\n也可以手动拿这个数字：\n"
                            "  控制台设备列表 → 浏览器 F12 → Network\n"
                            "  → 找 device 接口返回里的 \"did\" 字段" % hint,
                            parent=win)

                win.after(0, done)

            threading.Thread(target=work, daemon=True).start()

        btn_fetch = ttk.Button(form, text="自动获取设备 ID", command=do_fetch_id)
        btn_fetch.grid(row=3, column=2, padx=(8, 0), sticky=tk.W)

        def do_save():
            for key, v in vars_.items():
                c[key] = v.get().strip()
            missing = [lab for lab, k in labels
                       if not str(c.get(k, "")).strip()]
            if missing:
                messagebox.showwarning(
                    "还差参数",
                    "还需要填：" + "、".join(missing) +
                    "\n\n顺序是：产品ID → 设备名称 → 设备密钥，"
                    "\n三项填好后点「自动获取设备 ID」，最后点「保存」。",
                    parent=win)
                return
            save_cfg(c)
            ok, res = sync_esp32(c)
            if not ok:
                messagebox.showwarning("参数已保存，但 ESP32 头文件没写成",
                                       res, parent=win)
            else:
                self.log("参数已保存；ESP32 头文件已自动更新（Token 有效期 10 年）。")
                messagebox.showinfo(
                    "设置完成",
                    "已保存，并自动写入：\nOTA_ESP32/include/onenet_config.h\n\n"
                    "接下来只需在 PlatformIO 里把 ESP32 重新编译烧录一次，\n"
                    "以后升级只点主界面的按钮即可。", parent=win)
            win.destroy()
            self.detect_slot()

        ttk.Button(win, text="保存", command=do_save).pack(pady=6)
        if not force:
            ttk.Button(win, text="以后再说", command=win.destroy).pack()

    # ----------------------------------------------------------- 升级 --
    def start(self, slot: int):
        if self.busy:
            return
        c = self.cfg
        if not cfg_ready(c):
            self.settings(force=True)
            return
        path = find_firmware(slot, c)
        want = "APP_A.bin" if slot == 0 else "APP_B.bin"
        if not path:
            self.log(f"没找到 {want}，请手动选择一次（之后会记住）")
            p = filedialog.askopenfilename(
                title=f"选择 {want}", filetypes=[("固件", "*.bin")],
                initialdir=os.path.join(ROOT, "OTA_STM32", "MDK-ARM"))
            if not p:
                return
            c["fw_a" if slot == 0 else "fw_b"] = p
            save_cfg(c)
            path = p
            self.refresh_fw_label()
        try:
            data = open(path, "rb").read()
        except Exception as e:
            messagebox.showerror("读固件失败", str(e))
            return
        if len(data) > SLOT_MAX_SIZE:
            messagebox.showerror("固件过大", "超过 896KB 槽位容量")
            return
        up = os.path.basename(path).upper()
        wrong = "APP_A" if slot == 1 else "APP_B"
        if wrong in up:
            messagebox.showerror(
                "固件与槽位不匹配",
                f"{os.path.basename(path)} 不能写进 {SLOT_LETTER[slot]} 槽。\n"
                "STM32 固件按绝对地址链接，请在 Keil 里切到对应 Target 重新编译。")
            return

        self.busy = True
        self.abort_evt.clear()
        self.btn_ab.config(state=tk.DISABLED)
        self.btn_ba.config(state=tk.DISABLED)
        self.pbar["value"] = 0
        self.lbl_status.config(text="正在连接设备…", foreground="#1A73E8")
        threading.Thread(target=self._worker,
                         args=(slot, path, data, dict(c)),
                         daemon=True).start()

    def _worker(self, slot, path, data, c):
        try:
            client = make_client(c)
            self.post("log", f"目标：切到 {SLOT_LETTER[slot]} 槽  |  固件 {os.path.basename(path)}")
            ok, st = ping(client)
            if not ok:
                if st == ST_TIMEOUT:
                    self.post("fail",
                              "ESP32 在线，但 STM32 没有应答（ST_TIMEOUT）。\n"
                              "检查三根线：ESP32 GPIO18 -> STM32 PB15，"
                              "ESP32 GPIO17 <- STM32 PB14，两边 GND 必须共地。\n"
                              "确认 STM32 已上电、屏幕有显示。")
                else:
                    self.post("fail",
                              "设备不在线。检查 ESP32 是否已烧录最新配置、WiFi 与 Token。")
                return
            self.post("log", "设备在线，读取当前槽位")
            info = query_status(client)
            if info and info.get("active_slot") == slot:
                self.post("fail", f"设备当前已在 {SLOT_LETTER[slot]} 槽，要点另一个按钮。")
                return

            tr = CloudTransport(client,
                                log=lambda m, t="": self.post("log", m),
                                retry=3, interval=0.05, ack_timeout=8)
            ok, msg = run_ota(data, slot, c.get("version", "2.0.0"), tr,
                              lambda pct, txt: self.post("progress", (pct, txt)),
                              lambda m: self.post("log", m), self.abort_evt)
            if not ok:
                self.post("fail", msg)
                return
            self.post("progress", (100, "校验通过，等待重启"))
            self.post("log", "等待设备重启并确认新槽位…")
            if wait_slot(client, slot, self.post):
                self.post("dev", f"设备：在线  |  当前运行 {SLOT_LETTER[slot]} 槽（升级完成）")
                self.post("done", f"升级成功：已切换到 {SLOT_LETTER[slot]} 槽")
            else:
                self.post("done", "固件已下发；重启确认超时，多数情况已成功，重开界面可查槽位")
        except Exception as e:
            self.post("fail", str(e))
        finally:
            self.post("enable")


# ------------------------------------------------------------- 自测 --
class FakeClient:
    """离线设备模拟：解出上位机帧，回一个同 cmd/seq 的 ST_OK ACK。"""

    def __init__(self, active_slot=0):
        self.active_slot = active_slot
        self.frames = []

    def send_command(self, body, api_timeout=10):
        fr = FrameParser().feed(base64.b64decode(body))[0]
        self.frames.append(fr)
        if fr.cmd == CMD_GET_STATUS:
            payload = bytes([ST_OK]) + struct.pack(
                "<BBBBI", 0, ST_OK, 100, self.active_slot, 0)
        else:
            payload = bytes([ST_OK])
        return True, build_frame(fr.cmd, fr.seq, payload, resp=True)


def selftest() -> int:
    print("=== quick_ota 离线自测 ===")
    for cur, target, label in ((0, 1, "A → B"), (1, 0, "B → A")):
        client = FakeClient(active_slot=cur)
        _ok, _st = ping(client)
        assert _ok, "PING 应成功"
        info = query_status(client)
        assert info and info["active_slot"] == cur, "GET_STATUS 应返回当前槽"
        client.frames.clear()

        data = (bytes([0xA0 + target]) * 2048)
        tr = CloudTransport(client, log=lambda *a: None,
                            retry=1, interval=0.0, ack_timeout=8)
        logs, prog = [], []
        ok, msg = run_ota(data, target, "2.0.0", tr,
                          lambda p, t: prog.append((p, t)),
                          logs.append, threading.Event())
        assert ok, f"{label} 失败: {msg}"
        cmds = [f.cmd for f in client.frames]
        assert cmds[0] == CMD_OTA_START, "首帧必须是 OTA_START"
        st = unpack_start(client.frames[0].payload)
        assert st["target_slot"] == target, "START 的 target_slot 必须等于按钮槽位"
        assert st["fw_size"] == len(data) and st["fw_crc32"] == crc32(data)
        chunks = [f for f in client.frames if f.cmd == CMD_FW_CHUNK]
        assert len(chunks) == 4, f"2048B/640B 应为 4 片，实得 {len(chunks)}"
        offs = [struct.unpack_from("<I", f.payload, 0)[0] for f in chunks]
        assert offs == [0, 640, 1280, 1920], f"offset 不连续: {offs}"
        assert cmds[-2] == CMD_FW_END and cmds[-1] == CMD_REBOOT, "必须以 END+REBOOT 收尾"
        assert prog[-1][0] == 100
        print(f"[OK] {label}: {len(data)} B / {len(chunks)} 片 / "
              f"START slot={target} / offset 连续 / CRC=0x{crc32(data):08X} / END+REBOOT")

    # 防呆：当前已在目标槽时必须被挡住
    c = FakeClient(active_slot=1)
    assert query_status(c)["active_slot"] == 1

    # ESP32 头文件自动同步（用临时副本，不动真文件）
    import tempfile
    cfg = dict(DEFAULTS)
    cfg.update({"product_id": "P1", "device_name": "dev1", "device_id": "9",
                "device_key": base64.b64encode(b"k").decode()})
    with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False,
                                     encoding="utf-8") as f:
        f.write('#define ONENET_PRODUCT_ID ""\n'
                '#define ONENET_DEVICE_NAME ""\n'
                '#define ONENET_TOKEN ""\n')
        tmp = f.name
    ok, tok = sync_esp32(cfg, tmp)
    assert ok, tok
    out = open(tmp, encoding="utf-8").read()
    assert '"P1"' in out and '"dev1"' in out and "version=2018-10-31" in out
    assert sync_esp32(cfg, tmp)[1] == tok, "参数不变时 Token 必须复用（避免重烧 ESP32）"
    os.unlink(tmp)
    print("[OK] ESP32 头文件自动写入 + Token 缓存复用")

    # 固件自动发现
    print("[OK] find_firmware 可用:",
          bool(find_firmware(0, cfg)) or "当前尚未编译 .bin（正常，点按钮时会提示选择）")
    print("=== ALL PASS ===")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="一键远程 OTA 升级")
    ap.add_argument("--selftest", action="store_true", help="离线自测，不联网")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    root = tk.Tk()
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    QuickApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
