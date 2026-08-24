#pragma once
#include <Arduino.h>
#include "Motor.h"
#include "Proximity.h"
#include "LedRing.h"
#include "NfcTask.h"
#include "TouchManager.h"
#include "EspNow.h"
#include "PowerManager.h"

bool streaming = false;   // VL53L0X distance stream — toggled by 'stream' command

void printHelp() {
    Serial.println(
        "\nCommands:\n"
        "  status              — all sensor readings\n"
        "  stream              — toggle VL53L0X distance stream\n"
        "  scan                — I2C scan (both buses)\n"
        "  rainbow             — LED rainbow\n"
        "  solid <r> <g> <b>   — solid colour\n"
        "  bright <0-255>      — brightness\n"
        "  off / idle          — LED modes\n"
        "  motor <pulses>      — trigger burst\n"
        "  sleep               — go to deep sleep\n"
        "  battery             — read battery voltage\n"
        "  espnow              — ESP-NOW status\n"
        "  espnow scan         — discover and pair body bridge\n"
        "  espnow bind <MAC>   — manually set peer MAC\n"
        "  espnow clear        — clear stored pairing\n"
        "  espnow test         — send test packet\n"
    );
}

void handleCommand(const char* cmd) {
    if (strcmp(cmd, "status") == 0) {
        Serial.printf("NFC  %s  last: %s\n", nfcReady?"ok":"fail",
                      nfcLastUid[0] ? nfcLastUid : "none");
        Serial.printf("TOF  %s  dist: %u mm\n", tofReady?"ok":"fail", lastDist);
        Serial.printf("LED  mode:%d  bright:%u\n",
                      (int)ledMode, FastLED.getBrightness());
        Serial.printf("Touch: %s%s\n",
                      touchState==TouchState::RELEASED?"RELEASED":"IDLE",
                      touchState==TouchState::RELEASED
                          ? (millis()<silenceUntil?" (silence)":" (waiting separation)")
                          : "");

    } else if (strcmp(cmd, "stream") == 0) {
        streaming = !streaming;
        Serial.printf("VL53 stream: %s\n", streaming ? "ON" : "OFF");

    } else if (strcmp(cmd, "scan") == 0) {
        Serial.println("Wire (TOF bus):");
        for (uint8_t a = 1; a < 127; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0)
                Serial.printf("  0x%02X%s\n", a, a==0x29?" ← VL53L0X":"");
        }

    } else if (strcmp(cmd, "rainbow") == 0) {
        ledMode = LedMode::RAINBOW;

    } else if (strcmp(cmd, "off") == 0) {
        ledMode = LedMode::OFF; ledOff();

    } else if (strcmp(cmd, "idle") == 0) {
        ledMode = LedMode::IDLE;

    } else if (strncmp(cmd, "solid", 5) == 0) {
        int r=0,g=0,b=0;
        if (sscanf(cmd+6, "%d %d %d", &r, &g, &b) == 3) {
            solidColor = CRGB(r,g,b); ledMode = LedMode::SOLID; ledSolid(solidColor);
        } else Serial.println("Usage: solid <r> <g> <b>");

    } else if (strncmp(cmd, "bright", 6) == 0) {
        FastLED.setBrightness(constrain(atoi(cmd+7), 0, 255));

    } else if (strncmp(cmd, "motor", 5) == 0) {
        int n = atoi(cmd+6); if (n<=0) n=3;
        motorBurstStart(n, 200, 55, 35);

    } else if (strcmp(cmd, "sleep") == 0) {
        Serial.println("Sleeping...");
        powerSleep();

    } else if (strcmp(cmd, "battery") == 0) {
        uint16_t mv = readBatteryMv();
        Serial.printf("Battery: %u mV  (%s)\n", mv,
                      mv < VBAT_CRIT_MV ? "CRITICAL" :
                      mv < VBAT_LOW_MV  ? "LOW" : "OK");

    } else if (strncmp(cmd, "espnow", 6) == 0) {
        const char* sub = (strlen(cmd) > 7) ? cmd + 7 : "";
        if      (strcmp(sub, "scan")  == 0) espNowStartScan();
        else if (strcmp(sub, "clear") == 0) espNowClearPair();
        else if (strcmp(sub, "test")  == 0) espNowSendTest();
        else if (strncmp(sub, "bind", 4) == 0) espNowBindManual(sub + 5);
        else espNowPrintStatus();

    } else if (strcmp(cmd,"help")==0 || cmd[0]=='?') {
        printHelp();
    } else if (cmd[0] != '\0') {
        Serial.printf("Unknown: '%s'\n", cmd);
    }
}

char    cliBuf[64] = {};
uint8_t cliPos     = 0;

void cliPoll() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c=='\n'||c=='\r') {
            if (cliPos>0) { cliBuf[cliPos]='\0'; cliPos=0; handleCommand(cliBuf); }
        } else if (cliPos < sizeof(cliBuf)-1) {
            cliBuf[cliPos++] = c;
        }
    }
}