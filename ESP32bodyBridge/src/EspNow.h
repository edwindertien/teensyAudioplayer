#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "StatusLed.h"

// ── Shared message types (must match hand unit) ───────────────
#define MSG_PROXIMITY  0x01
#define MSG_NFC        0x02
#define MSG_TOUCH      0x03
#define MSG_PING       0x10
#define MSG_PONG       0x11
#define MSG_HEARTBEAT  0x12

struct __attribute__((packed)) HandPacket {
    uint8_t  type;
    uint16_t distMm;
    uint8_t  touchState;
    char     nfcUid[20];
};

struct __attribute__((packed)) PingPacket {
    uint8_t type;
    char    deviceId[16];
};

// ── State ─────────────────────────────────────────────────────
bool    espNowReady  = false;
bool    handPaired   = false;
uint8_t handMac[6]   = {};
uint32_t lastRxMs    = 0;    // time of last received packet
uint16_t lastDist    = 9999;
uint8_t  lastTouch   = 0;
char     lastUid[20] = {};

static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void macToStr(const uint8_t* mac, char* buf) {
    sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

static void espNowAddPeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

// ── Send PONG back to hand unit ───────────────────────────────
static void sendPong(const uint8_t* toMac) {
    PingPacket pkt = {};
    pkt.type = MSG_PONG;
    strlcpy(pkt.deviceId, "BODY_BRIDGE", sizeof(pkt.deviceId));
    esp_now_send(toMac, (uint8_t*)&pkt, sizeof(pkt));
}

// ── Receive callback ──────────────────────────────────────────
static void onRecv(const uint8_t* srcMac, const uint8_t* data, int len) {
    if (len < 1) return;
    lastRxMs = millis();
    uint8_t type = data[0];

    // ── Discovery: PING from hand unit ────────────────────────
    if (type == MSG_PING) {
        char buf[18]; macToStr(srcMac, buf);
        Serial.printf("[ESPNOW] PING from %s — sending PONG\n", buf);
        espNowAddPeer(srcMac);
        sendPong(srcMac);
        // Store as paired hand unit
        memcpy(handMac, srcMac, 6);
        handPaired = true;
        Preferences prefs;
        prefs.begin("bodybridge", false);
        prefs.putBytes("handMac", handMac, 6);
        prefs.end();
        Serial.printf("[ESPNOW] Paired with hand unit: %s\n", buf);
        statusOnPaired();
        return;
    }

    // ── Only process packets from paired hand unit ────────────
    if (handPaired && memcmp(srcMac, handMac, 6) != 0) return;

    if (len < (int)sizeof(HandPacket)) return;
    const HandPacket* pkt = (const HandPacket*)data;

    lastDist  = pkt->distMm;
    lastTouch = pkt->touchState;
    strlcpy(lastUid, pkt->nfcUid, sizeof(lastUid));

    switch (type) {

        case MSG_PROXIMITY:
            statusOnLinked();   // active link
            break;

        case MSG_NFC:
            statusOnNfc();
            Serial.printf("[NFC]   uid=%-16s  dist=%4u mm  touch=%u\n",
                          pkt->nfcUid, pkt->distMm, pkt->touchState);
            // TODO: forward to Teensy
            // TEENSY_UART.printf("{\"t\":\"n\",\"u\":\"%s\",\"d\":%u}\n",
            //                    pkt->nfcUid, pkt->distMm);
            break;

        case MSG_TOUCH:
            statusOnTouch();
            Serial.printf("[TOUCH] uid=%-16s  dist=%4u mm  CONNECTION\n",
                          pkt->nfcUid, pkt->distMm);
            // TODO: forward to Teensy
            // TEENSY_UART.printf("{\"t\":\"c\",\"u\":\"%s\"}\n", pkt->nfcUid);
            break;

        default:
            Serial.printf("[ESPNOW] Unknown type 0x%02X len=%d\n", type, len);
    }
}

static void onSent(const uint8_t* mac, esp_now_send_status_t status) {}

// ── Setup ─────────────────────────────────────────────────────
void espNowSetup() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] init failed"); return;
    }
    esp_now_register_recv_cb(onRecv);
    esp_now_register_send_cb(onSent);
    espNowAddPeer(BROADCAST);

    // Load stored hand unit MAC
    Preferences prefs;
    prefs.begin("bodybridge", true);
    if (prefs.getBytesLength("handMac") == 6) {
        prefs.getBytes("handMac", handMac, 6);
        handPaired = true;
        espNowAddPeer(handMac);
        char buf[18]; macToStr(handMac, buf);
        Serial.printf("[ESPNOW] Loaded hand unit: %s\n", buf);
        statusOnPaired();
    } else {
        Serial.println("[ESPNOW] No hand unit paired — wait for hand unit to scan");
    }
    prefs.end();

    Serial.printf("[ESPNOW] Body bridge MAC: %s\n", WiFi.macAddress().c_str());
    espNowReady = true;
}

// ── Heartbeat — tell hand unit we are alive ───────────────────
void espNowSendHeartbeat() {
    if (!handPaired) return;
    uint8_t pkt[1] = { MSG_HEARTBEAT };
    esp_now_send(handMac, pkt, sizeof(pkt));
}

// ── CLI helpers ───────────────────────────────────────────────
void espNowPrintStatus() {
    char buf[18];
    Serial.printf("ESPNOW:  %s\n", espNowReady ? "ready" : "not init");
    if (handPaired) {
        macToStr(handMac, buf);
        Serial.printf("Hand:    %s\n", buf);
    } else {
        Serial.println("Hand:    not paired");
    }
    if (lastRxMs > 0) {
        Serial.printf("Last rx: %lums ago\n", millis() - lastRxMs);
        Serial.printf("Dist:    %u mm  Touch: %u\n", lastDist, lastTouch);
        if (lastUid[0]) Serial.printf("Last UID: %s\n", lastUid);
    } else {
        Serial.println("Last rx: none");
    }
}

void espNowClearPair() {
    if (handPaired) esp_now_del_peer(handMac);
    handPaired = false;
    memset(handMac, 0, 6);
    Preferences prefs;
    prefs.begin("bodybridge", false);
    prefs.remove("handMac");
    prefs.end();
    Serial.println("[ESPNOW] Pairing cleared");
}