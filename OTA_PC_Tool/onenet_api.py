# -*- coding: utf-8 -*-
"""
onenet_api.py —— 方案 B：OneNET 只当哑管道 / 中转服务器

跟标准 OTA 的区别（重要）：
  * 不在平台上传升级包、不创建升级任务、不开通「远程升级」增值服务；
  * 只用【同步设备命令 API】把自定义字节透传给在线设备：
        POST {api_base}/v1/synccmds?device_id={数字设备ID}&timeout={5..30}
        Header: Authorization: <token>
        Body  : 自定义内容（< 1KB），平台原样放进 MQTT 的
                $sys/{pid}/{dev}/cmd/request/{cmdid} payload
  * 设备在 $sys/{pid}/{dev}/cmd/response/{cmdid} 回复（< 1KB），
    平台把应答 base64 编码后放进 data.cmd_resp 返回给本次 HTTP 调用。

官方文档：
  https://open.iot.10086.cn/doc/mqtt/book/application-develop/list/synccmds.html

只依赖 Python 标准库，不需要 requests / paho-mqtt。
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import time
import urllib.parse
import urllib.request
import urllib.error
from typing import Optional

API_BASE = "https://iot-api.heclouds.com"
COMMAND_PATH = "/v1/synccmds"
# 物模型属性下发（本产品的唯一可用下行通道）
PROPERTY_SET_PATH = "/thingmodel/set-device-property"
MQTT_HOST = "mqtts.heclouds.com"
MQTT_PORT = 1883

# 下行通道二选一：
#   "prop" —— POST /thingmodel/set-device-property，需要【产品 AccessKey】+ product scope
#   "cmd"  —— POST /v1/synccmds，需要【设备 sec_key】+ device scope
# 实测结论：物模型产品（data_pt=1，属性全只读转可写后）只能走 prop；
# cmd 通道会报 errno 12 "设备不存在"，且 cmd/request/+ 订阅被平台拒 0x80。
DEFAULT_CHANNEL = "prop"

CONSOLE_URLS = {
    "home": "https://open.iot.10086.cn/",
    "console": "https://open.iot.10086.cn/console/",
    "device": "https://open.iot.10086.cn/console/device",
}

ERRNO_TEXT = {
    12: "设备不存在（检查 device_id，注意是数字 ID 不是设备名称）",
    13: "设备不在线（ESP32 未连上 OneNET，检查 token / WiFi / 订阅 topic）",
    14: "设备命令应答超时",
    15: "设备命令应答超时",
    16: "命令请求体超过 1KB",
    17: "设备应答超过 1KB",
    401: "鉴权失败（Authorization token 不对或已过期）",
}


def make_token(product_id: str, device_name: str, key_b64: str,
               method: str = "sha1", version: str = "2018-10-31",
               lifetime_days: int = 3650,
               sign_order: str = "et_method_res_version",
               scope: str = "device") -> dict:
    """
    生成 OneNET Authorization token。

    key_b64 : 设备密钥 / 产品 access_key（控制台里通常给的是 Base64 串）
    scope   : device -> res=products/{pid}/devices/{dev}
              product-> res=products/{pid}
    sign_order : 不同版本文档的签名串字段顺序不同，连不上时切换试试：
                 et_method_res_version : f"{et}\\n{method}\\n{res}\\n{version}"
                 version_et_method_res: f"{version}\\n{et}\\n{method}\\n{res}\\n"

    ESP32 侧也用这同一个 token（粘贴到 include/onenet_config.h）。
    """
    if scope == "device":
        res = f"products/{product_id}/devices/{device_name}"
    elif scope == "product":
        res = f"products/{product_id}"
    else:
        res = f"userid/{device_name}"

    et = int(time.time()) + lifetime_days * 86400
    try:
        key = base64.b64decode(key_b64)
    except Exception:
        key = key_b64.encode("utf-8")

    if sign_order == "version_et_method_res":
        string_for_sign = f"{version}\n{et}\n{method}\n{res}\n"
    else:
        string_for_sign = f"{et}\n{method}\n{res}\n{version}"

    algo = {"sha1": hashlib.sha1, "sha256": hashlib.sha256,
            "md5": hashlib.md5}.get(method, hashlib.sha1)
    digest = hmac.new(key, string_for_sign.encode("utf-8"), algo).digest()
    sign = urllib.parse.quote_plus(base64.b64encode(digest).decode("ascii"))

    token = (f"version={version}&res={urllib.parse.quote_plus(res)}"
             f"&et={et}&method={method}&sign={sign}")
    return {"token": token, "res": res, "et": et,
            "string_for_sign": string_for_sign, "method": method}


class OneNetError(Exception):
    pass


class OneNetClient:
    """同步命令 API 的轻量封装。"""

    def __init__(self, product_id: str, device_name: str, key_b64: str,
                 device_id: str = "", api_base: str = API_BASE,
                 command_path: str = COMMAND_PATH, timeout: float = 10.0,
                 token_kwargs: Optional[dict] = None,
                 channel: str = DEFAULT_CHANNEL, access_key: str = ""):
        self.pid = product_id
        self.dev = device_name
        self.key = key_b64
        self.access_key = access_key or key_b64
        self.device_id = device_id
        self.api_base = api_base.rstrip("/")
        self.command_path = command_path
        self.timeout = timeout
        self.token_kwargs = token_kwargs or {}
        self.channel = (channel or DEFAULT_CHANNEL).lower()

    def _auth(self) -> str:
        """prop 通道用产品级 res + AccessKey；cmd 通道用设备级 res + sec_key。"""
        kw = dict(self.token_kwargs)
        if self.channel == "prop":
            kw["scope"] = "product"
            return make_token(self.pid, self.dev, self.access_key, **kw)["token"]
        kw.pop("scope", None)          # 设备级 token 由 make_token 默认处理
        return make_token(self.pid, self.dev, self.key, **kw)["token"]

    def send_command(self, body: bytes, api_timeout: int = 10) -> tuple[bool, object]:
        """
        下发一条命令。

        成功 -> (True, 设备应答的原始 bytes)
        失败 -> (False, 错误说明字符串)

        prop 通道把 body（base64 文本）塞进物模型属性 ota_cmd，
        设备处理完在 set_reply 的 msg 里回 base64 的 ACK 帧。
        """
        if self.channel == "prop":
            return self._send_property(body, api_timeout)
        return self._send_synccmd(body, api_timeout)

    def _send_property(self, body, api_timeout: int) -> tuple[bool, object]:
        if not (self.pid and self.dev and self.access_key):
            return False, "缺少 产品ID / 设备名称 / 产品AccessKey"
        if isinstance(body, (bytes, bytearray)):
            text = bytes(body).decode("utf-8", "replace")
        else:
            text = str(body)

        payload = json.dumps({
            "product_id": self.pid,
            "device_name": self.dev,
            "params": {"ota_cmd": text},
        }).encode("utf-8")

        t = max(5, min(30, int(api_timeout)))
        url = f"{self.api_base}{PROPERTY_SET_PATH}"
        headers = {
            "Authorization": self._auth(),
            "Content-Type": "application/json",
        }
        req = urllib.request.Request(url, data=payload, headers=headers,
                                     method="POST")
        try:
            with urllib.request.urlopen(req, timeout=t + 10) as resp:
                raw = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return False, f"HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:300]}"
        except Exception as e:
            return False, f"{type(e).__name__}: {e}"

        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            return False, f"响应不是 JSON: {raw[:200]}"

        code = data.get("code", 0)
        if str(code) not in ("0", "None"):
            return False, f"code={code} {data.get('msg', '')}"

        d = data.get("data") or {}
        dcode = str(d.get("code", ""))
        if dcode and dcode not in ("200", "0"):
            # 设备收到了但处理失败（比如帧校验不过）
            return False, f"设备回复 code={dcode} {d.get('msg', '')}"

        msg = str(d.get("msg") or "")
        if not msg:
            return False, "设备没有回复（属性设置未收到 set_reply）"
        try:
            return True, base64.b64decode(msg)
        except Exception:
            # 设备回了纯文本，交给上层按"不是 ACK 帧"处理
            return True, msg.encode("utf-8", "replace")

    def _send_synccmd(self, body, api_timeout: int) -> tuple[bool, object]:
        if not (self.pid and self.dev and self.key):
            return False, "缺少 产品ID / 设备名称 / 设备密钥"
        if not self.device_id:
            return False, "缺少 device_id（设备详情页里的数字 ID，不是设备名称）"
        if not isinstance(body, (bytes, bytearray)):
            body = str(body).encode("utf-8")
        if len(body) >= 1024:
            return False, f"命令体 {len(body)} B 超过 OneNET 的 1KB 上限"

        t = max(5, min(30, int(api_timeout)))
        query = urllib.parse.urlencode({"device_id": self.device_id,
                                        "timeout": t})
        url = f"{self.api_base}{self.command_path}?{query}"
        headers = {
            "Authorization": self._auth(),
            "Content-Type": "text/plain; charset=utf-8",
        }
        req = urllib.request.Request(url, data=bytes(body),
                                     headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=t + 6) as resp:
                raw = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:300]
            return False, f"HTTP {e.code}: {detail}"
        except Exception as e:
            return False, f"{type(e).__name__}: {e}"

        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            return False, f"响应不是 JSON: {raw[:200]}"

        errno = data.get("errno", 0)
        if str(errno) not in ("0", "000000"):
            return False, (f"errno={errno} {data.get('error', '')} "
                           f"| {ERRNO_TEXT.get(int(errno) if str(errno).isdigit() else -1, '')}")
        cmd_resp = (data.get("data") or {}).get("cmd_resp")
        if not cmd_resp:
            return False, "平台返回成功但没有 cmd_resp（设备没有回复 response topic）"
        try:
            return True, base64.b64decode(cmd_resp)
        except Exception as e:
            return False, f"cmd_resp base64 解码失败: {e}"

    def ping_raw(self, body: bytes = b"PING", api_timeout: int = 8):
        """连通性自检：只关心 HTTP 是否成功、设备是否在线。"""
        return self.send_command(body, api_timeout)


# ---------------------------------------------------------------------------
# 设备反查（产品ID + 设备名称 -> 数字 device_id）
#
# 注意两套 API 命名空间完全不同，踩过坑记在这里：
#
#   同步命令（方案 B 用的）：  POST /v1/synccmds        -> 返回 errno/error
#   设备管理（新版 AIoT）：    GET  /device/detail      -> 返回 code/msg
#                              GET  /device/list
#
#   * 新版设备管理接口【没有 /v1 前缀】。写成 /v1/device/detail 会得到
#     HTTP 406 "JUPITER: HTTP url namespace err"，看着像"接口不存在"，
#     其实只是前缀写错了。
#   * 这个网关按「路径+方法」路由：同一个 /v1/synccmds，GET 是 406、
#     POST 才是 200。所以不能用"GET 报 406"判断路径不存在。
# ---------------------------------------------------------------------------
DEVICE_DETAIL_PATH = "/device/detail"
DEVICE_LIST_PATH = "/device/list"


def _get_json(url: str, token: str, timeout: float = 15.0):
    """GET 一个 JSON 接口 -> (ok, data_or_errmsg, http_code)。"""
    req = urllib.request.Request(url, headers={"Authorization": token},
                                 method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
            code = resp.status
    except urllib.error.HTTPError as e:
        return False, (f"HTTP {e.code}: "
                       f"{e.read().decode('utf-8', 'replace')[:200]}"), e.code
    except Exception as e:
        return False, f"{type(e).__name__}: {e}", -1
    try:
        return True, json.loads(raw), code
    except json.JSONDecodeError:
        return False, f"响应不是 JSON: {raw[:160]}", code


def _pick_device_id(payload, device_name: str = ""):
    """
    从 /device/detail 或 /device/list 的返回里挖出数字设备 ID。

    详情：data = {"did":123456,"name":"stm32_ota_01","sec_key":"..."}
    列表：data = [ {"did":...}, ... ]
    顺手兼容一下 id / device_id 两种叫法。
    """
    d = payload.get("data")
    items = []
    if isinstance(d, dict):
        items = [d]
    elif isinstance(d, list):
        items = [x for x in d if isinstance(x, dict)]

    for it in items:
        if device_name and it.get("name") and it.get("name") != device_name:
            continue
        for k in ("did", "id", "device_id"):
            v = it.get(k)
            if v not in (None, "", 0):
                return str(v), it
    return "", None


def query_device_detail(product_id: str, device_name: str, key_b64: str,
                        api_base: str = API_BASE,
                        token_kwargs: Optional[dict] = None):
    """
    用 产品ID + 设备名称 反查数字 device_id。

    为什么需要它：OneNET 新版控制台的设备列表里那列叫「设备名称/ID」，
    但实际只显示名称（如 stm32_ota_01）。而方案 B 的同步命令 API 必须用
    纯数字的 ?device_id=，页面上不给看。

        GET {api_base}/device/detail?product_id=..&device_name=..
        Header: Authorization: <token>
        -> {"code":0,"msg":"succ","data":{"did":1234567890,
                                          "name":"stm32_ota_01", ...}}

    返回体里 data.did 就是 device_id；data.sec_key 是该设备的接入密钥。

    因为新版接口的鉴权 res 跟同步命令不完全一样（有的是 products/{pid}，
    有的是 products/{pid}/devices/{dev}），这里会把 scope 挨个试一遍，
    用户不用关心。

    成功 -> (True, {"device_id": "1234567890", "detail": {...}})
    失败 -> (False, "错误说明")
    """
    if not (product_id and device_name and key_b64):
        return False, "缺少 产品ID / 设备名称 / 设备密钥"

    base = (api_base or API_BASE).rstrip("/")
    tk = dict(token_kwargs or {})

    # 只有 product / device 两种 res 会被平台识别：
    #   products/{pid}                  <- 用产品 access_key 时
    #   products/{pid}/devices/{dev}    <- 用设备 sec_key 时
    # 给错格式平台回 "invalid res"，签名算错回 "invalid authorization"，
    # 两种错误的含义完全不同，所以两种 scope 都试一遍再下结论。
    scopes = []
    for sc in ("product", tk.get("scope"), "device"):
        if sc and sc not in scopes:
            scopes.append(sc)

    # 先查详情（精确、返回字段最全），失败再退到列表接口本地过滤
    probes = [
        (DEVICE_DETAIL_PATH,
         {"product_id": product_id, "device_name": device_name}),
        (DEVICE_LIST_PATH, {"product_id": product_id}),
    ]

    errors = []
    for scope in scopes:
        kw = dict(tk)
        kw["scope"] = scope
        try:
            token = make_token(product_id, device_name, key_b64, **kw)["token"]
        except Exception as e:
            errors.append(f"生成 token 失败: {e}")
            continue

        for path, params in probes:
            url = f"{base}{path}?" + urllib.parse.urlencode(params)
            ok, data, _code = _get_json(url, token)
            if not ok:
                errors.append(f"{path}: {data}")
                continue

            # 新版用 code，老式用 errno，两个都认
            err = data.get("code", data.get("errno", 0))
            if str(err) not in ("0", "None", ""):
                msg = str(data.get("msg") or data.get("error") or "")
                errors.append(f"{path}: code={err} {msg}")
                continue

            did, detail = _pick_device_id(data, device_name)
            if did:
                return True, {"device_id": did, "detail": detail or {}}
            errors.append(f"{path}: 返回里没有设备 ID 字段（did）")

    return False, _diagnose(errors)


def _diagnose(errors) -> str:
    """把一串试探失败翻译成用户能直接照做的提示。"""
    blob = " | ".join(errors) if errors else "未知错误"
    low = blob.lower()
    if "invalid authorization" in low:
        return ("设备密钥不对（平台回 invalid authorization = token 签名验不过）。"
                "请填控制台【设备详情 → 设备密钥】，"
                "或【产品概况 → 访问密钥 access_key】其一，"
                "注意整串 Base64 原样粘贴、不要带空格换行。"
                f" | 原始返回: {blob[:200]}")
    if "invalid res" in low:
        return ("token 的 res 格式不被平台接受。"
                f" | 原始返回: {blob[:200]}")
    if "10410" in blob or "not exist" in low:
        return ("设备不存在。设备名称必须和控制台里一字不差（区分大小写），"
                f" | 原始返回: {blob[:200]}")
    if "10404" in blob:
        return f"产品 ID 不对或设备不在该产品下。 | 原始返回: {blob[:200]}"
    return blob[:400]


def device_properties_payload(fw_version: str = "", active_slot: int = 0,
                              ota_stage: int = 0, ota_progress: int = 0,
                              ota_result: int = 0,
                              wifi_connected: int = 0) -> dict:
    """6 个只读属性的 OneJSON 模板（由 ESP32 发布，上位机只用来对照）。"""
    return {
        "id": str(int(time.time())),
        "version": "1.0",
        "params": {
            "fw_version":     {"value": fw_version},
            "active_slot":    {"value": active_slot},
            "ota_stage":      {"value": ota_stage},
            "ota_progress":   {"value": ota_progress},
            "ota_result":     {"value": ota_result},
            "wifi_connected": {"value": wifi_connected},
        },
    }


def topic_map(product_id: str, device_name: str) -> dict:
    """方案 B 用到的 3 个 topic，方便在界面里核对。"""
    p = f"$sys/{product_id}/{device_name}"
    return {
        "设备订阅（命令请求）": f"{p}/cmd/request/+",
        "设备发布（命令应答）": f"{p}/cmd/response/{{cmdid}}",
        "设备发布（属性上报）": f"{p}/thing/property/post",
    }


if __name__ == "__main__":
    t = make_token("123456", "stm32_ota_01",
                   base64.b64encode(b"demo-key").decode())
    print("token:", t["token"])
    print("res  :", t["res"], " et:", t["et"])
    print("topics:", json.dumps(topic_map("123456", "stm32_ota_01"),
                               ensure_ascii=False, indent=2))
