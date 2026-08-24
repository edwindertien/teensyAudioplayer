#pragma once
#include <Arduino.h>
#include "Config.h"

// ── Motor burst — esp_timer ISR ───────────────────────────────
// Fires independently of loop() timing.
// motorProxSet() is ignored while a burst is active.

struct MotorBurst {
    volatile bool     active  = false;
    volatile uint8_t  pulses  = 0;
    volatile uint8_t  done    = 0;
    volatile uint8_t  duty    = 0;
    volatile uint16_t onMs    = 0;
    volatile uint16_t offMs   = 0;
    volatile bool     motorOn = false;
} motorBurst;

esp_timer_handle_t motorTimer;

void IRAM_ATTR motorTimerCb(void*) {
    if (!motorBurst.active) return;
    if (motorBurst.motorOn) {
        ledcWrite(MOTOR_CH, 0);
        motorBurst.motorOn = false;
        esp_timer_start_once(motorTimer, motorBurst.offMs * 1000ULL);
    } else {
        if (++motorBurst.done >= motorBurst.pulses) {
            motorBurst.active = false; return;
        }
        ledcWrite(MOTOR_CH, motorBurst.duty);
        motorBurst.motorOn = true;
        esp_timer_start_once(motorTimer, motorBurst.onMs * 1000ULL);
    }
}

void motorSetup() {
    ledcSetup(MOTOR_CH, MOTOR_FREQ, MOTOR_RES);
    ledcAttachPin(MOTOR_PIN, MOTOR_CH);
    ledcWrite(MOTOR_CH, 0);
    esp_timer_create_args_t args = {};
    args.callback = motorTimerCb;
    args.name     = "motor";
    esp_timer_create(&args, &motorTimer);
}

void motorBurstStart(uint8_t pulses = 3, uint8_t duty = 200,
                     uint16_t onMs = 55, uint16_t offMs = 35) {
    esp_timer_stop(motorTimer);
    motorBurst.active  = true;  motorBurst.pulses  = pulses;
    motorBurst.done    = 0;     motorBurst.duty    = duty;
    motorBurst.onMs    = onMs;  motorBurst.offMs   = offMs;
    motorBurst.motorOn = true;
    ledcWrite(MOTOR_CH, duty);
    esp_timer_start_once(motorTimer, onMs * 1000ULL);
}

// Continuous intensity — ignored while burst is active
void motorProxSet(uint8_t duty) {
    if (!motorBurst.active) ledcWrite(MOTOR_CH, duty);
}
