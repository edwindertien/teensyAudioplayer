#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SD.h>
#include "NfcReader.h"
#include "AudioPlaySdWavMulti.h"
#include "RamPlayer.h"
#include "ConfigLoader.h"
#include "LedAnimator.h"

// ── Audio graph ───────────────────────────────────────────────
AudioPlaySdWavMulti   multiPlayer;
AudioMixer4           stemMixerL;
AudioMixer4           stemMixerR;
AudioMixer4           mainMixerL;
AudioMixer4           mainMixerR;
AudioMixer4           hapticMixer;
AudioAnalyzePeak      hapticPeak;
AudioOutputI2S        i2sOut;
AudioOutputPWM        pwmOut;
AudioControlSGTL5000  codec;
RamPlayer             ramFx;

AudioConnection  pBaseL  (multiPlayer, 0, stemMixerL, 0);
AudioConnection  pBaseR  (multiPlayer, 1, stemMixerR, 0);
AudioConnection  pNarrL  (multiPlayer, 2, stemMixerL, 1);
AudioConnection  pNarrR  (multiPlayer, 2, stemMixerR, 1);
AudioConnection  pOv1L   (multiPlayer, 4, stemMixerL, 2);
AudioConnection  pOv1R   (multiPlayer, 5, stemMixerR, 2);
AudioConnection  pOv2L   (multiPlayer, 6, stemMixerL, 3);
AudioConnection  pOv2R   (multiPlayer, 7, stemMixerR, 3);
AudioConnection  pStemL  (stemMixerL,  0, mainMixerL, 0);
AudioConnection  pStemR  (stemMixerR,  0, mainMixerR, 0);
AudioConnection  pFxL    (ramFx.player,0, mainMixerL, 1);
AudioConnection  pFxR    (ramFx.player,0, mainMixerR, 1);
AudioConnection  pOutL   (mainMixerL,  0, i2sOut,     0);
AudioConnection  pOutR   (mainMixerR,  0, i2sOut,     1);
AudioConnection  pHaptic (multiPlayer, 3, hapticMixer, 0);
AudioConnection  pHapOut (hapticMixer, 0, pwmOut,      0);
AudioConnection  pHapPeak(hapticMixer, 0, hapticPeak,  0);

// ── NFC ───────────────────────────────────────────────────────
LedAnimator leds;
NfcReader nfc;

// ── Config ────────────────────────────────────────────────────
#define LED_PIN          13
#define EXPERIENCE_FILE  "experience.wav"
#define GAIN_STEP        0.02f

ExperienceConfig     cfg;
const ChapterConfig* currentChapter = nullptr;

// ── Overlay state ─────────────────────────────────────────────
float narrCurrent = 0.0f, narrTarget = 0.0f;
float ov1Current  = 0.0f, ov1Target  = 0.0f;
float ov2Current  = 0.0f, ov2Target  = 0.0f;

// ── Seek crossfade ────────────────────────────────────────────
enum class SeekState : uint8_t { IDLE, FADE_OUT, SEEK, FADE_IN };
SeekState  seekState    = SeekState::IDLE;
uint32_t   seekTargetMs = 0;
float      stemGain     = 1.0f;
#define    SEEK_FADE_STEP 0.05f

bool          experienceStarted = false;

elapsedMillis gainRampTimer;
elapsedMillis nfcPollTimer;
elapsedMillis statusTimer;
uint16_t      _nfcPollInterval = 164;   // ms between NFC polls, set from sensitivity
elapsedMillis tagCooldown[MAX_TAGS];  // per-tag retrigger cooldown
char    serialBuf[32] = {};
uint8_t serialPos = 0;

// ── Helpers ───────────────────────────────────────────────────
float stepGain(float cur, float tgt) {
    if (fabsf(cur - tgt) <= GAIN_STEP) return tgt;
    return cur + (tgt > cur ? GAIN_STEP : -GAIN_STEP);
}

void applyOverlayDefaults(const ChapterConfig& ch) {
    if (ch.overlayMode == OverlayMode::KEEP) {
        Serial.println("[Chapter] Keeping overlay state");
        return;
    }
    ov1Target  = ch.overlays.ov1  ? 1.0f : 0.0f;
    ov2Target  = ch.overlays.ov2  ? 1.0f : 0.0f;
    narrTarget = ch.overlays.narr ? 1.0f : 0.0f;
    Serial.printf("[Chapter] Overlays: ov1=%s ov2=%s narr=%s\n",
                  ch.overlays.ov1  ? "ON":"OFF",
                  ch.overlays.ov2  ? "ON":"OFF",
                  ch.overlays.narr ? "ON":"OFF");
}

