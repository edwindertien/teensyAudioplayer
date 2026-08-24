#include <Arduino.h>
#include "Config.h"
#include "EspNow.h"
#include "StatusLed.h"
#include "Cli.h"

// ── Optional: UART to Teensy ──────────────────────────────────
// Uncomment when wired:
// void teensySetup() {
//     TEENSY_UART.begin(TEENSY_BAUD, SERIAL_8N1, TEENSY_RX, TEENSY_TX);
//     Serial.println("[UART] Teensy port ready");
// }

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Body Bridge ===");

    // teensySetup();  // uncomment when wired to Teensy
    statusLedSetup();
    espNowSetup();

    Serial.println("Waiting for hand unit...");
    printHelp();
}

uint32_t streamTimer    = 0;
uint32_t heartbeatTimer = 0;

void loop() {
    uint32_t now = millis();
    cliPoll();
    statusLedUpdate(now, handPaired, lastRxMs);

    // Heartbeat to hand unit every 1s
    if (now - heartbeatTimer >= 1000) {
        heartbeatTimer = now;
        espNowSendHeartbeat();
    }

    // Proximity stream to terminal (toggled via 'stream' command)
    if (streamProx && lastRxMs > 0 && (millis() - streamTimer >= 100)) {
        streamTimer = millis();
        uint8_t bars = (uint8_t)map(constrain(lastDist, 0, 500), 0, 500, 30, 0);
        char bar[32] = {}; memset(bar, '#', bars);
        Serial.printf("[PROX]  %4u mm  %s  touch:%u\n",
                      lastDist, bar, lastTouch);
    }
}