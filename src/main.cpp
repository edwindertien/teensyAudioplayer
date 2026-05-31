#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SD.h>
#include "AudioPlaySdWavMulti.h"
#include "RamPlayer.h"

// ── Audio graph ───────────────────────────────────────────────
//
// 7.1 channel order: FL FR FC LFE BL BR SL SR
//   out0 FL  (base L)      → stemMixerL ch0
//   out1 FR  (base R)      → stemMixerR ch0
//   out2 FC  (narration)   → stemMixerL ch1 + stemMixerR ch1
//   out3 LFE (haptic)      → hapticMixer → AudioOutputPWM (pin 2+4)
//   out4 BL  (overlay1 L)  → stemMixerL ch2
//   out5 BR  (overlay1 R)  → stemMixerR ch2
//   out6 SL  (overlay2 L)  → stemMixerL ch3
//   out7 SR  (overlay2 R)  → stemMixerR ch3
//
//   stemMixerL/R → mainMixerL/R ch0  (all stems combined)
//   RAM fx       → mainMixerL/R ch1  (FX on dedicated channel)
//   mainMixerL/R → i2sOut
//
//  Pin 2 (MSB) + Pin 4 (LSB) for haptic PWM:
//    Pin 2 → 1kΩ  ──┬── 100nF to GND ──► transducer amp
//    Pin 4 → 270kΩ ─┘

AudioPlaySdWavMulti   multiPlayer;

// Stem mixer — base, narr, ov1, ov2
AudioMixer4           stemMixerL;
AudioMixer4           stemMixerR;

// Main mixer — stems + FX
AudioMixer4           mainMixerL;
AudioMixer4           mainMixerR;

AudioMixer4           hapticMixer;
AudioOutputI2S        i2sOut;
AudioOutputPWM        pwmOut;
AudioControlSGTL5000  codec;

RamPlayer             ramFx;

// ── Stem connections ──────────────────────────────────────────
// Base stereo (FL/FR)
AudioConnection  pBaseL   (multiPlayer, 0, stemMixerL, 0);
AudioConnection  pBaseR   (multiPlayer, 1, stemMixerR, 0);
// Narration mono (FC) → both channels
AudioConnection  pNarrL   (multiPlayer, 2, stemMixerL, 1);
AudioConnection  pNarrR   (multiPlayer, 2, stemMixerR, 1);
// Overlay 1 stereo (BL/BR)
AudioConnection  pOv1L    (multiPlayer, 4, stemMixerL, 2);
AudioConnection  pOv1R    (multiPlayer, 5, stemMixerR, 2);
// Overlay 2 stereo (SL/SR)
AudioConnection  pOv2L    (multiPlayer, 6, stemMixerL, 3);
AudioConnection  pOv2R    (multiPlayer, 7, stemMixerR, 3);

// ── Main mixer connections ────────────────────────────────────
// Stem bus → main mixer ch0
AudioConnection  pStemL   (stemMixerL,   0, mainMixerL, 0);
AudioConnection  pStemR   (stemMixerR,   0, mainMixerR, 0);
// RAM FX → main mixer ch1 (dedicated, no gain conflict with stems)
AudioConnection  pFxL     (ramFx.player, 0, mainMixerL, 1);
AudioConnection  pFxR     (ramFx.player, 0, mainMixerR, 1);

// ── Output connections ────────────────────────────────────────
AudioConnection  pOutL    (mainMixerL,   0, i2sOut,     0);
AudioConnection  pOutR    (mainMixerR,   0, i2sOut,     1);

// ── Haptic (LFE) ─────────────────────────────────────────────
AudioConnection  pHaptic  (multiPlayer,  3, hapticMixer, 0);
AudioConnection  pHapOut  (hapticMixer,  0, pwmOut,      0);

// ── Config ────────────────────────────────────────────────────
#define LED_PIN          13
#define EXPERIENCE_FILE  "experience.wav"

#define FX_TOUCH_DEVICE  0
#define FX_TOUCH_LOC_A   1
#define FX_TOUCH_LOC_B   2
#define FX_BOOT          3

#define GAIN_STEP        0.02f  // per 20ms tick → ~1s full fade

