#pragma once
#include <Arduino.h>
#include <driver/adc.h>
#include <esp_wifi.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include "Config.h"
#include "StatusLed.h"   // for low-battery blink
#include "Motor.h"       // stop motor before sleep
#include "LedRing.h"     // LEDs off before sleep

// ── Wakeup cause ──────────────────────────────────────────────
void powerPrintWakeReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:
            Serial.println("[PWR] Woke from button press"); break;
        case ESP_SLEEP_WAKEUP_TIMER:
            Serial.println("[PWR] Woke from timer"); break;
        default:
            Serial.println("[PWR] Normal boot (not waking from sleep)"); break;
    }
}

// ── Power-down sequence ───────────────────────────────────────
// Call before esp_deep_sleep_start()
void powerDownPeripherals() {
    // ── 1. LEDs off FIRST — before WiFi stop resets the RMT peripheral
    fill_solid(leds, LED_COUNT, CRGB::Black);
    statusLed[0] = CRGB::Black;
    FastLED.show();
    delay(10);

    // ── 2. Motor off
    ledcWrite(MOTOR_CH, 0);

    // ── 3. Peripherals to standby + hold pins LOW through deep sleep
    // Without gpio_hold_en(), outputs float to HIGH on sleep entry
    // causing PN532/VL53 to power back on during sleep
    #if NFC_RST_PIN >= 0
    digitalWrite(NFC_RST_PIN, LOW);
    gpio_hold_en((gpio_num_t)NFC_RST_PIN);   // latch LOW through sleep
    #endif
    digitalWrite(TOF_XSHUT, LOW);
    gpio_hold_en((gpio_num_t)TOF_XSHUT);     // latch LOW through sleep

    // ── 4. WiFi/ESP-NOW off (after LEDs — resets RMT)
    esp_wifi_stop();
    delay(150);   // let WiFi actually stop
}

// ── Minimal sleep — call before peripherals are initialised ──────
// Used at boot to immediately go to sleep on fresh power-on.
// Full powerSleep() is used at runtime (shuts down peripherals first).
void powerSleepNow() {
    // Use INPUT_PULLUP + gpio_hold_en() — simpler than rtc_gpio_* and more reliable.
    // gpio_hold_en() latches the current HIGH state through deep sleep
    // so the pin cannot float to LOW and trigger a spurious wakeup.
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    delay(20);                                   // let pull-up charge up

    gpio_hold_en((gpio_num_t)BUTTON_PIN);        // latch HIGH through sleep

    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
    Serial.println("[PWR] Sleeping — press button to wake");
    delay(50);
    esp_deep_sleep_start();
    // Never returns
}

// ── Sleep ─────────────────────────────────────────────────────
void powerSleep() {
    Serial.println("[PWR] Sleep in 500ms — release button");
    delay(100);   // flush serial

    powerDownPeripherals();

    // Wait for button release after peripherals are down
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    delay(300);   // generous debounce after release

    // Latch button pin HIGH before sleep — prevents floating → spurious wake
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    delay(20);
    gpio_hold_en((gpio_num_t)BUTTON_PIN);

    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);

    Serial.println("[PWR] Deep sleep — press button to wake");
    delay(50);
    esp_deep_sleep_start();
    // Never returns
}

// ── Battery monitor ───────────────────────────────────────────
uint32_t lastBatCheckMs = 0;
uint16_t lastBatMv      = 0;
bool     batLow         = false;
bool     batCritical    = false;

// Returns battery voltage in mV (via 220k+220k 1:2 divider on VBAT_ADC_PIN)
// Uses analogReadMilliVolts() which applies ESP32 eFuse Vref calibration —
// much more accurate than raw ADC × 3300/4095 (which overestimates by ~7%)
uint16_t readBatteryMv() {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogReadMilliVolts(VBAT_ADC_PIN);
        delay(2);
    }
    // ×2 for the 1:2 (220k+220k) voltage divider
    return (uint16_t)((sum / 8) * 2);
}

void batterySetup() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);   // 0-3.3V range
    pinMode(VBAT_ADC_PIN, INPUT);
    lastBatCheckMs = millis();
    lastBatMv = readBatteryMv();
    Serial.printf("[BAT] %u mV\n", lastBatMv);
}

