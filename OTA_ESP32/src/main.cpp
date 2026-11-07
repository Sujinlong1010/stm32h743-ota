/**
 * @file  main.cpp
 * @brief ESP32-S3 = OneNET <-> STM32H743 frame bridge (方案 B / dumb pipe).
 *
 *  OneNET 不托管固件、不建 OTA 任务。上位机把固件切成片，封成 0xAA55
 *  帧，再 base64 后通过「同步设备命令 API」下发；平台把 body 原样放进
 *  $sys/{pid}/{dev}/cmd/request/{cmdid} 的 MQTT payload。
 *
 *  本固件做的事：
 *    1. base64 解码 -> 完整 0xAA55 帧 -> UART1 921600 发给 STM32；
 *    2. 等 STM32 的 ACK 帧 -> base64 -> 发布到 cmd/response/{cmdid}；
 *       平台把这段作为同步 API 的 cmd_resp 返回给上位机；
 *    3. STM32 的 OTA_PROGRESS 帧 -> 转成 6 个只读属性的 OneJSON 上报；
 *    4. 同时保留 USB CDC <-> UART1 的本地透明桥，方便不用云也能调。
 *
 *  零第三方库：MQTT 3.1.1、base64、帧组包全部自实现。
 */
#include <Arduino.h>
#include <WiFi.h>
#include "ota_protocol.h"
#include "mini_mqtt.h"

/* onenet_config.h 里有 WiFi 密码和设备 Token，不纳入版本库。
 * 首次编译前先从模板复制一份：
 *     cp include/onenet_config.example.h include/onenet_config.h
 * 没复制时给一条看得懂的报错，而不是让编译器在 include 里报 "No such file"。 */
#if __has_include("onenet_config.h")
#include "onenet_config.h"
#else
#error "缺少 include/onenet_config.h —— 请执行: cp include/onenet_config.example.h include/onenet_config.h 并填入 WiFi 与 OneNET 参数"
#endif

/* USB 串口里输出什么：
 *   0 = 只输出可读文本日志（默认，避免二进制帧把串口刷成乱码）
 *   1 = 每帧输出一行文本摘要
 *   2 = 输出原始 0xAA55 二进制帧（仅当要用上位机「本地串口」通道时才开）
 * 之前默认把 STM32 的二进制帧直接写到 USB，STM32 一上电就持续刷，
 * 串口里全是乱码且刷得极快，把 [WiFi] 日志整个淹掉，看起来像连不上。 */
#define USB_FRAME_OUTPUT        0

/* 开机做一次 UART 回环自检：把 GPIO17 和 GPIO18 用一根杜邦线短接后复位，
 * 串口会打印 [STM32] UART-SELFTEST，说明 ESP32 自己的 UART 发送/接收/
 * 引脚/波特率全部正常 —— 那问题就一定在外部杜邦线或 STM32 侧。
 * 不短接时这帧只是白发给 STM32，STM32 不认 CMD_LOG 请求，无副作用。 */
#define UART_SELFTEST_ON_BOOT   1

/* 每隔多少毫秒打印一次 UART 收字节统计。
 * 这个统计能一刀切区分两种「串口没日志」：
 *   rx 一直是 0        -> 物理层完全不通（杜邦线断 / 没共地 / 引脚错 / TX-RX 没交叉）
 *   rx 在涨但 frames=0 -> 线上有电平，但波特率不匹配或被干扰，帧 CRC 全错
 * 没有这个统计，两种情况在串口上看起来都是「什么都没有」。 */
#define UART_STATS_EVERY_MS     5000u

/* 帧追踪：把 UART1 上解析出来的每一帧的 cmd / seq / flag / 长度 / 前 20 字节
 * 打出来，用于确认「线上跑的到底是什么」。排查完置 0。 */
#define UART_FRAME_TRACE        1
#define UART_FRAME_TRACE_MAX    60

/* ------------------------------------------------------------- topics -- */
/* 为什么订阅 "#" 而不是文档写的 "cmd/request/+"（实测结论，别改回去）：
 *
 *   订阅 $sys/{pid}/{dev}/cmd/request/+   -> SUBACK 0x80 被平台拒绝
 *   订阅 $sys/{pid}/{dev}/cmd/request/abc -> SUBACK 0x80 被平台拒绝
 *   订阅 $sys/{pid}/{dev}/cmd/#           -> SUBACK 0x80 被平台拒绝
 *   订阅 $sys/{pid}/{dev}/#               -> SUBACK 0x00 授权成功
 *
 * 这个产品的命令前缀没有单独开放订阅权限，只有整棵 "#" 树能订。
 * 订阅被拒后旧代码会 mqtt.stop() 主动断开，3 秒后重连，于是平台上看
 * 设备一直「离线 / 反复上下线」，同步命令必然报设备不在线。
 *
 * 订 "#" 会同时收到 thing/property/set 等无关 topic，
 * handleCloudCommand() 里已经用 cmd/request/ 前缀做过过滤，安全。 */
static const char *TOPIC_CMD_REQ =
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/#";
static const char *TOPIC_CMD_RSP_PREFIX =
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/cmd/response/";
static const char *TOPIC_PROP_POST =
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post";
/* 物模型属性下发通道：平台 -> 设备，设备必须回 set_reply 才算设置成功 */
static const char *TOPIC_PROP_SET =
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/set";
static const char *TOPIC_PROP_SET_REPLY =
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/set_reply";