// ── State ─────────────────────────────────────────────────────
float narrCurrent = 0.0f, narrTarget = 0.0f;
float ov1Current  = 0.0f, ov1Target  = 0.0f;
float ov2Current  = 0.0f, ov2Target  = 0.0f;
bool  narrActive  = false;
bool  ov1Active   = false;
bool  ov2Active   = false;

elapsedMillis gainRampTimer;
elapsedMillis statusTimer;

char    serialBuf[32] = {};
uint8_t serialPos = 0;

// ── Seek crossfade state machine ──────────────────────────────
// IDLE → FADE_OUT → SEEK → FADE_IN → IDLE
enum class SeekState : uint8_t { IDLE, FADE_OUT, SEEK, FADE_IN };
SeekState  seekState     = SeekState::IDLE;
uint32_t   seekTargetMs  = 0;
float      stemGain      = 1.0f;     // current stem bus gain (for seek crossfade)
#define    SEEK_FADE_STEP 0.05f      // per 20ms tick → ~400ms full fade, ~100ms to silence

// ── Helpers ───────────────────────────────────────────────────
float stepGain(float cur, float tgt) {
    if (fabsf(cur - tgt) <= GAIN_STEP) return tgt;
    return cur + (tgt > cur ? GAIN_STEP : -GAIN_STEP);
}