void goToChapter(const char* id) {
    const ChapterConfig* ch = ConfigLoader::findChapter(id, cfg);
    if (!ch) { Serial.printf("[Chapter] '%s' not found\n", id); return; }
    currentChapter = ch;
    seekTargetMs   = ch->startMs;
    seekState      = SeekState::FADE_OUT;
    if (ch->onEnterFx >= 0) ramFx.play(ch->onEnterFx);
    applyOverlayDefaults(*ch);
    if (ch->looping) {
        multiPlayer.setLoop(ch->startMs, ch->loopEndMs);
        Serial.printf("[Chapter] -> '%s' LOOP %lu-%lu ms\n", ch->id, ch->startMs, ch->loopEndMs);
    } else {
        multiPlayer.setPlayOnce(ch->startMs, ch->loopEndMs);
        Serial.printf("[Chapter] -> '%s' ONCE %lu-%lu ms\n", ch->id, ch->startMs, ch->loopEndMs);
    }
    leds.setBackgroundFromParams(ch->ledBackground);
    leds.triggerForegroundFromParams(ch->ledEnter);
    leds.setBeatFromParams(ch->ledBeat);
}

void applyOverlayAction(const char* target, OverlayValue val) {
    float* tgt = nullptr;
    if      (strcmp(target, "ov1")  == 0) tgt = &ov1Target;
    else if (strcmp(target, "ov2")  == 0) tgt = &ov2Target;
    else if (strcmp(target, "narr") == 0) tgt = &narrTarget;
    else { Serial.printf("[Overlay] Unknown: %s\n", target); return; }
    switch (val) {
        case OverlayValue::ON:     *tgt = 1.0f; break;
        case OverlayValue::OFF:    *tgt = 0.0f; break;
        case OverlayValue::TOGGLE: *tgt = (*tgt > 0.5f) ? 0.0f : 1.0f; break;
    }
    Serial.printf("[Overlay] %s -> %.0f\n", target, *tgt);
}

void returnToIdle() {
    experienceStarted = false;
    currentChapter    = nullptr;
    // Reset overlay gains
    ov1Target = 0.0f; ov2Target = 0.0f; narrTarget = 0.0f;
    // Stop audio cleanly
    multiPlayer.stop();
    // Return to idle LED
    LedParams idle;
    strlcpy(idle.animation, cfg.idleLed.animation, sizeof(idle.animation));
    idle.color     = cfg.idleLed.color;
    idle.intensity = cfg.idleLed.intensity;
    idle.speed     = cfg.idleLed.speed;
    leds.setBackgroundFromParams(idle);
    // Transducer ring: slow heartbeat in idle
    LedParams idleBeat; strlcpy(idleBeat.animation,"heartbeat",20);
    idleBeat.color=0x220000; idleBeat.intensity=0.2f; idleBeat.speed=0.4f;
    leds.setBeatFromParams(idleBeat);
    Serial.println("[Idle] Experience ended — waiting for tag tap");
}

void startExperience() {
    if (experienceStarted) return;
    experienceStarted = true;
    // Just start the audio — dispatchTag() runs immediately after
    // and calls goToChapter() which sets loop, LED, overlays
    if (!multiPlayer.play(EXPERIENCE_FILE)) {
        Serial.println("[Audio] ERROR: experience.wav not found");
        experienceStarted = false;
        return;
    }
    Serial.println("[Audio] Experience started");
}

void dispatchTag(const TagConfig& tag) {
    Serial.printf("[NFC] '%s' (%s)\n", tag.label, tag.uid);
    for (uint8_t i = 0; i < tag.actionCount; i++) {
        const Action& a = tag.actions[i];
        switch (a.type) {
            case ActionType::CHAPTER: goToChapter(a.target); break;
            case ActionType::OVERLAY: applyOverlayAction(a.target, a.overlayValue); break;
            case ActionType::FX:      if (a.fxSlot >= 0) ramFx.play(a.fxSlot); break;
            case ActionType::LED:      leds.triggerForegroundFromParams(a.ledParams); break;
        }
    }
}

