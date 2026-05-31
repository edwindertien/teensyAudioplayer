#pragma once

// ============================================================
// LedAnimator — M5 Hex 37-LED SK6812 animations
// Non-blocking, 25fps cap, yields after show() for audio safety
// ============================================================

#include <FastLED.h>

#define LED_PIN          5
#define LED_COUNT        37
#define LED_COLOR_ORDER  GRB
#define LED_FRAME_MS     40    // 25fps max — protects audio ISR

// M5 Hex concentric rings (centre → outer)
static constexpr uint8_t RING_START[] = {  0,  1,  7, 19 };
static constexpr uint8_t RING_SIZE[]  = {  1,  6, 12, 18 };
static constexpr uint8_t RING_COUNT   = 4;

struct LedParams {
    char     animation[20] = "breathe";
    uint32_t color         = 0x0044FF;  // 0xRRGGBB — hint colour
    float    intensity     = 0.7f;      // 0.0–1.0
    float    speed         = 0.4f;      // 0.0–1.0
    uint16_t durationMs    = 0;         // foreground: ms before reverting
};

class LedAnimator {
public:
    void begin() {
        FastLED.addLeds<SK6812, LED_PIN, LED_COLOR_ORDER>(_leds, LED_COUNT);
        FastLED.setBrightness(255);
        fill_solid(_leds, LED_COUNT, CRGB::Black);
        FastLED.show();
        Serial.printf("[LED] %u SK6812 on pin %u\n", LED_COUNT, LED_PIN);
    }

