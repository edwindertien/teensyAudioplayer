#pragma once

// ============================================================
// LedAnimator — two-ring LED system
//
//  Ring 1: NFC reader ring     — 24 LEDs, indices  0-23
//  Ring 2: Transducer ring     — 16 LEDs, indices 24-39
//  Total: 40 LEDs, single data chain on pin 5
//
//  Wiring (series chain):
//    Teensy pin 5 → [33Ω] → NFC ring DIN
//    NFC ring DOUT → [33Ω] → Transducer ring DIN
//    Both rings share 5V power and GND
//
//  Two independent animation layers — one FastLED.show() per frame:
//    setBackground() / triggerForeground()  → NFC ring (indices 0-23)
//    setBeat()                              → Transducer ring (24-39)
// ============================================================

#include <FastLED.h>
#include <SD.h>
#include <ArduinoJson.h>

#define LED_NFC_PIN      5    // NFC reader ring — 24 LEDs
#define LED_BEAT_PIN     14   // Transducer ring  — 16 LEDs
#define LED_COLOR_ORDER  GRB

#define LED_NFC_COUNT    24
#define LED_BEAT_COUNT   16

#define LED_FRAME_MS     40    // 40ms = 25fps

// ── Custom frame animations (loaded from led_animations.json) ─
#define CUSTOM_ANIM_MAX    12   // max number of custom animations
#define CUSTOM_FRAME_MAX   64   // max frames per animation
#define CUSTOM_LED_MAX     40   // max LEDs per frame (NFC + beat combined)

struct LedAnimFrame {
    uint16_t ms = 100;
    uint8_t  r[CUSTOM_LED_MAX] = {};
    uint8_t  g[CUSTOM_LED_MAX] = {};
    uint8_t  b[CUSTOM_LED_MAX] = {};
};

struct CustomLedAnim {
    char          id[24]   = {};
    uint8_t       ring     = 0;     // 0=nfc 1=beat 2=both
    bool          loop     = true;
    uint8_t       frameCount = 0;
    LedAnimFrame  frames[CUSTOM_FRAME_MAX];
};

struct LedParams {
    char     animation[20] = "breathe";
    uint32_t color         = 0x0044FF;
    float    intensity     = 0.7f;
    float    speed         = 0.4f;
    uint16_t durationMs    = 0;
};

class LedAnimator {
public:
    void begin(uint8_t brightness = 160) {
        FastLED.addLeds<SK6812, LED_NFC_PIN,  LED_COLOR_ORDER>(_nfcLeds,  LED_NFC_COUNT);
        FastLED.addLeds<SK6812, LED_BEAT_PIN, LED_COLOR_ORDER>(_beatLeds, LED_BEAT_COUNT);
        FastLED.setBrightness(brightness);
        Serial.printf("[LED] NFC ring: %u LEDs pin %u  |  Beat ring: %u LEDs pin %u  brightness:%u\n",
                      LED_NFC_COUNT, LED_NFC_PIN, LED_BEAT_COUNT, LED_BEAT_PIN, brightness);

        // Hardware test: flash red on both rings for 500ms then go dark
        fill_solid(_nfcLeds,  LED_NFC_COUNT,  CRGB::Red);
        fill_solid(_beatLeds, LED_BEAT_COUNT, CRGB::Red);
        FastLED.show();
        Serial.println("[LED] Hardware test: RED — if no LEDs lit, check wiring/pin numbers");
        delay(500);
        fill_solid(_nfcLeds,  LED_NFC_COUNT,  CRGB::Black);
        fill_solid(_beatLeds, LED_BEAT_COUNT, CRGB::Black);
        FastLED.show();
        Serial.println("[LED] Hardware test done — starting animations");
    }

    // ── NFC ring (foreground/background model) ────────────────

    void setBackground(const char* anim, uint32_t color,
                       float intensity, float speed) {
        strlcpy(_bg.animation, anim, sizeof(_bg.animation));
        _bg.color     = color;
        _bg.intensity = constrain(intensity, 0.0f, 1.0f);
        _bg.speed     = constrain(speed,     0.0f, 1.0f);
        if (strcmp(anim, _lastBgAnim) != 0) {
            _bgPhase = 0; _bgStep = 0;
            _nfcCustomIdx = -1; _nfcFrameIdx = 0; // reset custom anim lookup
            strlcpy(_lastBgAnim, anim, sizeof(_lastBgAnim));
        }
    }