void handleSerialCommand(const char* cmd) {
    if      (strncmp(cmd, "go ",  3) == 0) goToChapter(cmd + 3);
    else if (strncmp(cmd, "seek ",5) == 0) { seekTargetMs=atoi(cmd+5); seekState=SeekState::FADE_OUT; ramFx.play(1); }
    else if (strncmp(cmd, "fx ",  3) == 0) ramFx.play(atoi(cmd + 3));
    else if (strcmp(cmd,  "narr")   == 0)  { narrTarget = (narrTarget > 0.5f) ? 0.0f : 1.0f; }
    else if (strcmp(cmd,  "ov1")    == 0)  { ov1Target  = (ov1Target  > 0.5f) ? 0.0f : 1.0f; }
    else if (strcmp(cmd,  "ov2")    == 0)  { ov2Target  = (ov2Target  > 0.5f) ? 0.0f : 1.0f; }
    else if (strcmp(cmd, "chapters")== 0) {
        for (uint8_t i=0; i<cfg.chapterCount; i++) {
            const ChapterConfig& c = cfg.chapters[i];
            Serial.printf("  [%u] '%s' %lu-%lu ms  fx=%d  %s\n",
                i, c.id, c.startMs, c.loopEndMs, c.onEnterFx,
                c.overlayMode==OverlayMode::KEEP?"keep":"fresh");
        }
    } else if (strcmp(cmd, "tags") == 0) {
        for (uint8_t i=0; i<cfg.tagCount; i++) {
            const TagConfig& t = cfg.tags[i];
            Serial.printf("  [%u] %s '%s' %u actions\n",
                i, t.uid, t.label, t.actionCount);
        }
    } else if (strcmp(cmd, "nfc") == 0) {
        // Debug: print which reader is compiled in and force a poll
#ifdef USE_PN532
        Serial.println("[NFC] Reader: PN532 (USE_PN532 defined)");
#else
        Serial.println("[NFC] Reader: WS1850S / MFRC522 (default)");
#endif
        Serial.println("[NFC] Forcing poll — tap a tag now...");
        char uid[20] = {};
        bool found = false;
        uint32_t t0 = millis();
        while (millis() - t0 < 3000) {   // 3 second window
            if (nfc.poll(uid)) { found = true; break; }
            delay(100);
        }
        if (found) Serial.printf("[NFC] Found: %s\n", uid);
        else       Serial.println("[NFC] No tag detected in 3s");

    } else {
        Serial.println("Commands: go <id> | seek <ms> | fx <n> | narr | ov1 | ov2 | chapters | tags | nfc");
    }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    pinMode(LED_PIN, OUTPUT);
    Serial.begin(115200);
    delay(500);
    Serial.println("=== Interactive Audio Player ===");

    leds.begin();
    AudioMemory(40);
    codec.enable();
    codec.volume(0.7);

    stemMixerL.gain(0,1.0f); stemMixerR.gain(0,1.0f);
    stemMixerL.gain(1,0.0f); stemMixerR.gain(1,0.0f);
    stemMixerL.gain(2,0.0f); stemMixerR.gain(2,0.0f);
    stemMixerL.gain(3,0.0f); stemMixerR.gain(3,0.0f);
    mainMixerL.gain(0,1.0f); mainMixerR.gain(0,1.0f);
    mainMixerL.gain(1,1.0f); mainMixerR.gain(1,1.0f);
    mainMixerL.gain(2,0.0f); mainMixerR.gain(2,0.0f);
    mainMixerL.gain(3,0.0f); mainMixerR.gain(3,0.0f);
    hapticMixer.gain(0, 0.8f);

    if (!SD.sdfs.begin(SdioConfig(FIFO_SDIO))) {
        Serial.println("[SD] mount failed");
        while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(100); }
    }
    Serial.println("[SD] mounted OK");

    ConfigLoader::load(cfg);

    ramFx.load(0, "fx_dev.wav");
    ramFx.load(1, "fx_loc_a.wav");
    ramFx.load(2, "fx_loc_b.wav");
    ramFx.load(3, "fx_boot.wav");
    ramFx.printMemoryUsage();

    // Start in idle — audio begins on first NFC tag tap
    Serial.println("[Idle] Waiting for first tag tap...");
    LedParams idle;
    strlcpy(idle.animation, cfg.idleLed.animation, sizeof(idle.animation));
    idle.color     = cfg.idleLed.color;
    idle.intensity = cfg.idleLed.intensity;
    idle.speed     = cfg.idleLed.speed;
    leds.setBackgroundFromParams(idle);
    // Transducer ring: slow heartbeat in idle
    LedParams idleBeat; strlcpy(idleBeat.animation,"heartbeat",20);
    idleBeat.color=0x220000; idleBeat.intensity=0.2f; idleBeat.speed=0.4f;
    leds.setBeatFromParams(idleBeat);

    Wire.begin();
    Wire.setClock(400000);
    nfc.begin();
    nfc.setSensitivity(cfg.nfcSensitivity);
    // Adaptive poll interval: give LED frames room to breathe between NFC polls
    // sensitivity 0→150ms, 3→180ms, 5→220ms
    _nfcPollInterval = 150 + cfg.nfcSensitivity * 14;
    Serial.printf("[NFC] Poll interval: %ums\n", _nfcPollInterval);

    Serial.println("Type 'chapters' or 'tags' to inspect. Tap NFC tags to navigate.");
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {

    // Drive beat ring from live haptic level — fast attack, slow decay
    if (hapticPeak.available()) {
        float peak = hapticPeak.read();
        leds.setBeatLevel(peak);
    }
    leds.update();

    if (gainRampTimer >= 20) {
        gainRampTimer = 0;

        narrCurrent = stepGain(narrCurrent, narrTarget);
        ov1Current  = stepGain(ov1Current,  ov1Target);
        ov2Current  = stepGain(ov2Current,  ov2Target);
        stemMixerL.gain(1, narrCurrent); stemMixerR.gain(1, narrCurrent);
        stemMixerL.gain(2, ov1Current);  stemMixerR.gain(2, ov1Current);
        stemMixerL.gain(3, ov2Current);  stemMixerR.gain(3, ov2Current);

        switch (seekState) {
            case SeekState::FADE_OUT:
                stemGain -= SEEK_FADE_STEP;
                if (stemGain <= 0.0f) { stemGain = 0.0f; seekState = SeekState::SEEK; }
                mainMixerL.gain(0, stemGain); mainMixerR.gain(0, stemGain);
                break;
            case SeekState::SEEK:
                multiPlayer.seekMs(seekTargetMs);
                seekState = SeekState::FADE_IN;
                break;
            case SeekState::FADE_IN:
                stemGain += SEEK_FADE_STEP;
                if (stemGain >= 1.0f) { stemGain=1.0f; seekState=SeekState::IDLE; }
                mainMixerL.gain(0, stemGain); mainMixerR.gain(0, stemGain);
                break;
            default: break;
        }
    }

    // Detect end of non-looping chapter → return to idle
    if (experienceStarted &&
        currentChapter && !currentChapter->looping &&
        seekState == SeekState::IDLE &&
        !multiPlayer.isPlaying()) {
        returnToIdle();
    }

    if (nfcPollTimer >= _nfcPollInterval) {
        nfcPollTimer = 0;
        char uid[20] = {};
        if (nfc.poll(uid)) {
            int8_t idx = ConfigLoader::findTagIndex(uid, cfg);
            if (idx < 0) {
                Serial.printf("[NFC] Unknown: %s -- add to config.json\n", uid);
            } else {
                const TagConfig& tag = cfg.tags[idx];
                if (tag.cooldownMs > 0 && tagCooldown[idx] < tag.cooldownMs) {
                    uint32_t remaining = tag.cooldownMs - tagCooldown[idx];
                    Serial.printf("[NFC] '%s' cooling down (%lums left)\n",
                                  tag.label, remaining);
                } else {
                    tagCooldown[idx] = 0;  // reset cooldown
                    if (!experienceStarted) startExperience();
                    dispatchTag(tag);
                }
            }
        }
    }

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialPos > 0) {
                serialBuf[serialPos] = '\0';
                serialPos = 0;
                handleSerialCommand(serialBuf);
            }
        } else if (serialPos < sizeof(serialBuf) - 1) {
            serialBuf[serialPos++] = c;
        }
    }

    if (statusTimer >= 3000) {
        statusTimer = 0;
        Serial.printf("[%6lu ms] ch:%s pos:%lu/%lu narr:%.2f ov1:%.2f ov2:%.2f  CPU:%.1f%%  mem:%u/%u\n",
                      millis(),
                      currentChapter ? currentChapter->id : "none",
                      multiPlayer.positionMs(), multiPlayer.lengthMs(),
                      narrCurrent, ov1Current, ov2Current,
                      AudioProcessorUsage(),
                      AudioMemoryUsage(), AudioMemoryUsageMax());
    }
}