    void setBackground(const char* anim, uint32_t color,
                       float intensity, float speed) {
        strlcpy(_bg.animation, anim, sizeof(_bg.animation));
        _bg.color     = color;
        _bg.intensity = constrain(intensity, 0.0f, 1.0f);
        _bg.speed     = constrain(speed,     0.0f, 1.0f);
        _bg.durationMs = 0;
        // Only reset phase if animation type changes
        if (strcmp(anim, _lastBgAnim) != 0) {
            _bgPhase = 0; _bgStep = 0;
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

    void setBackgroundFromParams(const LedParams& p) {
        setBackground(p.animation, p.color, p.intensity, p.speed);
    }
    void triggerForegroundFromParams(const LedParams& p) {
        triggerForeground(p.animation, p.color, p.intensity,
                          p.speed, p.durationMs);
    }

    void update() {
        if (_masterTimer < LED_FRAME_MS) return;
        _masterTimer = 0;

        if (_fgActive && _fgTimer >= _fg.durationMs) _fgActive = false;

        if (_fgActive) render(_fg, _fgPhase, _fgStep);
        else           render(_bg, _bgPhase, _bgStep);

        FastLED.show();
        yield();  // let audio ISR catch up after show()
    }

private:
    CRGB          _leds[LED_COUNT];
    LedParams     _bg, _fg;
    bool          _fgActive = false;
    elapsedMillis _fgTimer, _masterTimer;
    float         _bgPhase = 0.0f, _fgPhase = 0.0f;
    uint16_t      _bgStep  = 0,    _fgStep  = 0;
    char          _lastBgAnim[20] = {};

    // ── Colour helpers ────────────────────────────────────────
    CRGB col(uint32_t c, float brightness) {
        // brightness 0.0–1.0, never below 5% so LEDs are always perceptible
        float b = 0.05f + brightness * 0.95f;
        b = constrain(b * b, 0.0f, 1.0f); // gamma-ish curve
        return CRGB(
            (uint8_t)(((c >> 16) & 0xFF) * b),
            (uint8_t)(((c >>  8) & 0xFF) * b),
            (uint8_t)(( c        & 0xFF) * b)
        );
    }

    // Phase advance: speed 0→~4s cycle, 0.5→~2s cycle, 1→~1s cycle
    float advance(float speed) {
        return 0.08f + speed * 0.50f;  // radians per frame at 25fps
    }

    // ── Dispatch ─────────────────────────────────────────────
    void render(LedParams& p, float& phase, uint16_t& step) {
        const char* a = p.animation;

        if      (strcmp(a, "off")        == 0) rOff();
        else if (strcmp(a, "solid")      == 0) rSolid(p);
        else if (strcmp(a, "breathe")    == 0) rBreathe(p, phase);
        else if (strcmp(a, "sparkle")    == 0) rSparkle(p, step);
        else if (strcmp(a, "fire")       == 0) rFire(p, step);
        else if (strcmp(a, "ocean")      == 0) rOcean(p, phase);
        else if (strcmp(a, "rainbow")    == 0) rRainbow(p, phase);
        else if (strcmp(a, "pulse_ring") == 0) rPulseRing(p, step);
        else if (strcmp(a, "spin")       == 0) rSpin(p, step);
        else if (strcmp(a, "flash")      == 0) rFlash(p, step);
        else if (strcmp(a, "confetti")   == 0) rConfetti(p, step);
        else if (strcmp(a, "comet")      == 0) rComet(p, step);
        else rSolid(p);

        phase += advance(p.speed);
        if (phase > TWO_PI) phase -= TWO_PI;
        step++;
    }

    // ── Animations ────────────────────────────────────────────

    void rOff() {
        fill_solid(_leds, LED_COUNT, CRGB::Black);
    }

    void rSolid(const LedParams& p) {
        fill_solid(_leds, LED_COUNT, col(p.color, p.intensity));
    }

    void rBreathe(const LedParams& p, float phase) {
        // Sine from 0.25 to 1.0 — always clearly visible, smooth pulse
        float t = (sinf(phase) + 1.0f) * 0.5f;   // 0–1
        float b = 0.25f + t * 0.75f;               // 0.25–1.0
        fill_solid(_leds, LED_COUNT, col(p.color, p.intensity * b));
    }

    void rSparkle(const LedParams& p, uint16_t step) {
        // Base dim fill, random bright sparks
        CRGB base = col(p.color, p.intensity * 0.15f);
        fill_solid(_leds, LED_COUNT, base);
        uint8_t n = max((uint8_t)2, (uint8_t)(p.intensity * 5));
        for (uint8_t i = 0; i < n; i++) {
            uint8_t pos = random8(LED_COUNT);
            // White-tinted sparks for variety
            _leds[pos] = col(0xFFFFFF, p.intensity * (0.5f + random8(128)/255.0f));
        }
    }

    void rFire(const LedParams& p, uint16_t step) {
        // Fire uses its own heat palette — ignores hint colour
        static uint8_t heat[LED_COUNT] = {};
        // Cool down
        for (uint8_t i = 0; i < LED_COUNT; i++)
            heat[i] = qsub8(heat[i], random8(10, 30));
        // Heat outer ring
        for (uint8_t i = 0; i < RING_SIZE[3]; i++)
            heat[RING_START[3]+i] = qadd8(heat[RING_START[3]+i],
                                          random8(100, 200));
        // Spread inward ring by ring
        for (int8_t r = (int8_t)RING_COUNT - 2; r >= 0; r--) {
            for (uint8_t i = 0; i < RING_SIZE[r]; i++) {
                uint8_t j = (i * RING_SIZE[r+1]) / RING_SIZE[r];
                heat[RING_START[r]+i] = qadd8(
                    heat[RING_START[r]+i] / 2,
                    heat[RING_START[r+1] + j % RING_SIZE[r+1]] / 2
                );
            }
        }
        uint8_t bright = (uint8_t)(p.intensity * 255);
        for (uint8_t i = 0; i < LED_COUNT; i++) {
            CRGB c = HeatColor(scale8(heat[i], bright));
            _leds[i] = c;
        }
    }

    void rOcean(const LedParams& p, float phase) {
        // Ocean always uses a blue-cyan palette regardless of hint colour
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            float wave = (sinf(phase + r * 1.1f) + 1.0f) * 0.5f;
            float b    = p.intensity * (0.25f + wave * 0.75f);
            // Deep blue at trough, bright cyan at peak
            uint8_t blue = (uint8_t)(b * 200);
            uint8_t cyan = (uint8_t)(wave * b * 160);
            CRGB c = CRGB(0, cyan, blue);
            for (uint8_t i = 0; i < RING_SIZE[r]; i++)
                _leds[RING_START[r]+i] = c;
        }
    }

    void rRainbow(const LedParams& p, float phase) {
        // Full hue rotation — colour param ignored, intensity respected
        uint8_t hueBase = (uint8_t)(phase * 40.0f);
        uint8_t bright  = (uint8_t)(p.intensity * 230);
        for (uint8_t i = 0; i < LED_COUNT; i++)
            _leds[i] = CHSV(hueBase + i * (256/LED_COUNT), 230, bright);
    }

    void rPulseRing(const LedParams& p, uint16_t step) {
        // Expanding bright ring from centre, fade trail
        for (uint8_t i = 0; i < LED_COUNT; i++) _leds[i].nscale8(160);
        uint8_t ring = (step / 4) % RING_COUNT;
        for (uint8_t r = 0; r <= ring; r++) {
            float b = (float)(r + 1) / (ring + 1);
            CRGB c = col(p.color, p.intensity * b);
            for (uint8_t i = 0; i < RING_SIZE[r]; i++)
                _leds[RING_START[r]+i] += c;
        }
    }

    void rSpin(const LedParams& p, uint16_t step) {
        // Arc sweeping outer ring, fade trail
        for (uint8_t i = 0; i < LED_COUNT; i++) _leds[i].nscale8(170);
        uint8_t sz   = RING_SIZE[3];
        uint8_t head = step % sz;
        for (uint8_t t = 0; t < 6; t++) {
            uint8_t pos = RING_START[3] + (head + sz - t) % sz;
            _leds[pos] = col(p.color, p.intensity * (6-t) / 6.0f);
        }
    }

    void rFlash(const LedParams& p, uint16_t step) {
        // Three quick flashes then dim hold — no blank frames ever
        uint16_t s = step % 24;
        bool    on = (s < 3) || (s >= 6 && s < 9) || (s >= 12 && s < 15);
        float bright = on ? p.intensity : p.intensity * 0.12f;
        fill_solid(_leds, LED_COUNT, col(p.color, bright));
    }

    void rConfetti(const LedParams& p, uint16_t step) {
        // Fade toward dim colour, random hue pops
        for (uint8_t i = 0; i < LED_COUNT; i++) {
            _leds[i].nscale8(200);
            _leds[i] += col(p.color, p.intensity * 0.08f);
        }
        uint8_t n = max((uint8_t)1, (uint8_t)(p.intensity * 4));
        for (uint8_t i = 0; i < n; i++)
            _leds[random8(LED_COUNT)] = CHSV(random8(), 200,
                                             (uint8_t)(p.intensity * 240));
    }

    void rComet(const LedParams& p, uint16_t step) {
        // Bright head races through all pixels, fading tail
        for (uint8_t i = 0; i < LED_COUNT; i++) _leds[i].nscale8(185);
        uint8_t pos = step % LED_COUNT;
        _leds[pos]                            = col(p.color, p.intensity);
        _leds[(pos+LED_COUNT-1) % LED_COUNT]  = col(p.color, p.intensity*0.5f);
        _leds[(pos+LED_COUNT-2) % LED_COUNT]  = col(p.color, p.intensity*0.2f);
    }
};