    void triggerForeground(const char* anim, uint32_t color,
                           float intensity, float speed, uint16_t durationMs) {
        if (durationMs == 0) return;
        strlcpy(_fg.animation, anim, sizeof(_fg.animation));
        _fg.color      = color;
        _fg.intensity  = constrain(intensity, 0.0f, 1.0f);
        _fg.speed      = constrain(speed,     0.0f, 1.0f);
        _fg.durationMs = durationMs;
        _fgActive      = true;
        _fgTimer       = 0;
        _fgPhase       = 0;
        _fgStep        = 0;
    }

    // Load custom frame animations from led_animations.json on SD card
    bool loadAnimations(const char* path = "/led_animations.json") {
        File f = SD.open(path);
        if (!f) {
            Serial.printf("[LED] No %s on SD — custom animations unavailable\n", path);
            return false;
        }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            Serial.printf("[LED] led_animations.json parse error: %s\n", err.c_str());
            return false;
        }
        _customCount = 0;
        for (JsonObject a : doc["animations"].as<JsonArray>()) {
            if (_customCount >= CUSTOM_ANIM_MAX) break;
            CustomLedAnim& ca = _custom[_customCount];
            strlcpy(ca.id, a["id"] | "", sizeof(ca.id));
            const char* ring = a["ring"] | "nfc";
            ca.ring = strcmp(ring,"beat")==0 ? 1 : strcmp(ring,"both")==0 ? 2 : 0;
            ca.loop = a["loop"] | true;
            ca.frameCount = 0;
            for (JsonObject fr : a["frames"].as<JsonArray>()) {
                if (ca.frameCount >= CUSTOM_FRAME_MAX) break;
                LedAnimFrame& lf = ca.frames[ca.frameCount];
                lf.ms = fr["ms"] | 100;
                uint8_t pi = 0;
                for (const char* px : fr["pixels"].as<JsonArray>()) {
                    if (pi >= CUSTOM_LED_MAX) break;
                    // Parse "#RRGGBB"
                    uint32_t v = strtoul(px+1, nullptr, 16);
                    lf.r[pi] = (v>>16)&0xFF;
                    lf.g[pi] = (v>>8) &0xFF;
                    lf.b[pi] = (v)    &0xFF;
                    pi++;
                }
                ca.frameCount++;
            }
            Serial.printf("[LED] Custom anim '%s': %u frames, ring=%s\n",
                          ca.id, ca.frameCount, ring);
            _customCount++;
        }
        Serial.printf("[LED] Loaded %u custom animations\n", _customCount);
        return true;
    }

    // Find a custom animation by id, returns index or -1
    int8_t findCustomAnim(const char* id) {
        for (uint8_t i = 0; i < _customCount; i++)
            if (strcmp(_custom[i].id, id) == 0) return (int8_t)i;
        return -1;
    }

    // Global brightness scale (0-255). Applied immediately.
    void setGlobalBrightness(uint8_t b) {
        FastLED.setBrightness(b);
    }

    void setBackgroundFromParams(const LedParams& p) {
        setBackground(p.animation, p.color, p.intensity, p.speed);
    }
    void triggerForegroundFromParams(const LedParams& p) {
        triggerForeground(p.animation, p.color, p.intensity,
                          p.speed, p.durationMs);
    }

    // ── Transducer ring (beat animation) ─────────────────────
    // speed maps loosely to BPM: 0.0=~30bpm 0.5=~60bpm 1.0=~120bpm

    void setBeat(const char* anim, uint32_t color,
                 float intensity, float speed) {
        strlcpy(_beat.animation, anim, sizeof(_beat.animation));
        _beat.color     = color;
        _beat.intensity = constrain(intensity, 0.0f, 1.0f);
        _beat.speed     = constrain(speed,     0.0f, 1.0f);
    }

    void setBeatFromParams(const LedParams& p) {
        setBeat(p.animation, p.color, p.intensity, p.speed);
    }

    // Called from loop() with live haptic peak value (0.0-1.0)
    void setBeatLevel(float peak) {
        _beatLevel = peak;
    }

    // ── Main update ───────────────────────────────────────────

    void update() {
        if (_masterTimer < LED_FRAME_MS) return;
        _masterTimer = 0;

        // Expire foreground
        if (_fgActive && _fgTimer >= _fg.durationMs) _fgActive = false;

        // Render NFC ring (indices 0–23)
        if (_fgActive) renderNfc(_fg, _fgPhase, _fgStep);
        else           renderNfc(_bg, _bgPhase, _bgStep);

        // Render transducer beat ring (indices 24–39)
        renderBeat();

        FastLED.show();
        // No yield() — FastLED on Teensy 4.x uses FlexIO DMA (non-blocking)
    }

