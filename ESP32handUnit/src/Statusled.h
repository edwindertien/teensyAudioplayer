#pragma once
#include <Arduino.h>
#include <FastLED.h>

// GPIO48 — onboard WS2812B on ESP32-S3 Super Mini
// Second FastLED controller alongside ring (GPIO4) — same show() updates both

#define STATUS_LED_PIN 48

CRGB statusLed[1];

// ── States — identical scheme to body bridge ──────────────────
//  NOT_PAIRED   red breathe       — no body bridge paired
//  PAIRED_IDLE  amber pulse       — paired, no sends for >2s
//  LINKED       green breathe     — actively sending proximity packets
//  NFC_EVENT    white flash       — NFC tag detected and sent
//  TOUCH_EVENT  cyan flash        — connection/touch event sent
// ─────────────────────────────────────────────────────────────

enum class StatusState {
    NOT_PAIRED,
    PAIRED_IDLE,
    LINKED,
    NFC_EVENT,
    TOUCH_EVENT
};

StatusState statusState   = StatusState::NOT_PAIRED;
uint32_t    flashUntil    = 0;
uint32_t    lastLinkedMs  = 0;   // time of last proximity send
uint32_t    statusTimer   = 0;
#define     LINK_TIMEOUT  2000   // ms without a send → revert to PAIRED_IDLE

void statusLedSetup() {
    FastLED.addLeds<WS2812B, STATUS_LED_PIN, GRB>(statusLed, 1);
    statusLed[0] = CRGB::Black;
}

// ── Event hooks ───────────────────────────────────────────────
void statusOnPaired() {
    if (statusState == StatusState::NOT_PAIRED) {
        statusState = StatusState::PAIRED_IDLE;
        Serial.println("[LED] paired → amber idle");
    }
}

void statusOnLinked() {
    lastLinkedMs = millis();
    if (statusState == StatusState::PAIRED_IDLE ||
        statusState == StatusState::NOT_PAIRED) {
        statusState = StatusState::LINKED;
        Serial.println("[LED] linked → green");
    }
}

void statusOnNfc() {
    statusState = StatusState::NFC_EVENT;
    flashUntil  = millis() + 400;
}

void statusOnTouch() {
    statusState = StatusState::TOUCH_EVENT;
    flashUntil  = millis() + 800;
}

void statusOnUnpaired() {
    statusState  = StatusState::NOT_PAIRED;
    lastLinkedMs = 0;
    Serial.println("[LED] unpaired → red");
}

// ── Update — call every loop ──────────────────────────────────
void statusLedUpdate(uint32_t now, bool paired) {

    // Sync connectivity state
    if (!paired) {
        if (statusState != StatusState::NFC_EVENT &&
            statusState != StatusState::TOUCH_EVENT)
            statusState = StatusState::NOT_PAIRED;
    } else if (statusState == StatusState::NOT_PAIRED) {
        statusState  = StatusState::PAIRED_IDLE;
    }

    // Expire flash states → return to link/idle
    if (now > flashUntil &&
        (statusState == StatusState::NFC_EVENT ||
         statusState == StatusState::TOUCH_EVENT)) {
        bool active = paired && lastLinkedMs && (now - lastLinkedMs < LINK_TIMEOUT);
        statusState = active ? StatusState::LINKED
                     : paired ? StatusState::PAIRED_IDLE
                     : StatusState::NOT_PAIRED;
    }

    // Linked → idle when no sends for LINK_TIMEOUT
    if (statusState == StatusState::LINKED &&
        lastLinkedMs && (now - lastLinkedMs > LINK_TIMEOUT)) {
        statusState = StatusState::PAIRED_IDLE;
        Serial.println("[LED] link timeout → amber idle");
    }

    if (now - statusTimer < 40) return;   // 25fps
    statusTimer = now;

    float br;
    switch (statusState) {
        case StatusState::NOT_PAIRED:
            br = 0.05f + 0.55f * ((sinf(now / 1500.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB((uint8_t)(br * 255), 0, 0);
            break;

        case StatusState::PAIRED_IDLE:
            br = 0.05f + 0.45f * ((sinf(now / 1000.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB((uint8_t)(br * 255), (uint8_t)(br * 120), 0);
            break;

        case StatusState::LINKED:
            br = 0.15f + 0.30f * ((sinf(now / 1200.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB(0, (uint8_t)(br * 255), (uint8_t)(br * 40));
            break;

        case StatusState::NFC_EVENT:
            statusLed[0] = CRGB(200, 200, 200);
            break;

        case StatusState::TOUCH_EVENT:
            statusLed[0] = CRGB(0, 180, 200);
            break;
    }

    FastLED.show();
}