// Returns true if battery is critically low (caller should sleep)
bool batteryUpdate(uint32_t now) {
    if (now - lastBatCheckMs < VBAT_CHECK_MS) return false;
    lastBatCheckMs = now;

    lastBatMv  = readBatteryMv();
    batLow      = lastBatMv < VBAT_LOW_MV;
    batCritical = lastBatMv < VBAT_CRIT_MV;

    Serial.printf("[BAT] %u mV%s\n", lastBatMv,
                  batCritical ? " CRITICAL" : batLow ? " LOW" : "");

    if (batCritical) {
        Serial.println("[BAT] Critical — shutting down to protect battery");
        // Rapid red blink before sleep
        for (int i = 0; i < 6; i++) {
            statusLed[0] = (i % 2 == 0) ? CRGB(255,0,0) : CRGB::Black;
            FastLED.show(); delay(120);
        }
        powerSleep();
    }
    return false;
}

void batteryLowBlink(uint32_t now) {
    if (!batLow) return;
    // Rapid orange blink on status LED every 200ms
    static uint32_t lastBlink = 0;
    static bool blinkOn = false;
    if (now - lastBlink >= 200) {
        lastBlink = now;
        blinkOn = !blinkOn;
        statusLed[0] = blinkOn ? CRGB(255, 80, 0) : CRGB::Black;
        FastLED.show();
    }
}

// ── Button state machine ──────────────────────────────────────
// Detects long press (USE_LONG_PRESS=1) or double press (USE_LONG_PRESS=0)

enum class BtnState { IDLE, PRESSED, HELD, RELEASED };
BtnState btnState    = BtnState::IDLE;
uint32_t btnDownMs   = 0;      // time button went down
uint32_t btnUpMs     = 0;      // time button last released
uint8_t  pressCount  = 0;      // for double-press counting

void buttonSetup() {
    gpio_hold_dis((gpio_num_t)BUTTON_PIN);  // release hold latched before sleep
    pinMode(BUTTON_PIN, INPUT_PULLUP);
}

// Returns true when sleep trigger fires
bool buttonUpdate(uint32_t now) {
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);

    if (USE_LONG_PRESS) {
        // ── Long press mode ───────────────────────────────────
        switch (btnState) {
            case BtnState::IDLE:
                if (pressed) { btnState = BtnState::PRESSED; btnDownMs = now; }
                break;
            case BtnState::PRESSED:
                if (!pressed) { btnState = BtnState::IDLE; }   // released early
                else if (now - btnDownMs >= POWER_HOLD_MS) {
                    btnState = BtnState::HELD;
                    // Pulse status LED to confirm
                    statusLed[0] = CRGB(255, 60, 0); FastLED.show();
                    delay(200);
                    statusLed[0] = CRGB::Black; FastLED.show();
                    return true;  // → sleep
                }
                break;
            case BtnState::HELD:
                if (!pressed) btnState = BtnState::IDLE;
                break;
            default: break;
        }
    } else {
        // ── Double press mode ─────────────────────────────────
        switch (btnState) {
            case BtnState::IDLE:
                if (pressed) {
                    btnState   = BtnState::PRESSED;
                    btnDownMs  = now;
                }
                break;
            case BtnState::PRESSED:
                if (!pressed) {
                    btnState = BtnState::RELEASED;
                    btnUpMs  = now;
                    pressCount++;
                }
                break;
            case BtnState::RELEASED:
                // Timeout window: single press, reset
                if (now - btnUpMs > DOUBLE_PRESS_MS) {
                    pressCount = 0;
                    btnState   = BtnState::IDLE;
                }
                // Second press within window
                if (pressed) {
                    if (pressCount >= 1) {
                        pressCount = 0;
                        btnState   = BtnState::IDLE;
                        return true;  // → sleep
                    }
                    btnState  = BtnState::PRESSED;
                    btnDownMs = now;
                }
                break;
            default: break;
        }
    }
    return false;
}

// ── Unified power update — call from loop() ───────────────────
void powerUpdate(uint32_t now) {
    if (buttonUpdate(now)) powerSleep();
    batteryUpdate(now);
    batteryLowBlink(now);
}