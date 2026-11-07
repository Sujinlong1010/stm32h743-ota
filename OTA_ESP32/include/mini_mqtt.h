/**
 * @file  mini_mqtt.h
 * @brief 极简 MQTT 3.1.1 客户端，只依赖 Arduino 自带 WiFiClient。
 *
 *  为什么自己写：OneNET 同步命令只需要 CONNECT / SUBSCRIBE / PUBLISH /
 *  PINGREQ，引入 PubSubClient 在国内受限网络下经常装不上，且它默认
 *  256B 缓冲区装不下 <1KB 的固件分片。这里按报文直接拼，零依赖。
 *
 *  只支持 QoS0（方案 B 的实时命令通道足够，可靠性由上位机 ACK 重试保证）。
 */
#ifndef MINI_MQTT_H
#define MINI_MQTT_H

#include <Arduino.h>
#include <WiFi.h>

class MiniMQTT
{
public:
    explicit MiniMQTT(WiFiClient &client);

    /* 建立 TCP + MQTT CONNECT。失败返回 false，可稍后重试。 */
    bool connect(const char *host, uint16_t port,
                 const char *clientId, const char *username,
                 const char *password, uint16_t keepalive = 60);

    bool subscribe(const char *topicFilter);

    /* QoS0 PUBLISH。payload 允许二进制。 */
    bool publish(const char *topic, const uint8_t *payload, size_t len);
    bool publishText(const char *topic, const char *text);

    /* 非阻塞：没有完整报文时立刻返回 false。收到 PUBLISH 时填充
       topic / payload 并返回 true；其它报文（SUBACK/PINGRESP）内部消化。 */
    bool poll(char *topic, size_t topicCap,
              uint8_t *payload, size_t payloadCap, size_t *payloadLen);

    bool ping();
    bool connected();
    void stop();

    /* 距离上次收到 PINGRESP 已经多久没应答（毫秒）。
       0 = 当前没有等待中的 PINGREQ。超过阈值说明这条 TCP 已经半死。 */
    uint32_t pingAgeMs() const;

private:
    WiFiClient *_c;
    uint16_t    _packetId;
    uint32_t    _lastPingMs;
    bool        _mqttReady;
    /* 发过 PINGREQ 但还没收到 PINGRESP = 正在探测。
       TCP 半开时 write() 照样成功，只有等不到 PINGRESP 才能发现连接已死。 */
    bool        _pongPending;
    uint32_t    _pongSentMs;

    static bool encodeRemaining(uint32_t v, uint8_t *out, size_t *n);
    bool readRemaining(uint32_t *v);
    uint16_t readU16();
    void skip(size_t n);
};

#endif /* MINI_MQTT_H */
