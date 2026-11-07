/**
 * @file  mini_mqtt.cpp
 * @brief Minimal MQTT 3.1.1 over plain TCP, Arduino WiFiClient only.
 */
#include "mini_mqtt.h"

#define MQTT_CONNECT     0x10
#define MQTT_CONNACK     0x20
#define MQTT_PUBLISH     0x30
#define MQTT_SUBSCRIBE   0x82
#define MQTT_SUBACK      0x90
#define MQTT_PINGREQ     0xC0
#define MQTT_PINGRESP    0xD0

/* 等待 PINGRESP 的上限。校园网/手机热点 RTT 通常 <300ms，
   10 秒足够宽松；超过就认定这条 TCP 已经半死，主动 stop() 触发重连。 */
#define MQTT_PONG_TIMEOUT_MS   10000u

MiniMQTT::MiniMQTT(WiFiClient &client)
    : _c(&client), _packetId(1), _lastPingMs(0), _mqttReady(false),
      _pongPending(false), _pongSentMs(0)
{
}

bool MiniMQTT::encodeRemaining(uint32_t v, uint8_t *out, size_t *n)
{
    size_t i = 0;
    do
    {
        uint8_t d = (uint8_t)(v % 128u);
        v /= 128u;
        if (v > 0u) { d |= 0x80u; }
        out[i++] = d;
    } while ((v > 0u) && (i < 4u));
    *n = i;
    return true;
}

bool MiniMQTT::readRemaining(uint32_t *v)
{
    uint32_t multiplier = 1u;
    uint32_t value = 0u;
    uint8_t b;
    uint8_t i;

    for (i = 0u; i < 4u; ++i)
    {
        if (_c->readBytes(&b, 1) != 1u) { return false; }
        value += (uint32_t)(b & 0x7Fu) * multiplier;
        if ((b & 0x80u) == 0u)
        {
            *v = value;
            return true;
        }
        multiplier *= 128u;
    }
    return false;
}