void handleCommand(const char* cmd) {
    if (strcmp(cmd, "narr") == 0) {
        narrActive = !narrActive;
        narrTarget = narrActive ? 1.0f : 0.0f;
        Serial.printf("[Narr] %s\n", narrActive ? "ON" : "OFF");

    } else if (strcmp(cmd, "ov1") == 0) {
        ov1Active = !ov1Active;
        ov1Target = ov1Active ? 1.0f : 0.0f;
        Serial.printf("[Ov1] %s\n", ov1Active ? "ON" : "OFF");

    } else if (strcmp(cmd, "ov2") == 0) {
        ov2Active = !ov2Active;
        ov2Target = ov2Active ? 1.0f : 0.0f;
        Serial.printf("[Ov2] %s\n", ov2Active ? "ON" : "OFF");

    } else if (strncmp(cmd, "seek", 4) == 0) {
        uint32_t ms = atoi(cmd + 5);
        seekTargetMs = ms;
        seekState    = SeekState::FADE_OUT;
        ramFx.play(1); // TODO: make transition FX slot configurable
        Serial.printf("[Seek] fading out -> %lu ms\n", ms);

    } else if (strncmp(cmd, "loop", 4) == 0) {
        uint32_t startMs = 0, endMs = 0;
        sscanf(cmd + 5, "%lu %lu", &startMs, &endMs);
        multiPlayer.setLoop(startMs, endMs);
        if (endMs == 0)
            Serial.println("[Loop] full file");
        else
            Serial.printf("[Loop] %lu ms -> %lu ms\n", startMs, endMs);

    } else if (strncmp(cmd, "fx", 2) == 0) {
        uint8_t slot = atoi(cmd + 3);
        ramFx.play(slot);
        Serial.printf("[FX] slot %u\n", slot);

    } else {
        Serial.println("Commands: narr | ov1 | ov2 | seek <ms> | loop <s> <e> | fx <n>");
    }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    pinMode(LED_PIN, OUTPUT);
    Serial.begin(115200);
    delay(500);
    Serial.println("=== Step 7: 7.1 Multichannel + FX cascade ===");

    AudioMemory(40);

    codec.enable();
    codec.volume(0.7);

    // Stem mixer — base always on, rest start silent
    stemMixerL.gain(0, 1.0f);   // base L
    stemMixerR.gain(0, 1.0f);   // base R
    stemMixerL.gain(1, 0.0f);   // narr
    stemMixerR.gain(1, 0.0f);
    stemMixerL.gain(2, 0.0f);   // ov1
    stemMixerR.gain(2, 0.0f);
    stemMixerL.gain(3, 0.0f);   // ov2
    stemMixerR.gain(3, 0.0f);

    // Main mixer — stems at full, FX at full (FX self-silences when not playing)
    mainMixerL.gain(0, 1.0f);   // stem bus
    mainMixerR.gain(0, 1.0f);
    mainMixerL.gain(1, 1.0f);   // FX
    mainMixerR.gain(1, 1.0f);
    mainMixerL.gain(2, 0.0f);   // unused
    mainMixerR.gain(2, 0.0f);
    mainMixerL.gain(3, 0.0f);   // unused
    mainMixerR.gain(3, 0.0f);

    hapticMixer.gain(0, 0.8f);

    if (!SD.sdfs.begin(SdioConfig(FIFO_SDIO))) {
        Serial.println("[SD] mount failed");
        while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(100); }
    }
    Serial.println("[SD] mounted OK");

    // Load RAM FX (skips gracefully if files not present)
    ramFx.load(FX_TOUCH_DEVICE, "fx_dev.wav");
    ramFx.load(FX_TOUCH_LOC_A,  "fx_loc_a.wav");
    ramFx.load(FX_TOUCH_LOC_B,  "fx_loc_b.wav");
    ramFx.load(FX_BOOT,         "fx_boot.wav");
    ramFx.printMemoryUsage();

    if (!multiPlayer.play(EXPERIENCE_FILE)) {
        Serial.println("[Audio] ERROR: experience.wav not found");
        Serial.println("[Audio] Run: ./assemble_71.sh base.wav narr.wav ov1.wav ov2.wav haptic.wav");
    } else {
        multiPlayer.setLoop(0, 0);
    }

    Serial.println("\nCommands: narr | ov1 | ov2 | seek <ms> | loop <s> <e> | fx <n>");
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {

    // Gain ramp every 20ms
    if (gainRampTimer >= 20) {
        gainRampTimer = 0;

        narrCurrent = stepGain(narrCurrent, narrTarget);
        ov1Current  = stepGain(ov1Current,  ov1Target);
        ov2Current  = stepGain(ov2Current,  ov2Target);

        stemMixerL.gain(1, narrCurrent);
        stemMixerR.gain(1, narrCurrent);
        stemMixerL.gain(2, ov1Current);
        stemMixerR.gain(2, ov1Current);
        stemMixerL.gain(3, ov2Current);
        stemMixerR.gain(3, ov2Current);

        // Seek crossfade state machine
        switch (seekState) {
            case SeekState::FADE_OUT:
                stemGain -= SEEK_FADE_STEP;
                if (stemGain <= 0.0f) {
                    stemGain  = 0.0f;
                    seekState = SeekState::SEEK;
                }
                mainMixerL.gain(0, stemGain);
                mainMixerR.gain(0, stemGain);
                break;

            case SeekState::SEEK:
                multiPlayer.seekMs(seekTargetMs);
                seekState = SeekState::FADE_IN;
                break;

            case SeekState::FADE_IN:
                stemGain += SEEK_FADE_STEP;
                if (stemGain >= 1.0f) {
                    stemGain  = 1.0f;
                    seekState = SeekState::IDLE;
                    Serial.printf("[Seek] done -> %lu ms\n", seekTargetMs);
                }
                mainMixerL.gain(0, stemGain);
                mainMixerR.gain(0, stemGain);
                break;

            case SeekState::IDLE:
            default:
                break;
        }
    }

    // Serial command parser
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialPos > 0) {
                serialBuf[serialPos] = '\0';
                serialPos = 0;
                handleCommand(serialBuf);
            }
        } else if (serialPos < sizeof(serialBuf) - 1) {
            serialBuf[serialPos++] = c;
        }
    }

    // Status every 3 seconds
    if (statusTimer >= 3000) {
        statusTimer = 0;
        Serial.printf("[%6lu ms] pos:%lu/%lu narr:%.2f ov1:%.2f ov2:%.2f  CPU:%.1f%%  mem:%u/%u\n",
                      millis(),
                      multiPlayer.positionMs(),
                      multiPlayer.lengthMs(),
                      narrCurrent, ov1Current, ov2Current,
                      AudioProcessorUsage(),
                      AudioMemoryUsage(),
                      AudioMemoryUsageMax());
    }
}