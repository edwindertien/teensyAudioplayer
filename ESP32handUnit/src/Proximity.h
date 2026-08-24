#pragma once
#include <Arduino.h>
#include <driver/gpio.h>
#include <Wire.h>
#include <VL53L0X.h>
#include "Config.h"

VL53L0X  tof;
bool     tofReady = false;
uint16_t lastDist = 9999;

// Valid measurement: 50–2000mm
// Below 50: crosstalk artifact. Above 2000 / 8190: no target.
inline bool distValid(uint16_t d) { return d >= 50 && d <= 2000; }

void proximitySetup() {
    gpio_hold_dis((gpio_num_t)TOF_XSHUT);  // release hold from sleep
    // Wire is used by VL53L0X (Pololu lib uses global Wire)
    Wire.begin(TOF_SDA, TOF_SCL, TOF_FREQ);

    pinMode(TOF_XSHUT, OUTPUT);
    digitalWrite(TOF_XSHUT, LOW);  delay(10);
    digitalWrite(TOF_XSHUT, HIGH); delay(10);

    tof.setTimeout(500);
    if (!tof.init()) {
        Serial.println("[TOF] VL53L0X not found");
        return;
    }
    tof.setSignalRateLimit(0.5);   // reduce crosstalk false positives
    tof.startContinuous(50);
    Serial.println("[TOF] VL53L0X ready");
    tofReady = true;
}

// Non-blocking: only reads when sensor has new data ready
void proximityRead() {
    if (!tofReady) return;
    if (!(tof.readReg(0x13) & 0x07)) return;    // RESULT_INTERRUPT_STATUS
    uint16_t d = tof.readRangeContinuousMillimeters();
    if (tof.timeoutOccurred()) return;
    if      (d >= 8190 || d < 50) lastDist = 9999;  // no target / crosstalk
    else if (d <= 2000)            lastDist = d;
}