uint16_t MiniMQTT::readU16()
{
    uint8_t hi, lo;
    if (_c->readBytes(&hi, 1) != 1u) { return 0u; }
    if (_c->readBytes(&lo, 1) != 1u) { return 0u; }
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

void MiniMQTT::skip(size_t n)
{
    uint8_t buf[32];
    while (n > 0u)
    {
        size_t take = (n > sizeof(buf)) ? sizeof(buf) : n;
        if (_c->readBytes(buf, take) != take) { return; }
        n -= take;
    }
}

bool MiniMQTT::connected()
{
    return (_c != nullptr) && _c->connected() && _mqttReady;
}

void MiniMQTT::stop()
{
    _mqttReady = false;
    if (_c != nullptr) { _c->stop(); }
}

bool MiniMQTT::connect(const char *host, uint16_t port,
                       const char *clientId, const char *username,
                       const char *password, uint16_t keepalive)
{
    uint8_t fixed[5];
    uint8_t varHeader[10];
    uint8_t lenBuf[2];
    size_t fixedLen = 0u;
    size_t i = 0u;
    uint32_t payloadLen, remaining;
    uint16_t cidLen, userLen, passLen;
    uint8_t ack[4];

    _mqttReady = false;
    _c->setTimeout(3000);
    if (!_c->connect(host, port)) { return false; }
    /* 读 CONNACK 的超时不能太短：走手机热点时 RTT 经常 300~800 ms，
       原来给 400 ms 会导致"服务器其实已经认证通过、本地却认为失败"，
       于是立刻重连、把刚建好的会话踢掉，表现为设备反复上下线。 */
    _c->setTimeout(2500);

    cidLen  = (uint16_t)strlen(clientId);
    userLen = (uint16_t)strlen(username);
    passLen = (uint16_t)strlen(password);
    payloadLen = 2u + cidLen + 2u + userLen + 2u + passLen;

    varHeader[i++] = 0x00; varHeader[i++] = 0x04;   /* protocol name len */
    varHeader[i++] = 'M';  varHeader[i++] = 'Q';
    varHeader[i++] = 'T';  varHeader[i++] = 'T';
    varHeader[i++] = 0x04;                          /* MQTT 3.1.1        */
    varHeader[i++] = 0xC2;                          /* clean + user + pw */
    varHeader[i++] = (uint8_t)(keepalive >> 8);
    varHeader[i++] = (uint8_t)(keepalive & 0xFFu);

    remaining = (uint32_t)i + payloadLen;
    fixed[0] = MQTT_CONNECT;
    encodeRemaining(remaining, &fixed[1], &fixedLen);
    ++fixedLen;

    _c->write(fixed, fixedLen);
    _c->write(varHeader, i);

    lenBuf[0] = (uint8_t)(cidLen >> 8);
    lenBuf[1] = (uint8_t)(cidLen & 0xFFu);
    _c->write(lenBuf, 2);
    _c->write((const uint8_t *)clientId, cidLen);

    lenBuf[0] = (uint8_t)(userLen >> 8);
    lenBuf[1] = (uint8_t)(userLen & 0xFFu);
    _c->write(lenBuf, 2);
    _c->write((const uint8_t *)username, userLen);

    lenBuf[0] = (uint8_t)(passLen >> 8);
    lenBuf[1] = (uint8_t)(passLen & 0xFFu);
    _c->write(lenBuf, 2);
    _c->write((const uint8_t *)password, passLen);
    _c->flush();

    if (_c->readBytes(ack, 4) != 4u)
    {
        Serial.println("[MQTT] CONNACK timeout");
        stop();                 /* 别把半开的 socket 留给下一次 connect */
        return false;
    }
    if ((ack[0] != MQTT_CONNACK) || (ack[1] != 0x02u) || (ack[3] != 0x00u))
    {
        Serial.printf("[MQTT] CONNACK refused: 0x%02X 0x%02X\n",
                      ack[2], ack[3]);
        stop();
        return false;
    }
    _mqttReady = true;
    _lastPingMs = millis();
    /* 握手期用宽超时，之后回到短超时，避免 poll() 在半包上卡太久 */
    _c->setTimeout(1000);
    return true;
}

bool MiniMQTT::subscribe(const char *topicFilter)
{
    uint8_t fixed[5];
    uint8_t qos = 0u;
    uint16_t tl = (uint16_t)strlen(topicFilter);
    uint32_t remaining = 2u + 2u + tl + 1u;
    size_t n = 0u;
    uint8_t pid[2], lenBuf[2];
    uint8_t ack[5];

    fixed[0] = MQTT_SUBSCRIBE;
    encodeRemaining(remaining, &fixed[1], &n);
    ++n;
    pid[0] = (uint8_t)(_packetId >> 8);
    pid[1] = (uint8_t)(_packetId & 0xFFu);
    lenBuf[0] = (uint8_t)(tl >> 8);
    lenBuf[1] = (uint8_t)(tl & 0xFFu);

    _c->write(fixed, n);
    _c->write(pid, 2);
    _c->write(lenBuf, 2);
    _c->write((const uint8_t *)topicFilter, tl);
    _c->write(&qos, 1);
    _c->flush();
    ++_packetId;

    /* SUBACK: 0x90 0x03 <pid hi> <pid lo> <return code> */
    if (_c->readBytes(ack, 5) != 5u)
    {
        Serial.println("[MQTT] SUBACK timeout");
        return false;
    }
    if (ack[0] != MQTT_SUBACK)
    {
        Serial.printf("[MQTT] expect SUBACK got 0x%02X\n", ack[0]);
        return false;
    }
    /* 0x80 = 平台拒绝该 topic 的订阅权限（换成 /# 就好了，见 main.cpp） */
    if (ack[4] == 0x80u)
    {
        Serial.printf("[MQTT] SUBACK refused 0x80 for \"%s\"\n", topicFilter);
        return false;
    }
    return true;
}

bool MiniMQTT::publish(const char *topic, const uint8_t *payload, size_t len)
{
    uint8_t fixed[5];
    uint16_t tl = (uint16_t)strlen(topic);
    uint32_t remaining = 2u + tl + (uint32_t)len;
    size_t n = 0u;
    uint8_t lenBuf[2];

    if (!connected()) { return false; }
    fixed[0] = MQTT_PUBLISH;
    encodeRemaining(remaining, &fixed[1], &n);
    ++n;
    lenBuf[0] = (uint8_t)(tl >> 8);
    lenBuf[1] = (uint8_t)(tl & 0xFFu);

    _c->write(fixed, n);
    _c->write(lenBuf, 2);
    _c->write((const uint8_t *)topic, tl);
    if (len > 0u) { _c->write(payload, len); }
    _c->flush();
    return true;
}

bool MiniMQTT::publishText(const char *topic, const char *text)
{
    return publish(topic, (const uint8_t *)text, strlen(text));
}

bool MiniMQTT::poll(char *topic, size_t topicCap,
                    uint8_t *payload, size_t payloadCap, size_t *payloadLen)
{
    int first;
    uint8_t type;
    uint32_t remaining;
    uint16_t tl;
    size_t plen, toRead;

    if (!_c->connected() || (_c->available() <= 0)) { return false; }
    first = _c->read();
    if (first < 0) { return false; }
    type = (uint8_t)(first >> 4);
    if (!readRemaining(&remaining)) { return false; }

    if (type == 3u)
    {
        if (remaining < 2u) { return false; }
        tl = readU16();
        if ((tl == 0u) || ((size_t)tl > topicCap - 1u) ||
            (remaining < (uint32_t)tl + 2u))
        {
            skip(remaining - 2u);
            return false;
        }
        if (_c->readBytes((uint8_t *)topic, tl) != tl) { return false; }
        topic[tl] = '\0';

        plen = (size_t)remaining - 2u - tl;
        toRead = (plen > payloadCap) ? payloadCap : plen;
        if (toRead > 0u)
        {
            if (_c->readBytes(payload, toRead) != toRead) { return false; }
        }
        if (plen > payloadCap) { skip(plen - payloadCap); }
        *payloadLen = toRead;
        return true;
    }

    /* PINGRESP：连接是活的，清掉探测标记。 */
    if (type == 13u)
    {
        _pongPending = false;
    }
    /* CONNACK / SUBACK / PINGRESP / PUBACK: just consume. */
    skip(remaining);
    return false;
}

/* 距离上次收到 PINGRESP 已经多久没应答（毫秒）。0 = 当前没有等待中的探测，
   调用方据此判断这条连接是不是已经半死。 */
uint32_t MiniMQTT::pingAgeMs() const
{
    return _pongPending ? (uint32_t)(millis() - _pongSentMs) : 0u;
}

bool MiniMQTT::ping()
{
    uint8_t req[2] = {MQTT_PINGREQ, 0x00};
    if (!connected()) { return false; }

    /* Ping 前先检查上一次有没有回来。
       为什么要这个：TCP 半开（对端已经把这个会话踢掉、本端却还没收到 RST）时
       write() 依然返回成功，_c->connected() 也依然是 true，于是固件会一直
       往一个死 socket 里发命令，而平台那边早就把设备标成「不在线」，
       所有下发的命令都返回 10411 设备不在线 —— 必须靠物理复位才能恢复。
       唯一能发现的办法就是等 PINGRESP。 */
    if (_pongPending && ((uint32_t)(millis() - _pongSentMs) > MQTT_PONG_TIMEOUT_MS))
    {
        stop();
        _pongPending = false;
        return false;
    }

    _c->write(req, 2);
    _c->flush();
    _lastPingMs = millis();
    _pongPending = true;
    _pongSentMs  = millis();
    return true;
}