/* -------------------------------------------------------------- state -- */
static WiFiClient netClient;
static MiniMQTT   mqtt(netClient);
static bool       cloudEnabled = false;
static uint32_t   lastMqttTry  = 0;

struct PendingCmd
{
    bool     active;
    uint16_t cmd;
    uint16_t seq;
    char     cmdid[96];
    uint32_t startMs;
    uint8_t  resp[CLOUD_FRAME_MAX];
    size_t   respLen;
    uint8_t  via;          /* 0 = cmd/request 通道, 1 = thing/property/set 通道 */
};
static PendingCmd pending = {false, 0, 0, {0}, 0, {0}, 0, 0};

/* ------------------------------------------------------------ base64 -- */
static int b64Val(char c)
{
    if (c >= 'A' && c <= 'Z') { return c - 'A'; }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
    if (c >= '0' && c <= '9') { return c - '0' + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

static int b64Decode(const char *in, size_t n, uint8_t *out, size_t cap)
{
    uint32_t acc = 0u;
    int bits = 0, o = 0;
    size_t i;

    for (i = 0u; i < n; ++i)
    {
        char c = in[i];
        int v;
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\0') { break; }
        v = b64Val(c);
        if (v < 0) { continue; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if ((size_t)o >= cap) { return -1; }
            out[o++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    return o;
}

/* 属性通道回执要把 ACK 帧塞进 set_reply 的 msg 字段，所以需要编码 */
static int b64Encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0u;

    for (i = 0u; i + 2u < n; i += 3u)
    {
        if (o + 4u >= cap) { return -1; }
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1u] << 8)
                   | (uint32_t)in[i + 2u];
        out[o++] = T[(v >> 18) & 0x3Fu];
        out[o++] = T[(v >> 12) & 0x3Fu];
        out[o++] = T[(v >> 6) & 0x3Fu];
        out[o++] = T[v & 0x3Fu];
    }
    if (i < n)
    {
        if (o + 4u >= cap) { return -1; }
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2) { v |= (uint32_t)in[i + 1u] << 8; }
        out[o++] = T[(v >> 18) & 0x3Fu];
        out[o++] = T[(v >> 12) & 0x3Fu];
        out[o++] = (rem == 2) ? T[(v >> 6) & 0x3Fu] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return (int)o;
}

/* --------------------------------------------------- frame assembler -- */
class FrameAsm
{
public:
    FrameAsm() : pos(0), need(0) {}

    void reset() { pos = 0; need = 0; }

    bool feed(uint8_t b, uint8_t *out, size_t *outLen)
    {
        if (pos == 0u)
        {
            if (b == OTA_MAGIC0) { buf[pos++] = b; }
            return false;
        }
        if (pos == 1u)
        {
            if (b == OTA_MAGIC1)
            {
                buf[pos++] = b;
            }
            else
            {
                pos = (b == OTA_MAGIC0) ? 1u : 0u;
                if (pos) { buf[0] = b; }
            }
            return false;
        }
        if (pos >= sizeof(buf)) { reset(); return false; }
        buf[pos++] = b;

        if (pos == OTA_FRAME_HEADER_LEN)
        {
            uint16_t ln = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
            if ((buf[2] != OTA_PROTO_VER) || (ln > OTA_FRAME_MAX_PAYLOAD))
            {
                reset();
                return false;
            }
            need = (size_t)OTA_FRAME_HEADER_LEN + ln + OTA_FRAME_CRC_LEN;
            if (need > sizeof(buf)) { reset(); return false; }
        }
        if ((need > 0u) && (pos >= need))
        {
            uint32_t got = (uint32_t)buf[need - 4]
                         | ((uint32_t)buf[need - 3] << 8)
                         | ((uint32_t)buf[need - 2] << 16)
                         | ((uint32_t)buf[need - 1] << 24);
            uint32_t crc = ota_crc32(buf, (uint32_t)(need - 4));
            if (crc == got)
            {
                memcpy(out, buf, need);
                *outLen = need;
                reset();
                return true;
            }
            reset();
        }
        return false;
    }

private:
    uint8_t buf[CLOUD_FRAME_MAX];
    size_t  pos;
    size_t  need;
};

static FrameAsm uartAsm;   /* Serial1 -> STM32 replies */
static FrameAsm usbAsm;    /* USB CDC -> PC local frames */

/* UART1 累计收字节 / 收帧数，只用于诊断打印，不参与业务逻辑。 */
static uint32_t g_uartRxBytes = 0u;
static uint32_t g_uartFrames  = 0u;

static uint16_t frameCmd(const uint8_t *f)
{
    return (uint16_t)f[4] | ((uint16_t)f[5] << 8);
}
static uint16_t frameSeq(const uint8_t *f)
{
    return (uint16_t)f[6] | ((uint16_t)f[7] << 8);
}
static bool frameIsResp(const uint8_t *f)
{
    return (f[3] & OTA_FLAG_RESP) != 0u;
}

/* ------------------------------------------------------------- helpers -- */
/* 通用帧构造器（与 STM32 侧 ota_link.c 的 send_frame 布局一致）。 */
static size_t buildFrame(uint8_t *out, uint16_t cmd, uint16_t seq,
                         const uint8_t *payload, uint16_t len, uint8_t resp)
{
    uint32_t crc;

    out[0] = (uint8_t)OTA_MAGIC0;
    out[1] = (uint8_t)OTA_MAGIC1;
    out[2] = (uint8_t)OTA_PROTO_VER;
    out[3] = resp ? (uint8_t)OTA_FLAG_RESP : 0u;
    out[4] = (uint8_t)(cmd & 0xFFu);
    out[5] = (uint8_t)(cmd >> 8);
    out[6] = (uint8_t)(seq & 0xFFu);
    out[7] = (uint8_t)(seq >> 8);
    out[8] = (uint8_t)(len & 0xFFu);
    out[9] = (uint8_t)(len >> 8);
    if ((len > 0u) && (payload != 0))
    {
        memcpy(&out[OTA_FRAME_HEADER_LEN], payload, len);
    }
    crc = ota_crc32(out, (uint32_t)(OTA_FRAME_HEADER_LEN + len));
    out[OTA_FRAME_HEADER_LEN + len + 0] = (uint8_t)(crc & 0xFFu);
    out[OTA_FRAME_HEADER_LEN + len + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    out[OTA_FRAME_HEADER_LEN + len + 2] = (uint8_t)((crc >> 16) & 0xFFu);
    out[OTA_FRAME_HEADER_LEN + len + 3] = (uint8_t)((crc >> 24) & 0xFFu);
    return (size_t)(OTA_FRAME_HEADER_LEN + len + OTA_FRAME_CRC_LEN);
}

/* UART 自检用的 LOG 帧。resp=1 与 STM32 的 ota_link_log() 保持一致，
 * 这样真 STM32 收到它也不会回 ACK。 */
static size_t buildLogFrame(uint8_t *out, const char *txt)
{
    return buildFrame(out, CMD_LOG, 0u, (const uint8_t *)txt,
                      (uint16_t)strlen(txt), 1u);
}

/* 本地 ping：ESP32 自己发一帧 CMD_PING 给 STM32，看 600ms 内有没有 ACK。
 * 这一条能单独证明「ESP32 -> STM32」这个方向，不需要云、不需要上位机。
 * 触发方式：在串口里敲一个小写 p。 */
static void localPingTest(void)
{
    uint8_t  f[16];
    uint8_t  got[CLOUD_FRAME_MAX];
    size_t   gotLen = 0u;
    size_t   n;
    uint32_t t0;
    bool     ok = false;

    n = buildFrame(f, CMD_PING, 0x5A5Au, 0, 0u, 0u);
    while (Serial1.available() > 0) { (void)Serial1.read(); }
    Serial1.write(f, n);
    Serial1.flush();

    Serial.printf("[PING] TX %u B -> STM32 (GPIO%d), 等 ACK 600ms ...\n",
                  (unsigned)n, STM32_UART_TX_PIN);

    t0 = millis();
    while (!ok && ((millis() - t0) < 600u))
    {
        while ((Serial1.available() > 0) && !ok)
        {
            if (uartAsm.feed((uint8_t)Serial1.read(), got, &gotLen))
            {
                g_uartFrames++;
                if ((frameCmd(got) == CMD_PING) && frameIsResp(got) &&
                    (frameSeq(got) == 0x5A5Au))
                {
                    ok = true;
                }
            }
        }
    }

    if (ok)
    {
        Serial.println("[PING] ACK OK -> ESP32->STM32 方向通，接线正确");
    }
    else
    {
        Serial.println("[PING] NO ACK -> ESP32 发出的帧没到 STM32。"
                       "查 GPIO18 -> STM32 PB15（PB15 是 STM32 的 RX，"
                       "别接到 PB14 上，那是 STM32 的 TX）");
    }
}

/* 每条命令该等 STM32 多久。
 *
 * 为什么不能统一 3 秒：CMD_OTA_START 在 STM32 侧是「同步擦除目标槽再回 ACK」，
 * 一个槽 0xE0000 = 7 个 128KB 扇区，H743 每扇区擦除 1~2 秒，合计 7~14 秒。
 * 原来统一等 3 秒，STM32 还在擦除，ESP32 就抢先造了一个 ST_TIMEOUT 假应答，
 * 上位机于是报「设备拒绝擦除目标槽 (0x09)」——看起来像设备拒绝，其实是
 * 我们自己等太短。
 *
 * 阈值关系：ESP32 的等待窗口必须 > 上位机的 request timeout(25s)，
 * 否则上位机永远只会看到假的 ST_TIMEOUT。这里给 45 秒留足余量。 */
#define OTA_LONG_ACK_WAIT_MS    45000u

static uint32_t ackWaitMs(uint16_t cmd)
{
    switch (cmd)
    {
    case CMD_OTA_START:
    case CMD_FW_END:
    case CMD_OTA_ABORT:
        return OTA_LONG_ACK_WAIT_MS;
    case CMD_FW_CHUNK:
        /* 分片本身很快，但某一帧可能触发惰性擦除一个 128KB 扇区
           （H743 最长 4 s）。给 4.5 s，仍低于平台属性接口约 5 s 的上限。 */
        return 4500u;
    default:
        return CMD_ACK_WAIT_MS;
    }
}

static void makeTimeoutAck(uint8_t *out, size_t *outLen)
{
    uint32_t crc;
    out[0] = (uint8_t)OTA_MAGIC0;
    out[1] = (uint8_t)OTA_MAGIC1;
    out[2] = (uint8_t)OTA_PROTO_VER;
    out[3] = (uint8_t)OTA_FLAG_RESP;
    out[4] = (uint8_t)(pending.cmd & 0xFFu);
    out[5] = (uint8_t)(pending.cmd >> 8);
    out[6] = (uint8_t)(pending.seq & 0xFFu);
    out[7] = (uint8_t)(pending.seq >> 8);
    out[8] = 1u;
    out[9] = 0u;
    out[10] = ST_TIMEOUT;
    crc = ota_crc32(out, 11u);
    out[11] = (uint8_t)(crc & 0xFFu);
    out[12] = (uint8_t)((crc >> 8) & 0xFFu);
    out[13] = (uint8_t)((crc >> 16) & 0xFFu);
    out[14] = (uint8_t)((crc >> 24) & 0xFFu);
    *outLen = 15u;
}

static void replyToPlatform()
{
    if (pending.via == 1u)
    {
        /* 属性下发通道：设备必须回 thing/property/set_reply，
           平台才会把结果同步返回给 set-device-property 的调用方。
           ACK 只有 15~40 字节，base64 后塞进 msg 字段完全够。 */
        char b64[160];
        char json[320];
        int n = b64Encode(pending.resp, pending.respLen, b64, sizeof(b64));

        if (n < 0) { n = 0; b64[0] = '\0'; }
        snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"code\":200,\"msg\":\"%s\"}",
                 pending.cmdid, b64);
        mqtt.publishText(TOPIC_PROP_SET_REPLY, json);
        Serial.printf("[PROP] reply id=%s ack=%d B -> b64 %d B\n",
                      pending.cmdid, (int)pending.respLen, n);
        pending.active = false;
        return;
    }

    String topic = String(TOPIC_CMD_RSP_PREFIX) + String(pending.cmdid);

    /* Publish the RAW 0xAA55 ACK frame. OneNET base64-encodes it into
       data.cmd_resp, so the PC only has to decode once. */
    mqtt.publish(topic.c_str(), pending.resp, pending.respLen);
    Serial.printf("[CMD] resp %s -> %d B\n",
                  pending.cmdid, (int)pending.respLen);
    pending.active = false;
}

static void reportProperties(const uint8_t *payload, size_t len)
{
    char json[360];
    uint8_t stage = 0u, result = 0u, progress = 0u, slot = 0u;

    if (len >= 8u)
    {
        stage    = payload[0];
        result   = payload[1];
        progress = payload[2];
        slot     = payload[3];
    }
    snprintf(json, sizeof(json),
             "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{"
             "\"fw_version\":{\"value\":\"%s\"},"
             "\"active_slot\":{\"value\":%u},"
             "\"ota_stage\":{\"value\":%u},"
             "\"ota_progress\":{\"value\":%u},"
             "\"ota_result\":{\"value\":%u},"
             "\"wifi_connected\":{\"value\":%d}}}",
             (unsigned long)(millis() + 1000000UL),
             ONENET_DEFAULT_VERSION,
             (unsigned)slot, (unsigned)stage, (unsigned)progress,
             (unsigned)result, (WiFi.status() == WL_CONNECTED) ? 1 : 0);
    mqtt.publishText(TOPIC_PROP_POST, json);
}

/* -------------------------------------------------------- cloud command -- */
static void handleCloudCommand(const char *topic, const uint8_t *payload,
                               size_t len)
{
    String prefix = String("$sys/") + ONENET_PRODUCT_ID + "/"
                  + ONENET_DEVICE_NAME + "/cmd/request/";
    uint8_t frame[CLOUD_FRAME_MAX];
    int n;

    if (strncmp(topic, prefix.c_str(), prefix.length()) != 0) { return; }
    if (pending.active)
    {
        Serial.println("[CMD] busy, drop duplicate");
        return;
    }

    n = b64Decode((const char *)payload, len, frame, sizeof(frame));
    if ((n < (int)(OTA_FRAME_OVERHEAD)) ||
        (frame[0] != OTA_MAGIC0) || (frame[1] != OTA_MAGIC1))
    {
        Serial.println("[CMD] bad base64 / bad frame magic");
        return;
    }

    memset(&pending, 0, sizeof(pending));
    pending.cmd  = frameCmd(frame);
    pending.seq  = frameSeq(frame);
    pending.startMs = millis();
    strncpy(pending.cmdid, topic + prefix.length(), sizeof(pending.cmdid) - 1u);
    pending.cmdid[sizeof(pending.cmdid) - 1u] = '\0';
    pending.active = true;

    Serial.printf("[CMD] id=%s cmd=0x%04X seq=%u bytes=%d\n",
                  pending.cmdid, pending.cmd, pending.seq, n);
    Serial1.write(frame, (size_t)n);
    Serial1.flush();
}

/* ---------------------------------------------- 物模型属性下发通道 -- */
/* 为什么需要这一条：本产品的 cmd/request/+ 订阅被平台拒绝（SUBACK 0x80），
   同步命令 API 也报"设备不存在"，唯一能下发的通道是物模型属性设置。

   请求（平台 -> 设备）：
     topic   $sys/{pid}/{dev}/thing/property/set
     payload {"id":"12345","version":"1.0","params":{"ota_cmd":"<base64 帧>"}}
   应答（设备 -> 平台）：
     topic   $sys/{pid}/{dev}/thing/property/set_reply
     payload {"id":"12345","code":200,"msg":"<base64 ACK 帧>"}

   msg 里塞 base64 是我们自己的约定，平台只要求 code/msg 存在。            */
static void handlePropertySet(const uint8_t *payload, size_t len)
{
    static char js[CLOUD_FRAME_MAX + 128];
    char  idbuf[96];
    uint8_t frame[CLOUD_FRAME_MAX];
    const char *p;
    const char *q;
    size_t idlen, blen;
    int n;

    if (len >= sizeof(js)) { return; }
    memcpy(js, payload, len);
    js[len] = '\0';

    if (pending.active)
    {
        Serial.println("[PROP] busy, drop duplicate");
        return;
    }

    p = strstr(js, "\"id\":\"");
    if (p == nullptr) { return; }
    p += 6;
    q = strchr(p, '"');
    if (q == nullptr) { return; }
    idlen = (size_t)(q - p);
    if (idlen >= sizeof(idbuf)) { idlen = sizeof(idbuf) - 1u; }
    memcpy(idbuf, p, idlen);
    idbuf[idlen] = '\0';

    p = strstr(js, "\"ota_cmd\":\"");
    if (p == nullptr)
    {
        Serial.println("[PROP] no ota_cmd in params, ignore");
        return;
    }
    p += 11;
    q = strchr(p, '"');
    if (q == nullptr) { return; }
    blen = (size_t)(q - p);
    if (blen == 0u) { return; }

    n = b64Decode(p, blen, frame, sizeof(frame));
    if ((n < (int)(OTA_FRAME_OVERHEAD)) ||
        (frame[0] != OTA_MAGIC0) || (frame[1] != OTA_MAGIC1))
    {
        Serial.printf("[PROP] bad frame (b64 %u B -> %d B)\n",
                      (unsigned)blen, n);
        return;
    }

    memset(&pending, 0, sizeof(pending));
    pending.cmd     = frameCmd(frame);
    pending.seq     = frameSeq(frame);
    pending.startMs = millis();
    pending.via     = 1u;
    memcpy(pending.cmdid, idbuf, idlen + 1u);
    pending.active  = true;

    Serial.printf("[PROP] id=%s cmd=0x%04X seq=%u b64=%uB -> %dB\n",
                  pending.cmdid, pending.cmd, pending.seq,
                  (unsigned)blen, n);
    Serial1.write(frame, (size_t)n);
    Serial1.flush();
}

/* ------------------------------------------------------------- networks -- */
/* 连接是异步的：WiFi.begin() 只是启动流程，之后 5~10 s 才会变成 CONNECTED。
 * 旧实现每 2 s 就重新 begin()，等于每次刚连上就被打断，永远连不上。
 * 现在用 IDLE / CONNECTING / UP 三态，同一次连接只 begin 一次，
 * 15 s 拿到不 CONNECTED 才算失败，失败后隔 5 s 再重试。 */
#define WIFI_CONNECT_TIMEOUT_MS  15000u
#define WIFI_RETRY_DELAY_MS       5000u
#define WIFI_SCAN_ON_BOOT             1

enum WifiPhase { WIFI_IDLE = 0, WIFI_CONNECTING, WIFI_UP };

static WifiPhase wifiPhase   = WIFI_IDLE;
static uint32_t  wifiStartMs = 0u;
static uint32_t  wifiRetryAt = 0u;
static int       wifiLastSt  = -1;

static const char *wifiStatusText(int s)
{
    switch (s)
    {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (SSID not visible: 5GHz-only? typo? hidden?)";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (wrong password / portal / WPA-Enterprise)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
    }
}

/* 用数值判断，避免不同 arduino-esp32 版本的枚举名差异导致编译失败。
 * 0=OPEN 1=WEP 2=WPA_PSK 3=WPA2_PSK 4=WPA_WPA2_PSK 5=WPA2_ENTERPRISE
 * 6=WPA3_PSK 7=WPA2_WPA3_PSK                                       */
static const char *authName(int t)
{
    if (t == 0) { return "OPEN"; }
    if (t == 1) { return "WEP"; }
    if (t == 2) { return "WPA-PSK"; }
    if (t == 3) { return "WPA2-PSK"; }
    if (t == 4) { return "WPA/WPA2-PSK"; }
    if (t == 5) { return "WPA2-ENTERPRISE *cannot login with password*"; }
    if (t == 6) { return "WPA3-PSK"; }
    if (t == 7) { return "WPA2/WPA3-PSK"; }
    return "OTHER";
}

/* 开机扫一次 2.4GHz，直接看出目标 SSID 在不在、加密方式是否可连。 */
static void wifiScanReport()
{
    Serial.printf("[WiFi] target SSID = \"%s\"\n", WIFI_SSID);
    Serial.println("[WiFi] scanning 2.4GHz APs ...");

    /* 每信道默认 300ms，13 个信道要 4s；降到 120ms 约 1.6s，少耗电、少阻塞启动 */
    int n = (int)WiFi.scanNetworks(false, true, false, 120u);
    if (n <= 0)
    {
        Serial.println("[WiFi] scan found 0 AP -> antenna / board / 2.4GHz disabled?");
        WiFi.scanDelete();
        return;
    }

    Serial.printf("[WiFi] found %d AP(s):\n", n);
    bool found = false;
    for (int i = 0; i < n; ++i)
    {
        String ssid = WiFi.SSID(i);
        bool   hit  = (ssid == String(WIFI_SSID));
        found = found || hit;
        Serial.printf("  %2d) %-24s ch=%2d rssi=%4d dBm auth=%d(%s)%s\n",
                      i + 1, ssid.c_str(), (int)WiFi.channel(i),
                      (int)WiFi.RSSI(i), (int)WiFi.encryptionType(i),
                      authName((int)WiFi.encryptionType(i)),
                      hit ? "  <== target" : "");
    }
    if (!found)
    {
        Serial.printf("[WiFi] \"%s\" NOT in the scan list.\n", WIFI_SSID);
        Serial.println("[WiFi] ESP32-S3 only supports 2.4GHz. If the router publishes a");
        Serial.println("[WiFi] 5GHz-only SSID, split it into 2.4G/5G or use the 2.4G one.");
    }
    WiFi.scanDelete();
}

static void wifiBeginConnect()
{
    Serial.printf("[WiFi] begin connect to \"%s\" ...\n", WIFI_SSID);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);          /* 省电模式会让连接/重连慢很多 */
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiPhase   = WIFI_CONNECTING;
    wifiStartMs = millis();
    wifiLastSt  = (int)WiFi.status();
}

static bool ensureWifi()
{
    wl_status_t st = WiFi.status();

    if (st == WL_CONNECTED)
    {
        if (wifiPhase != WIFI_UP)
        {
            wifiPhase = WIFI_UP;
            Serial.printf("[WiFi] CONNECTED  ip=%s  gw=%s  rssi=%d dBm  ch=%d  bssid=%s\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          (int)WiFi.RSSI(), (int)WiFi.channel(),
                          WiFi.BSSIDstr().c_str());
        }
        return true;
    }

    if (wifiPhase == WIFI_UP)
    {
        Serial.printf("[WiFi] link lost: %s\n", wifiStatusText((int)st));
        wifiPhase   = WIFI_IDLE;
        wifiRetryAt = millis() + 500u;
    }

    uint32_t now = millis();

    if (wifiPhase == WIFI_CONNECTING)
    {
        if ((int)st != wifiLastSt)
        {
            wifiLastSt = (int)st;
            Serial.printf("[WiFi] status -> %s\n", wifiStatusText((int)st));
        }
        if ((now - wifiStartMs) < WIFI_CONNECT_TIMEOUT_MS) { return false; }

        Serial.printf("[WiFi] timeout %us (last=%s), retry in %us\n",
                      (unsigned)(WIFI_CONNECT_TIMEOUT_MS / 1000u),
                      wifiStatusText((int)st),
                      (unsigned)(WIFI_RETRY_DELAY_MS / 1000u));
        WiFi.disconnect();
        wifiPhase   = WIFI_IDLE;
        wifiRetryAt = now + WIFI_RETRY_DELAY_MS;
        return false;
    }

    if ((int32_t)(now - wifiRetryAt) < 0) { return false; }
    wifiBeginConnect();
    return false;
}

static void ensureMqtt()
{
    if (!cloudEnabled || !ensureWifi()) { return; }
    if (mqtt.connected()) { return; }
    if (millis() - lastMqttTry < MQTT_RECONNECT_MS) { return; }
    lastMqttTry = millis();

    Serial.printf("[MQTT] connect %s:%d ...\n",
                  ONENET_MQTT_HOST, ONENET_MQTT_PORT);
    if (!mqtt.connect(ONENET_MQTT_HOST, ONENET_MQTT_PORT,
                      ONENET_DEVICE_NAME, ONENET_PRODUCT_ID,
                      ONENET_TOKEN, MQTT_KEEPALIVE_S))
    {
        Serial.println("[MQTT] CONNECT failed");
        return;
    }
    if (!mqtt.subscribe(TOPIC_CMD_REQ))
    {
        Serial.println("[MQTT] SUBSCRIBE failed -> will retry in 3 s");
        mqtt.stop();
        return;
    }
    Serial.printf("[MQTT] ready, subscribed to %s\n", TOPIC_CMD_REQ);
}

/* -------------------------------------------------------------- arduino -- */
void setup()
{
    Serial.begin(115200);
    /* 原生 USB CDC 要等主机枚举完成，否则开机前几百毫秒的日志会整段丢失 */
    {
        uint32_t t0 = millis();
        while (!Serial && ((millis() - t0) < 3000u)) { delay(10); }
    }
    delay(400);
    /* setRxBufferSize() must be called before begin() to take effect. */
    Serial1.setRxBufferSize(4096);
    Serial1.begin(STM32_UART_BAUD, SERIAL_8N1,
                  STM32_UART_RX_PIN, STM32_UART_TX_PIN);

    cloudEnabled = (strlen(ONENET_PRODUCT_ID) > 0u)
                && (strlen(ONENET_DEVICE_NAME) > 0u)
                && (strlen(ONENET_TOKEN) > 0u);

    Serial.println("\n=== ESP32-S3 OneNET OTA bridge ===");
    Serial.printf("UART1: %d baud  RX=GPIO%d TX=GPIO%d\n",
                  STM32_UART_BAUD, STM32_UART_RX_PIN, STM32_UART_TX_PIN);
    Serial.printf("Cloud: %s\n", cloudEnabled ? "ENABLED" : "DISABLED (local bridge only)");
    if (!cloudEnabled)
    {
        Serial.println("Fill ONENET_PRODUCT_ID / DEVICE_NAME / TOKEN in include/onenet_config.h");
    }

#if UART_SELFTEST_ON_BOOT
    /* ---------------------------------------------------- UART 回环自检 --
     * 发一帧 CMD_LOG 出去，若 GPIO17 与 GPIO18 被短接，这帧会立刻被自己收到。
     * 必须校验「回环回来的帧内容」，不能只数收到几个字节 —— STM32 每隔一两秒
     * 就会推一帧 CMD_OTA_PROGRESS(22B)，光看字节数会把它误判成回环成功。 */
    {
        uint8_t  selftestFrame[64];
        uint8_t  echoFrame[CLOUD_FRAME_MAX];
        size_t   selftestLen;
        size_t   echoLen = 0u;
        uint32_t t0;
        bool     looped = false;
        const char *tag = "UART-SELFTEST";

        selftestLen = buildLogFrame(selftestFrame, tag);

        while (Serial1.available() > 0) { (void)Serial1.read(); }   /* 清上电噪声 */
        Serial1.write(selftestFrame, selftestLen);
        Serial1.flush();

        t0 = millis();
        while (!looped && ((millis() - t0) < 500u))
        {
            while ((Serial1.available() > 0) && !looped)
            {
                if (!uartAsm.feed((uint8_t)Serial1.read(), echoFrame, &echoLen))
                {
                    continue;
                }
                if ((frameCmd(echoFrame) == CMD_LOG) && (echoLen >= 10u + strlen(tag)))
                {
                    uint16_t ln = (uint16_t)echoFrame[8] | ((uint16_t)echoFrame[9] << 8);
                    if ((ln == (uint16_t)strlen(tag)) &&
                        (memcmp(&echoFrame[OTA_FRAME_HEADER_LEN], tag, strlen(tag)) == 0))
                    {
                        looped = true;
                    }
                }
            }
        }

        Serial.println("[SELFTEST] UART loopback: 短接 GPIO17 <-> GPIO18 后复位可验证");
        if (looped)
        {
            Serial.println("[SELFTEST] PASS  回环收到自己的帧 "
                           "-> ESP32 UART TX/RX/引脚/波特率 全部正常");
        }
        else
        {
            Serial.println("[SELFTEST] no echo（没短接就属正常）。"
                           "若 GPIO17/18 已短接仍无回显 -> 查 ESP32 引脚/焊接");
        }
    }
#endif

    /* 开机扫一次，日志里能直接看出目标 SSID 在不在、是不是企业级加密。 */
    WiFi.mode(WIFI_STA);
#if WIFI_SCAN_ON_BOOT
    wifiScanReport();
#endif
    Serial.println("[WiFi] boot scan done, entering connect loop");
}

void loop()
{
    static char     topic[192];
    static uint8_t  mqPayload[2048];
    static uint8_t  frame[CLOUD_FRAME_MAX];
    size_t          payloadLen = 0u, frameLen = 0u;
    static uint32_t lastPing = 0u;

    /* WiFi 无条件维护：即使还没填 OneNET 三个参数，也要在串口里能看到
     * 连接成功 / 失败原因，否则「连不上 WiFi」根本无法诊断。 */
    (void)ensureWifi();

    if (cloudEnabled)
    {
        ensureMqtt();
        if (mqtt.connected())
        {
            if (mqtt.poll(topic, sizeof(topic), mqPayload,
                          sizeof(mqPayload), &payloadLen))
            {
                /* 订阅的是 /#，会收到多种 topic，按完整字符串分发。
                   必须用 strcmp：set_reply 也以 /thing/property/set 打头。 */
                if (strcmp(topic, TOPIC_PROP_SET) == 0)
                {
                    handlePropertySet(mqPayload, payloadLen);
                }
                else
                {
                    handleCloudCommand(topic, mqPayload, payloadLen);
                }
            }
            if (millis() - lastPing > 30000u)
            {
                lastPing = millis();
                if (!mqtt.ping())
                {
                    /* ping() 只有在「上一次 PINGREQ 超时没回」时才会返回 false，
                       并已经 stop() 掉连接，下一轮 ensureMqtt() 会重连。
                       没有这行日志的话，这种半死连接在串口里完全看不出来。 */
                    Serial.println("[MQTT] no PINGRESP -> link dead, will reconnect");
                }
            }
        }
    }

    /* STM32 -> PC / OneNET */
    while (Serial1.available() > 0)
    {
        uint8_t rxByte = (uint8_t)Serial1.read();
        g_uartRxBytes++;
        if (!uartAsm.feed(rxByte, frame, &frameLen)) { continue; }
        g_uartFrames++;

#if UART_FRAME_TRACE
        /* 周期性状态帧 0x0031 每 1.7 秒一帧，会把追踪额度占满，跳过它。 */
        if ((g_uartFrames <= UART_FRAME_TRACE_MAX) &&
            (frameCmd(frame) != CMD_OTA_PROGRESS))
        {
            char hex[3 * 20 + 4];
            uint16_t ln = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8);
            uint16_t shown = (ln < 20u) ? ln : 20u;
            int i, o = 0;

            for (i = 0; i < (int)shown; ++i)
            {
                static const char *HD = "0123456789ABCDEF";
                uint8_t b = frame[OTA_FRAME_HEADER_LEN + i];
                hex[o++] = HD[b >> 4];
                hex[o++] = HD[b & 0x0Fu];
                hex[o++] = ' ';
            }
            hex[o] = '\0';
            Serial.printf("[TRACE] #%u cmd=0x%04X seq=%u flag=0x%02X len=%u total=%u data=%s\n",
                          (unsigned)g_uartFrames, frameCmd(frame), frameSeq(frame),
                          (unsigned)frame[3], (unsigned)ln, (unsigned)frameLen, hex);
        }
#endif

        /* CMD_LOG 是 STM32 的纯 ASCII 调试文本（Bootloader 和 APP 都用它），
           始终转成一行 [STM32] 打印出来 —— 这是唯一能看到对端在干什么的通道。
           注意：不要过滤 RESP 标志！STM32 的 ota_link_log() 是用
           send_frame(CMD_LOG, 0, txt, n, 1) 发的，resp=1 是故意的
           （让对端不要给日志帧回 ACK）。之前这里写了 !frameIsResp(frame)，
           结果 STM32 每一行日志都被当成「响应」丢掉，串口里一行都没有，
           看起来像 UART 完全没通。 */
        if (frameCmd(frame) == CMD_LOG)
        {
            uint16_t ln = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8);
            Serial.printf("[STM32] %.*s\n", (int)ln,
                          (const char *)&frame[OTA_FRAME_HEADER_LEN]);
        }

        /* 默认不往 USB 写二进制（会把串口刷成乱码），见文件顶部 USB_FRAME_OUTPUT。 */
#if USB_FRAME_OUTPUT == 2
        Serial.write(frame, frameLen);
#elif USB_FRAME_OUTPUT == 1
        Serial.printf("[UART] cmd=0x%04X seq=%u len=%u%s\n",
                      frameCmd(frame), frameSeq(frame),
                      (unsigned)((uint16_t)frame[8] | ((uint16_t)frame[9] << 8)),
                      frameIsResp(frame) ? " (resp)" : "");
#endif

        if (pending.active && frameIsResp(frame) &&
            (frameCmd(frame) == pending.cmd) &&
            (frameSeq(frame) == pending.seq))
        {
            size_t n = (frameLen < sizeof(pending.resp))
                     ? frameLen : sizeof(pending.resp);
            memcpy(pending.resp, frame, n);
            pending.respLen = n;
            replyToPlatform();
        }
        if ((frameCmd(frame) == CMD_OTA_PROGRESS) && mqtt.connected())
        {
            uint16_t ln = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8);
            reportProperties(&frame[OTA_FRAME_HEADER_LEN], ln);
        }
    }

    /* PC USB -> STM32 (local transparent bridge, same frames as cloud) */
    while (Serial.available() > 0)
    {
        int ch = Serial.read();
        if (ch < 0) { break; }
        /* 串口里敲一个小写 p：ESP32 自己发一帧 CMD_PING 给 STM32 并报结果。
         * 不用云、不用上位机，单靠这一条就能判断 ESP32->STM32 方向通不通。
         * 'p' = 0x70 不可能是帧头 0xAA，也不会被下面的组帧器误吃。 */
        if ((uint8_t)ch == (uint8_t)'p')
        {
            localPingTest();
            continue;
        }
        if (!usbAsm.feed((uint8_t)ch, frame, &frameLen)) { continue; }
        Serial1.write(frame, frameLen);
        Serial1.flush();
    }

    /* STM32 迟迟不回 ACK 时，不要让平台干等 30 秒：
     * 按命令种类给等待窗口（擦除类命令要等十几秒），超时才伪造 ST_TIMEOUT。 */
    if (pending.active && ((millis() - pending.startMs) > ackWaitMs(pending.cmd)))
    {
        Serial.printf("[CMD] id=%s STM32 timeout after %u ms, replying ST_TIMEOUT\n",
                      pending.cmdid, (unsigned)ackWaitMs(pending.cmd));
        makeTimeoutAck(pending.resp, &pending.respLen);
        replyToPlatform();
    }

    /* UART 链路统计：把「线上一个字节都没有」和「有电平但帧解析不出来」分开。
     * 只在数值变化时打印，不会刷屏。 */
    {
        static uint32_t lastReportMs = 0u;
        static uint32_t lastBytes    = 0xFFFFFFFFu;   /* 哨兵：保证开机后必打一次 */
        static uint32_t lastFrames   = 0xFFFFFFFFu;

        if ((millis() - lastReportMs) >= UART_STATS_EVERY_MS)
        {
            lastReportMs = millis();
            if ((g_uartRxBytes != lastBytes) || (g_uartFrames != lastFrames))
            {
                Serial.printf("[UART] rx=%u B, frames=%u\n",
                              (unsigned)g_uartRxBytes, (unsigned)g_uartFrames);
                if (g_uartRxBytes == 0u)
                {
                    Serial.println("[UART] 5 秒内 0 字节 = 这条线完全没有电平。"
                                   "查：1) GND 是否共地  2) STM32 PB14 -> ESP32 GPIO17  "
                                   "3) ESP32 GPIO18 -> STM32 PB15  4) 杜邦线是否通");
                }
                else if (g_uartFrames == 0u)
                {
                    Serial.println("[UART] 有字节但 0 个合法帧 = 波特率不匹配或线太长被干扰"
                                   "（两边都必须是 921600，杜邦线尽量短且远离电机/电源）");
                }
                lastBytes  = g_uartRxBytes;
                lastFrames = g_uartFrames;
            }
        }
    }

    delay(1);
}
