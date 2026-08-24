#pragma once
#include <Arduino.h>
#include <FastLED.h>

// GPIO48 — onboard WS2812B on ESP32-S3 Super Mini
#define STATUS_LED_PIN  48
#define STATUS_LED_N    1

CRGB statusLed[STATUS_LED_N];

// ── States ────────────────────────────────────────────────────
//  NOT_PAIRED   slow red breathe      — waiting for hand unit
//  PAIRED_IDLE  amber pulse           — paired but no packets >2s
//  LINKED       green breathe         — receiving proximity packets
//  NFC_EVENT    white flash           — NFC tag received
//  TOUCH_EVENT  cyan flash            — connection/touch event
// ─────────────────────────────────────────────────────────────

enum class StatusState {
    NOT_PAIRED,
    PAIRED_IDLE,
    LINKED,
    NFC_EVENT,
    TOUCH_EVENT
};

StatusState statusState  = StatusState::NOT_PAIRED;
uint32_t    flashUntil   = 0;   // ms — flash states hold until this time
uint32_t    ledTimer     = 0;   // 25fps update gate

void statusLedSetup() {
    FastLED.addLeds<WS2812B, STATUS_LED_PIN, GRB>(statusLed, STATUS_LED_N);
    FastLED.setBrightness(60);   // onboard LED — keep modest
    statusLed[0] = CRGB::Black;
    FastLED.show();
}

// Call from event handlers in EspNow.h
void statusOnNfc()   { statusState = StatusState::NFC_EVENT;   flashUntil = millis() + 400; }
void statusOnTouch() { statusState = StatusState::TOUCH_EVENT; flashUntil = millis() + 800; }
void statusOnLinked(){ if (statusState != StatusState::NFC_EVENT &&
                           statusState != StatusState::TOUCH_EVENT)
                           statusState = StatusState::LINKED; }
void statusOnPaired(){ if (statusState == StatusState::NOT_PAIRED)
                           statusState = StatusState::PAIRED_IDLE; }

// Call each loop — drives LED state machine
void statusLedUpdate(uint32_t now, bool paired, uint32_t lastRxMs) {

    // Determine base state from connectivity
    if (!paired) {
        if (statusState != StatusState::NFC_EVENT &&
            statusState != StatusState::TOUCH_EVENT)
            statusState = StatusState::NOT_PAIRED;
    } else if (now > flashUntil) {
        // Flash states expire → return to connectivity state
        bool recentRx = (lastRxMs > 0) && (now - lastRxMs < 2000);
        statusState = recentRx ? StatusState::LINKED : StatusState::PAIRED_IDLE;
    }

    // Rate-limit to 25fps
    if (now - ledTimer < 40) return;
    ledTimer = now;

    float br;
    switch (statusState) {

        case StatusState::NOT_PAIRED:
            // Slow red breathe — 3s period
            br = 0.05f + 0.55f * ((sinf(now / 1500.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB((uint8_t)(br * 255), 0, 0);
            break;

        case StatusState::PAIRED_IDLE:
            // Amber pulse — paired but quiet >2s
            br = 0.05f + 0.45f * ((sinf(now / 1000.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB((uint8_t)(br * 255), (uint8_t)(br * 120), 0);
            break;

        case StatusState::LINKED:
            // Steady gentle green — active link
            br = 0.15f + 0.30f * ((sinf(now / 1200.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB(0, (uint8_t)(br * 255), (uint8_t)(br * 40));
            break;

        case StatusState::NFC_EVENT:
            // White flash
            statusLed[0] = CRGB(200, 200, 200);
            break;

        case StatusState::TOUCH_EVENT:
            // Cyan flash
            statusLed[0] = CRGB(0, 180, 200);
            break;
    }

    FastLED.show();
}