private:
    CRGB          _nfcLeds[LED_NFC_COUNT];
    CRGB          _beatLeds[LED_BEAT_COUNT];

    // NFC ring layers
    LedParams     _bg, _fg;
    bool          _fgActive = false;
    elapsedMillis _fgTimer;
    elapsedMillis _masterTimer;
    float         _bgPhase = 0.0f, _fgPhase = 0.0f;
    uint16_t      _bgStep  = 0,    _fgStep  = 0;
    char          _lastBgAnim[20] = {};

    // Custom frame animations
    CustomLedAnim _custom[CUSTOM_ANIM_MAX];
    uint8_t       _customCount = 0;
    // Playback state for NFC ring custom anim
    int8_t        _nfcCustomIdx   = -1;
    uint8_t       _nfcFrameIdx    = 0;
    elapsedMillis _nfcFrameTimer;
    // Playback state for beat ring custom anim
    int8_t        _beatCustomIdx  = -1;
    uint8_t       _beatFrameIdx   = 0;
    elapsedMillis _beatFrameTimer;

    // Beat ring
    LedParams     _beat = {"heartbeat", 0xFF2200, 0.7f, 0.5f};
    float         _beatPhase = 0.0f;
    uint16_t      _beatStep  = 0;
    float         _beatLevel = 0.0f;     // live haptic level (0.0-1.0)
    float         _beatEnv   = 0.0f;     // envelope follower output

    // ── Colour helper ─────────────────────────────────────────
    CRGB col(uint32_t c, float brightness) {
        float b = constrain(0.05f + brightness * 0.95f, 0.0f, 1.0f);
        b = b * b;
        return CRGB(
            (uint8_t)(((c >> 16) & 0xFF) * b),
            (uint8_t)(((c >>  8) & 0xFF) * b),
            (uint8_t)(( c        & 0xFF) * b)
        );
    }

    float phaseStep(float speed) { return 0.08f + speed * 0.50f; }

    // ── NFC ring render — writes to indices 0–23 ──────────────
    void renderNfc(LedParams& p, float& phase, uint16_t& step) {
        CRGB* ring = _nfcLeds;
        uint8_t n  = LED_NFC_COUNT;
        const char* a = p.animation;

        // Check for custom frame animation first
        if (_nfcCustomIdx < 0) _nfcCustomIdx = findCustomAnim(a);
        if (_nfcCustomIdx >= 0) { renderCustomNfc(); return; }

        if      (strcmp(a, "off")        == 0) fill_solid(ring, n, CRGB::Black);
        else if (strcmp(a, "solid")      == 0) fill_solid(ring, n, col(p.color, p.intensity));
        else if (strcmp(a, "breathe")    == 0) nfcBreathe(ring, n, p, phase);
        else if (strcmp(a, "sparkle")    == 0) nfcSparkle(ring, n, p, step);
        else if (strcmp(a, "fire")       == 0) nfcFire(ring, n, p, step);
        else if (strcmp(a, "ocean")      == 0) nfcOcean(ring, n, p, phase);
        else if (strcmp(a, "rainbow")    == 0) nfcRainbow(ring, n, p, phase);
        else if (strcmp(a, "spin")       == 0) nfcSpin(ring, n, p, step);
        else if (strcmp(a, "flash")      == 0) nfcFlash(ring, n, p, step);
        else if (strcmp(a, "pulse_ring") == 0) nfcPulse(ring, n, p, step);
        else if (strcmp(a, "confetti")   == 0) nfcConfetti(ring, n, p, step);
        else if (strcmp(a, "comet")      == 0) nfcComet(ring, n, p, step);
        else fill_solid(ring, n, col(p.color, p.intensity));

        phase += phaseStep(p.speed);
        if (phase > TWO_PI) phase -= TWO_PI;
        step++;
    }

    // ── Transducer ring render — writes to indices 24–39 ─────
    void renderBeat() {
        CRGB* ring = _beatLeds;
        uint8_t n  = LED_BEAT_COUNT;
        const char* a = _beat.animation;

        // Check for custom frame animation first
        if (_beatCustomIdx < 0) _beatCustomIdx = findCustomAnim(a);
        if (_beatCustomIdx >= 0) { renderCustomBeat(); return; }

        if      (strcmp(a, "level")      == 0) beatLevel(ring, n);
        else if (strcmp(a, "heartbeat")  == 0) beatHeartbeat(ring, n);
        else if (strcmp(a, "breathe")    == 0) beatBreathe(ring, n);
        else if (strcmp(a, "spin")       == 0) beatSpin(ring, n);
        else if (strcmp(a, "off")        == 0) fill_solid(ring, n, CRGB::Black);
        else    beatLevel(ring, n);   // default to live level

        _beatPhase += phaseStep(_beat.speed);
        if (_beatPhase > TWO_PI) _beatPhase -= TWO_PI;
        _beatStep++;
    }

    // ── NFC ring animations ───────────────────────────────────

    void nfcBreathe(CRGB* r, uint8_t n, const LedParams& p, float ph) {
        float t = 0.25f + ((sinf(ph) + 1.0f) * 0.5f) * 0.75f;
        fill_solid(r, n, col(p.color, p.intensity * t));
    }

    void nfcSparkle(CRGB* r, uint8_t n, const LedParams& p, uint16_t s) {
        fill_solid(r, n, col(p.color, p.intensity * 0.12f));
        uint8_t cnt = max((uint8_t)2, (uint8_t)(p.intensity * 5));
        for (uint8_t i = 0; i < cnt; i++)
            r[random8(n)] = col(0xFFFFFF, p.intensity * (0.5f + random8(128)/255.0f));
    }

    void nfcFire(CRGB* r, uint8_t n, const LedParams& p, uint16_t s) {
        static uint8_t heat[LED_NFC_COUNT] = {};
        // Cool every cell
        for (uint8_t i = 0; i < n; i++)
            heat[i] = qsub8(heat[i], random8(8, 22));
        // Spread heat around the ring (wrapping)
        for (uint8_t i = 0; i < n; i++) {
            uint8_t prev = heat[(i + n - 1) % n];
            uint8_t next = heat[(i + 1) % n];
            heat[i] = (heat[i] + prev + next) / 3;
        }
        // Randomly inject heat at 2-3 points around the ring
        uint8_t hotspots = 2 + (uint8_t)(p.intensity * 2);
        for (uint8_t h = 0; h < hotspots; h++) {
            uint8_t pos = random8(n);
            heat[pos]            = qadd8(heat[pos],            random8(120, 200));
            heat[(pos+1) % n]    = qadd8(heat[(pos+1) % n],   random8(60, 120));
            heat[(pos+n-1) % n]  = qadd8(heat[(pos+n-1) % n], random8(60, 120));
        }
        uint8_t bright = (uint8_t)(p.intensity * 255);
        for (uint8_t i = 0; i < n; i++)
            r[i] = HeatColor(scale8(heat[i], bright));
    }

    void nfcOcean(CRGB* r, uint8_t n, const LedParams& p, float ph) {
        for (uint8_t i = 0; i < n; i++) {
            float wave = (sinf(ph + i * TWO_PI / n) + 1.0f) * 0.5f;
            float b    = p.intensity * (0.25f + wave * 0.75f);
            r[i] = CRGB(0, (uint8_t)(wave * b * 160), (uint8_t)(b * 200));
        }
    }

    void nfcRainbow(CRGB* r, uint8_t n, const LedParams& p, float ph) {
        uint8_t hue = (uint8_t)(ph * 40.0f);
        uint8_t brt = (uint8_t)(p.intensity * 230);
        for (uint8_t i = 0; i < n; i++)
            r[i] = CHSV(hue + i * (256/n), 220, brt);
    }

    void nfcSpin(CRGB* r, uint8_t n, const LedParams& p, uint16_t s) {
        for (uint8_t i = 0; i < n; i++) r[i].nscale8(170);
        uint8_t head = s % n;
        for (uint8_t t = 0; t < 6; t++)
            r[(head + n - t) % n] = col(p.color, p.intensity * (6-t)/6.0f);
    }

    void nfcFlash(CRGB* r, uint8_t n, const LedParams& p, uint16_t s) {
        uint16_t f = s % 24;
        bool on = (f < 3) || (f >= 6 && f < 9) || (f >= 12 && f < 15);
        fill_solid(r, n, col(p.color, p.intensity * (on ? 1.0f : 0.08f)));
    }

    void nfcPulse(CRGB* r, uint8_t n, const LedParams& p, uint16_t s) {
        for (uint8_t i = 0; i < n; i++) r[i].nscale8(160);
        // Expand bright arc around ring
        uint8_t arc = (s * 2) % n;
        for (uint8_t i = 0; i < arc; i++) {
            float b = (float)(i + 1) / (arc + 1);
            r[i] += col(p.color, p.intensity * b);
        }
    }

    void nfcConfetti(CRGB* r, uint8_t n, const LedParams& p, uint16_t s) {
        for (uint8_t i = 0; i < n; i++) {
            r[i].nscale8(200);
            r[i] += col(p.color, p.intensity * 0.08f);
        }
        uint8_t cnt = max((uint8_t)1, (uint8_t)(p.intensity * 3));
        for (uint8_t i = 0; i < cnt; i++)
            r[random8(n)] = CHSV(random8(), 200, (uint8_t)(p.intensity*230));
    }

    void nfcComet(CRGB* r, uint8_t n, const LedParams& p, uint16_t s) {
        for (uint8_t i = 0; i < n; i++) r[i].nscale8(185);
        uint8_t pos = s % n;
        r[pos]              = col(p.color, p.intensity);
        r[(pos+n-1) % n]   = col(p.color, p.intensity * 0.5f);
        r[(pos+n-2) % n]   = col(p.color, p.intensity * 0.2f);
    }

    // ── Transducer ring animations ────────────────────────────

    // Live level follower with non-linear (exponential saturation) response.
    // Soft pulses are lifted, loud pulses saturate near max — compression-like.
    //
    // speed in config controls sensitivity/compression:
    //   0.0 = gentle (follows dynamics), 0.5 = medium, 1.0 = aggressive (all pulses equal)
    // Formula: output = 1 - exp(-gain * envelope)
    //   gain = 2 + speed * 12  → speed 0.0→gain 2, speed 0.5→gain 8, speed 1.0→gain 14
    //
    // Example at gain=8 (speed=0.5):
    //   input 0.05 (whisper) → output 0.33   (clearly visible)
    //   input 0.20 (soft)    → output 0.80   (bright)
    //   input 0.50 (medium)  → output 0.98   (near full)
    //   input 1.00 (peak)    → output 1.00   (full)
    void beatLevel(CRGB* r, uint8_t n) {
        // Envelope: instant attack, ~300ms decay at 25fps
        if (_beatLevel > _beatEnv) {
            _beatEnv = _beatLevel;
        } else {
            _beatEnv *= 0.88f;
        }

        // Exponential saturation — lifts soft pulses, normalises loud ones
        float gain       = 2.0f + _beat.speed * 12.0f;
        float compressed = 1.0f - expf(-gain * _beatEnv);

        // Always keep a faint idle glow so the ring is never fully dark
        float b = max(compressed * _beat.intensity, 0.04f);
        fill_solid(r, n, col(_beat.color, b));
    }

    // Cardiac-style pulse: quick bright flash, then slow decay
    // speed controls BPM: 0.0≈30bpm, 0.5≈60bpm, 1.0≈120bpm
    void beatHeartbeat(CRGB* r, uint8_t n) {
        // Use beatPhase: full cycle = one heartbeat
        // Shape: fast rise, double peak (lub-dub), slow decay to dim
        float t  = (_beatPhase / TWO_PI);         // 0.0–1.0 through cycle
        float brightness;

        if      (t < 0.06f) brightness = t / 0.06f;              // rise: lub
        else if (t < 0.14f) brightness = 1.0f - (t-0.06f)/0.08f * 0.4f; // fall
        else if (t < 0.20f) brightness = 0.6f + (t-0.14f)/0.06f * 0.4f; // rise: dub
        else if (t < 0.30f) brightness = 1.0f - (t-0.20f)/0.10f * 0.8f; // fall
        else                brightness = 0.2f * (1.0f - (t-0.30f)/0.70f); // decay

        brightness = max(brightness, 0.05f);   // always faintly visible
        fill_solid(r, n, col(_beat.color, _beat.intensity * brightness));
    }

    // Simple breathe for when no haptic content is playing
    void beatBreathe(CRGB* r, uint8_t n) {
        float t = 0.2f + ((sinf(_beatPhase) + 1.0f) * 0.5f) * 0.8f;
        fill_solid(r, n, col(_beat.color, _beat.intensity * t));
    }

    // Spinning arc on the transducer ring
    void beatSpin(CRGB* r, uint8_t n) {
        for (uint8_t i = 0; i < n; i++) r[i].nscale8(170);
        uint8_t head = _beatStep % n;
        for (uint8_t t = 0; t < 4; t++)
            r[(head + n - t) % n] = col(_beat.color,
                                        _beat.intensity * (4-t)/4.0f);
    }
};