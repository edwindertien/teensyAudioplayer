#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "Proximity.h"
#include "TouchManager.h"
#include "StatusLed.h"

// ── Message types (shared with body bridge) ───────────────────
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
bool    espNowReady = false;
bool    peerPaired  = false;
bool    discovering = false;
uint8_t peerMac[6]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
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

// ── Callbacks ─────────────────────────────────────────────────
static void onSent(const uint8_t* mac, esp_now_send_status_t status) {
    // Note: ESP_NOW_SEND_SUCCESS only confirms the frame was transmitted,
    // not that the peer received it. Use heartbeat from body bridge instead.
}

static void onRecv(const uint8_t* srcMac, const uint8_t* data, int len) {
    if (len < 1) return;
    if (data[0] == MSG_HEARTBEAT) {
        // Body bridge is alive and receiving — go green
        statusOnLinked();
        return;
    }
    if (data[0] == MSG_PONG && discovering) {
        memcpy(peerMac, srcMac, 6);
        peerPaired  = true;
        discovering = false;
        espNowAddPeer(peerMac);
        Preferences prefs;
        prefs.begin("handunit", false);
        prefs.putBytes("peerMac", peerMac, 6);
        prefs.end();
        char buf[18]; macToStr(peerMac, buf);
        Serial.printf("[ESPNOW] Paired: %s\n", buf);
        statusOnPaired();
    }
}

// ── Setup ─────────────────────────────────────────────────────
void espNowSetup() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] init failed"); return;
    }
    esp_now_register_send_cb(onSent);
    esp_now_register_recv_cb(onRecv);
    espNowAddPeer(BROADCAST);

    Preferences prefs;
    prefs.begin("handunit", true);
    if (prefs.getBytesLength("peerMac") == 6) {
        prefs.getBytes("peerMac", peerMac, 6);
        peerPaired = true;
        espNowAddPeer(peerMac);
        char buf[18]; macToStr(peerMac, buf);
        Serial.printf("[ESPNOW] Loaded peer: %s\n", buf);
        statusOnPaired();
    } else {
        Serial.println("[ESPNOW] No peer — type 'espnow scan'");
    }
    prefs.end();
    Serial.printf("[ESPNOW] MAC: %s\n", WiFi.macAddress().c_str());
    espNowReady = true;
}

// ── Send ──────────────────────────────────────────────────────
void espNowSendProximity() {
    if (!espNowReady || !peerPaired) return;
    HandPacket pkt = {};
    pkt.type       = MSG_PROXIMITY;
    pkt.distMm     = lastDist;
    pkt.touchState = (uint8_t)touchState;
    esp_now_send(peerMac, (uint8_t*)&pkt, sizeof(pkt));
}

void espNowSendNfc(const char* uid) {
    if (!espNowReady) return;
    const uint8_t* dst = peerPaired ? peerMac : BROADCAST;
    HandPacket pkt = {};
    pkt.type       = MSG_NFC;
    pkt.distMm     = lastDist;
    pkt.touchState = (uint8_t)touchState;
    strlcpy(pkt.nfcUid, uid, sizeof(pkt.nfcUid));
    esp_now_send(dst, (uint8_t*)&pkt, sizeof(pkt));
}

void espNowSendTouch(const char* uid) {
    if (!espNowReady) return;
    const uint8_t* dst = peerPaired ? peerMac : BROADCAST;
    HandPacket pkt = {};
    pkt.type       = MSG_TOUCH;
    pkt.distMm     = lastDist;
    pkt.touchState = 1;
    strlcpy(pkt.nfcUid, uid, sizeof(pkt.nfcUid));
    esp_now_send(dst, (uint8_t*)&pkt, sizeof(pkt));
}

// ── CLI ───────────────────────────────────────────────────────
void espNowPrintStatus() {
    char buf[18];
    Serial.printf("ESPNOW: %s\n", espNowReady ? "ready" : "not init");
    if (peerPaired) { macToStr(peerMac, buf); Serial.printf("Peer:   %s\n", buf); }
    else              Serial.println("Peer:   none");
}

void espNowStartScan() {
    if (!espNowReady) { Serial.println("ESPNOW not ready"); return; }
    discovering = true;
    PingPacket pkt = {};
    pkt.type = MSG_PING;
    strlcpy(pkt.deviceId, "HAND_UNIT", sizeof(pkt.deviceId));
    esp_now_send(BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
    Serial.println("[ESPNOW] PING sent — waiting for body bridge...");
}

void espNowClearPair() {
    if (peerPaired) esp_now_del_peer(peerMac);
    peerPaired = false;
    memset(peerMac, 0xFF, 6);
    Preferences prefs;
    prefs.begin("handunit", false);
    prefs.remove("peerMac");
    prefs.end();
    statusOnUnpaired();
    Serial.println("[ESPNOW] Pairing cleared");
}

void espNowBindManual(const char* macStr) {
    uint8_t mac[6];
    if (sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6) {
        Serial.println("Usage: espnow bind AA:BB:CC:DD:EE:FF"); return;
    }
    if (peerPaired) esp_now_del_peer(peerMac);
    memcpy(peerMac, mac, 6);
    peerPaired = true;
    espNowAddPeer(peerMac);
    Preferences prefs;
    prefs.begin("handunit", false);
    prefs.putBytes("peerMac", peerMac, 6);
    prefs.end();
    char buf[18]; macToStr(peerMac, buf);
    Serial.printf("[ESPNOW] Bound to %s\n", buf);
    statusOnPaired();
}

void espNowSendTest() {
    if (!espNowReady) { Serial.println("ESPNOW not ready"); return; }
    const uint8_t* dst = peerPaired ? peerMac : BROADCAST;
    HandPacket pkt = {};
    pkt.type   = MSG_PROXIMITY;
    pkt.distMm = lastDist;
    strlcpy(pkt.nfcUid, "TEST", sizeof(pkt.nfcUid));
    esp_err_t r = esp_now_send(dst, (uint8_t*)&pkt, sizeof(pkt));
    Serial.printf("[ESPNOW] Test: %s\n", r==ESP_OK?"sent":"failed");
}