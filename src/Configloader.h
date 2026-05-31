#pragma once

// ============================================================
// ConfigLoader
// Parses /config.json from SD card into typed structs.
// Audio designers edit only the JSON — no reflashing needed.
//
// Call ConfigLoader::load(config) once after SD.begin().
// ============================================================

#include <SD.h>
#include <ArduinoJson.h>

// ── Limits ───────────────────────────────────────────────────
static constexpr uint8_t MAX_CHAPTERS  = 16;
static constexpr uint8_t MAX_TAGS      = 32;
static constexpr uint8_t MAX_ACTIONS   = 6;

// ── Action ───────────────────────────────────────────────────
enum class ActionType : uint8_t {
    CHAPTER,    // seek to chapter, apply its settings
    OVERLAY,    // set ov1 / ov2 / narr to on/off/toggle
    FX,         // play a RAM FX slot
};

enum class OverlayValue : uint8_t { ON, OFF, TOGGLE };

struct Action {
    ActionType   type;
    char         target[16];    // chapter id  OR  "ov1"/"ov2"/"narr"
    OverlayValue overlayValue;  // for OVERLAY actions
    int8_t       fxSlot;        // for FX actions (-1 = none)
};

// ── Overlay defaults for a chapter ───────────────────────────
enum class OverlayMode : uint8_t {
    FRESH,   // apply chapter's overlay defaults on entry
    KEEP,    // keep whatever overlays are currently active
};

struct OverlayDefaults {
    bool ov1  = false;
    bool ov2  = false;
    bool narr = false;
};

// ── Chapter ───────────────────────────────────────────────────
struct ChapterConfig {
    char            id[16]      = {};
    char            label[32]   = {};
    uint32_t        startMs     = 0;
    uint32_t        loopEndMs   = 0;    // 0 = end of file
    int8_t          onEnterFx   = -1;   // RAM FX slot, -1 = none
    OverlayMode     overlayMode = OverlayMode::FRESH;
    OverlayDefaults overlays;
};

// ── Tag ──────────────────────────────────────────────────────
enum class TagType : uint8_t { LOCATION, DEVICE };

struct TagConfig {
    char        uid[20]     = {};    // uppercase hex, e.g. "04A32B1C"
    char        label[48]   = {};    // human-readable, for debug only
    TagType     type        = TagType::LOCATION;
    Action      actions[MAX_ACTIONS];
    uint8_t     actionCount = 0;
};

// ── Global config ─────────────────────────────────────────────
struct ExperienceConfig {
    char          deviceId[16]      = "UNIT_01";
    char          startChapter[16]  = "intro";
    ChapterConfig chapters[MAX_CHAPTERS];
    uint8_t       chapterCount      = 0;
    TagConfig     tags[MAX_TAGS];
    uint8_t       tagCount          = 0;
};

// ── Loader ────────────────────────────────────────────────────
class ConfigLoader {
public:
    static bool load(ExperienceConfig& cfg, const char* path = "/config.json") {
        File f = SD.open(path);
        if (!f) {
            Serial.printf("[Config] ERROR: cannot open %s\n", path);
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            Serial.printf("[Config] JSON error: %s\n", err.c_str());
            return false;
        }

        strlcpy(cfg.deviceId,      doc["device_id"]     | "UNIT_01", sizeof(cfg.deviceId));
        strlcpy(cfg.startChapter,  doc["start_chapter"] | "intro",   sizeof(cfg.startChapter));

        // ── Chapters ─────────────────────────────────────────
        cfg.chapterCount = 0;
        for (JsonObject ch : doc["chapters"].as<JsonArray>()) {
            if (cfg.chapterCount >= MAX_CHAPTERS) break;
            ChapterConfig& c = cfg.chapters[cfg.chapterCount++];

            strlcpy(c.id,    ch["id"]    | "unnamed", sizeof(c.id));
            strlcpy(c.label, ch["label"] | "",        sizeof(c.label));
            c.startMs    = ch["start_ms"]   | 0;
            c.loopEndMs  = ch["loop_end_ms"]| 0;
            c.onEnterFx  = ch["on_enter_fx"]| -1;

            const char* mode = ch["overlay_mode"] | "fresh";
            c.overlayMode = (strcmp(mode, "keep") == 0)
                            ? OverlayMode::KEEP
                            : OverlayMode::FRESH;

            JsonObject ov = ch["overlays"];
            if (!ov.isNull()) {
                c.overlays.ov1  = ov["ov1"]  | false;
                c.overlays.ov2  = ov["ov2"]  | false;
                c.overlays.narr = ov["narr"] | false;
            }
        }

        // ── Tags ─────────────────────────────────────────────
        cfg.tagCount = 0;
        for (JsonObject tag : doc["tags"].as<JsonArray>()) {
            if (cfg.tagCount >= MAX_TAGS) break;
            TagConfig& t = cfg.tags[cfg.tagCount++];

            strlcpy(t.uid,   tag["uid"]   | "", sizeof(t.uid));
            strlcpy(t.label, tag["label"] | "", sizeof(t.label));

            const char* type = tag["type"] | "location";
            t.type = (strcmp(type, "device") == 0)
                     ? TagType::DEVICE
                     : TagType::LOCATION;

            t.actionCount = 0;
            for (JsonObject act : tag["actions"].as<JsonArray>()) {
                if (t.actionCount >= MAX_ACTIONS) break;
                Action& a = t.actions[t.actionCount++];

                const char* doStr = act["do"] | "";
                if (strcmp(doStr, "chapter") == 0) {
                    a.type = ActionType::CHAPTER;
                    strlcpy(a.target, act["target"] | "", sizeof(a.target));

                } else if (strcmp(doStr, "overlay") == 0) {
                    a.type = ActionType::OVERLAY;
                    strlcpy(a.target, act["target"] | "", sizeof(a.target));
                    const char* val = act["value"] | "toggle";
                    if      (strcmp(val, "on")  == 0) a.overlayValue = OverlayValue::ON;
                    else if (strcmp(val, "off") == 0) a.overlayValue = OverlayValue::OFF;
                    else                              a.overlayValue = OverlayValue::TOGGLE;

                } else if (strcmp(doStr, "fx") == 0) {
                    a.type   = ActionType::FX;
                    a.fxSlot = act["slot"] | -1;
                }
            }
        }

        Serial.printf("[Config] Loaded: %u chapters, %u tags, device=%s\n",
                      cfg.chapterCount, cfg.tagCount, cfg.deviceId);
        return true;
    }

    // Find a chapter by id
    static const ChapterConfig* findChapter(const char* id,
                                            const ExperienceConfig& cfg) {
        for (uint8_t i = 0; i < cfg.chapterCount; i++) {
            if (strcmp(cfg.chapters[i].id, id) == 0) return &cfg.chapters[i];
        }
        return nullptr;
    }

    // Find a tag by UID string
    static const TagConfig* findTag(const char* uid,
                                    const ExperienceConfig& cfg) {
        for (uint8_t i = 0; i < cfg.tagCount; i++) {
            if (strcmp(cfg.tags[i].uid, uid) == 0) return &cfg.tags[i];
        }
        return nullptr;
    }
};