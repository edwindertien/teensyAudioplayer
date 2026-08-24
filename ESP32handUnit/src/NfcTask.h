#pragma once
#include <Arduino.h>
#include "Config.h"

#if NFC_UART
  #include <PN532_HSU.h>
  #include <PN532.h>
  HardwareSerial NfcSerial(1);    // named instance — gives us pin control
  PN532_HSU pn532hsu(NfcSerial);
  PN532     nfc(pn532hsu);
#else
  #include <Wire.h>
  #include <PN532_I2C.h>
  #include <PN532.h>
  PN532_I2C pn532i2c(Wire1);
  PN532     nfc(pn532i2c);
#endif
bool          nfcReady     = false;
QueueHandle_t nfcQueue;            // posts char[20] UIDs to main loop
char          nfcLastUid[20] = {};
bool          nfcTagPresent  = false;

void nfcReinit() {
    Serial.println("[NFC] reinitialising...");
    #if NFC_RST_PIN >= 0
    digitalWrite(NFC_RST_PIN, LOW);  vTaskDelay(pdMS_TO_TICKS(10));
    digitalWrite(NFC_RST_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(150));
    #endif
    #if NFC_UART
    NfcSerial.begin(NFC_BAUD, SERIAL_8N1, NFC_RX_PIN, NFC_TX_PIN);
    vTaskDelay(pdMS_TO_TICKS(50));
    { uint8_t w[]={0x55,0x55,0x00,0x00,0x00,0x00,
                   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
      NfcSerial.write(w,sizeof(w)); }
    vTaskDelay(pdMS_TO_TICKS(10));
    // Do NOT call nfc.begin() in UART mode — resets Serial1 pins
    #else
    nfc.begin();   // I2C only
    #endif
    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        Serial.println("[NFC] reinit failed");
        nfcReady = false; return;
    }
    nfc.SAMConfig();
    nfc.setPassiveActivationRetries(0xFF);
    nfcReady = true;
    Serial.printf("[NFC] reinit ok fw %u.%u\n", (ver>>16)&0xFF, (ver>>8)&0xFF);
}


// ── PN532 Power management ────────────────────────────────────
// PowerDown draws ~5µA vs ~80mA with RF active.
// Wake is via UART preamble (~2ms), so total cycle is:
//   NFC_SLEEP_MS (off) + ~20ms (poll) per detection window.

void nfcSendWakeup() {
    uint8_t w[] = {0x55,0x55,0x00,0x00,0x00,0x00,
                   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    NfcSerial.write(w, sizeof(w));
    delay(2);
}

void nfcPowerDown() {
#if NFC_SLEEP_MS > 0
    // PN532 HSU PowerDown packet:
    // preamble + start + len=3 + lchk + TFI(D4) + cmd(16) + WakeUpEnable(20=HSU) + dchk + postamble
    const uint8_t dchk = (uint8_t)(0x100 - ((0xD4 + 0x16 + 0x20) & 0xFF));
    uint8_t pkt[] = {0x00, 0x00, 0xFF, 0x03, 0xFD,
                     0xD4, 0x16, 0x20, dchk, 0x00};
    NfcSerial.write(pkt, sizeof(pkt));
    NfcSerial.flush();
    // PN532 powers down immediately — don't wait for response
#endif
}

void nfcTask(void*) {
    uint32_t lastSuccessMs = millis();
    for (;;) {
        if (millis() - lastSuccessMs > 8000) {
            nfcReinit();
            lastSuccessMs = millis();
        }

        // Wake PN532 from PowerDown before polling
#if NFC_SLEEP_MS > 0
        nfcSendWakeup();
#endif

        uint8_t uid[7] = {}, len = 0;
        bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50);
        lastSuccessMs = millis();

        if (!found || len == 0) {
            if (nfcTagPresent) { nfcTagPresent = false; nfcLastUid[0] = '\0'; }
            // Power down between polls — saves ~75mA during sleep
            nfcPowerDown();
#if NFC_SLEEP_MS > 0
            vTaskDelay(pdMS_TO_TICKS(NFC_SLEEP_MS));
#else
            vTaskDelay(pdMS_TO_TICKS(20));
#endif
            continue;
        }

        char s[20] = {};
        for (uint8_t i = 0; i < len; i++) sprintf(s + i*2, "%02X", uid[i]);
        s[len*2] = '\0';
        if (nfcTagPresent && strcmp(s, nfcLastUid) == 0) {
            // Tag still present — keep RF on for fast tracking, short sleep
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        nfcTagPresent = true;
        strlcpy(nfcLastUid, s, sizeof(nfcLastUid));
        xQueueSend(nfcQueue, s, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void nfcSetup() {
    // Create queue first — loop() calls xQueueReceive regardless of NFC state
    nfcQueue = xQueueCreate(8, 20);

#if NFC_UART
    // ── UART init sequence (order matters for reliable wake after deep sleep) ──
    //
    // 1. Assert RST LOW IMMEDIATELY after releasing hold.
    //    Without this, module pull-up floats pin HIGH and PN532 starts
    //    booting mid-sequence, corrupting the RST pulse that follows.
    #if NFC_RST_PIN >= 0
    gpio_hold_dis((gpio_num_t)NFC_RST_PIN);
    pinMode(NFC_RST_PIN, OUTPUT);
    digitalWrite(NFC_RST_PIN, LOW);   // hold in reset during serial setup
    #endif

    // 2. Configure UART with our pins while PN532 is held in reset
    NfcSerial.begin(NFC_BAUD, SERIAL_8N1, NFC_RX_PIN, NFC_TX_PIN);
    delay(20);
    while (NfcSerial.available()) NfcSerial.read();  // flush any garbage

    // 3. Release reset, wait for PN532 to fully boot
    #if NFC_RST_PIN >= 0
    digitalWrite(NFC_RST_PIN, HIGH);
    delay(200);   // PN532 needs ~150ms to boot; 200ms gives margin
    while (NfcSerial.available()) NfcSerial.read();  // flush boot output
    #else
    delay(100);
    while (NfcSerial.available()) NfcSerial.read();
    #endif

    // 4. After hardware RST, PN532 is fully awake — no wakeup preamble needed.
    //    Preamble is only for waking from software PowerDown (sent in nfcSendWakeup).
    //    Do NOT call nfc.begin() for UART — it resets Serial1 to default pins.
#else
    // I2C mode — nfc.begin() sets up Wire, safe to call
    Wire1.begin(NFC_SDA, NFC_SCL, NFC_FREQ);
    pinMode(NFC_RST_PIN, OUTPUT);
    digitalWrite(NFC_RST_PIN, LOW);  delay(10);
    digitalWrite(NFC_RST_PIN, HIGH); delay(150);
    nfc.begin();
#endif
    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        Serial.println("[NFC] not found — DIP SEL0=HIGH SEL1=LOW");
        return;
    }
    nfc.SAMConfig();
    nfc.setPassiveActivationRetries(0xFF);
    Serial.printf("[NFC] PN532 fw %u.%u ready\n", (ver>>16)&0xFF, (ver>>8)&0xFF);
    nfcReady = true;

    xTaskCreatePinnedToCore(nfcTask, "nfc", 4096, NULL, 1, NULL, 0);
    Serial.println("[NFC] task on Core 0");
}