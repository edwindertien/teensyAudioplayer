# Engineering Context & Lessons Learned

This document captures what didn't work, why, and what was chosen instead.
Useful for future development, porting, or debugging.

---

## Audio Architecture

### ❌ Multiple concurrent AudioPlaySdWav readers

**What we tried:** Running 3 separate `AudioPlaySdWav` objects simultaneously
(base + 2 overlays) to mix independent stems from separate WAV files.

**What happened:** CPU spiked from ~1% to ~30% per additional player. Adding one
overlay jumped CPU to 30%, two overlays to ~37%. The Teensy's `AudioProcessorUsage()`
measures time spent *inside the audio ISR including SD wait states*. When two players
read concurrently, the SDIO bus serialises the reads and the wait time shows as CPU.
With enough players this caused audio glitches and eventual crashes.

**Root cause:** The SDIO driver on Teensy 4.1 at the time of development (Teensyduino
1.60) does not support concurrent efficient reads. Each `AudioPlaySdWav::update()` call
blocks until its SD read completes.

**Solution:** Single 7.1-channel interleaved WAV file (`AudioPlaySdWavMulti`). One SD
read per ISR tick feeds all channels simultaneously. CPU dropped to ~4% for 8 channels.

---

### ❌ AUDIO_BLOCK_SAMPLES=256

**What we tried:** Doubling the block size to halve SD read frequency.

**What happened:** Audio quality became severely degraded — bit-crushed, almost
unrecognisable. The SGTL5000 codec (and the I2S driver) is calibrated for 128-sample
blocks. Non-standard values break internal timing assumptions.

**Solution:** Keep `AUDIO_BLOCK_SAMPLES=128`. This is not configurable in practice.

---

### ❌ AudioOutputAnalog / DAC on Teensy 4.1

**What we tried:** `AudioOutputAnalog` for haptic transducer output, expecting a true DAC.

**What happened:** Teensy 4.1 has no hardware DAC (unlike Teensy 3.2). `AudioOutputAnalog`
on Teensy 4.x uses PWM, not a DAC. `AudioOutputPWM` is the correct class.

**Solution:** `AudioOutputPWM` on pins 3 (MSB) + 4 (LSB) with a resistor network
(1kΩ + 270kΩ) and RC low-pass filter. Only pin 4 (LSB) reliably produced output
in practice — pin 3 (MSB) did not fire its DMA. The LSB-only output sounds
"bit-crushed" through speakers but is perfectly acceptable for a haptic transducer
whose frequency response is limited to <300Hz.

**Note:** `AudioOutputPWM::begin(pin1, pin2)` exists but is declared `private` — it
cannot be called from user code. The default `begin()` hardwires pins 3+4.

---

### ❌ MQS (Medium Quality Sound) output on Teensy 4.1

**What we tried:** `AudioOutputMQS` as an alternative PWM output for haptic.

**What happened:** MQS uses fixed pins 10 (MQSR) and 12 (MQSL). These are the default
SPI CS and MISO pins — both used by the Audio Shield. No pin rerouting is possible
for MQS on Teensy 4.1.

**Solution:** `AudioOutputPWM` on pins 3+4 (different peripheral, no conflict).

---

### ❌ seekMs() called from main loop while audio ISR runs

**What we tried:** Calling `multiPlayer.seekMs()` directly from `loop()` inside the
seek crossfade state machine.

**What happened:** After several NFC tag interactions, the 7.1 player would silently
stop. `pos:0/96000`, `CPU:0.0%`, `mem:0` in serial output. The player's `_playing`
flag was being set to false. Subsequent `ramFx.play()` still worked (RAM-based, no SD).

**Root cause:** Race condition. `seekMs()` calls `_file.seek()` from `loop()`. Simultaneously,
`AudioPlaySdWavMulti::update()` runs in the audio ISR and also accesses `_file`. Concurrent
access to the SD file handle corrupted internal state.

**Solution:** Deferred seek. `seekMs()` sets `_seekPending = true` and `_seekTargetMs`.
The actual `_file.seek()` only ever happens inside `update()` which runs in the audio ISR —
single-threaded, no race possible. `volatile` on the flag ensures the compiler doesn't
optimise across the ISR boundary.

---

### ❌ isPlaying() restart loop (thousands of calls per millisecond)

**What we tried:**
```cpp
void loop() {
    if (!player.isPlaying()) player.play(FILE);
}
```

**What happened:** `isPlaying()` returns false for several milliseconds after a file
ends (while the last buffer drains). `loop()` runs thousands of times per millisecond,
calling `player.play()` thousands of times before the player starts. This corrupted
the player's internal state and caused the SD file handle to be opened repeatedly.
Audible as a loud click/crunch on every loop restart.

**Solution:** Edge detection — track previous state:
```cpp
bool wasPlaying = false;
void loop() {
    bool nowPlaying = player.isPlaying();
    if (wasPlaying && !nowPlaying) player.play(FILE);  // falling edge only
    wasPlaying = nowPlaying;
}
```

---

### ❌ AudioPlayMemory header format

**What we tried:** Encoding `numBlocks` in the header word passed to `AudioPlayMemory`.

**What happened:** Files played back as a very brief click (~1ms) regardless of length.

**Root cause:** `AudioPlayMemory::update()` decrements `length` by `AUDIO_BLOCK_SAMPLES`
(128) each ISR tick and stops when `length < 128`. `length` is therefore measured in
**samples**, not blocks. With `numBlocks = 46` stored as length, the player stopped
after the very first update call (46 < 128).

Additionally, the 32-bit header word layout is:
- Bits 31–24: type flag (0x81 = 16-bit PCM 44100Hz)
- Bits 23–0: **number of samples**

Two `uint16_t` words must be written in little-endian order.

**Solution:** `header = (0x81 << 24) | (numSamples & 0x00FFFFFF)`.

---

## SD Card

### ❌ macOS hidden files on FAT32

**What we tried:** Dragging files onto the SD card in Finder.

**What happened:** macOS creates `._filename` resource fork files alongside every copied
file, plus `.DS_Store`, `.Spotlight-V100`, `.fseventsd`. These populate the FAT directory
table, slowing lookups and potentially confusing the Teensy's SdFat library.

**Solution:** Always run `dot_clean /Volumes/SDCARD/` after copying, or use `cp` from
terminal instead of Finder drag-and-drop.

### ❌ Slow SD card (Class 4)

**What we tried:** Generic Class 4 SD card.

**What happened:** Even single-player audio had occasional glitches — irregular vinyl-like
clicks. The Class 4 rating (4 MB/s minimum) is insufficient for reliable real-time audio
streaming, especially with directory lookups happening for file opens.

**Solution:** SanDisk Ultra or Extreme (A1/A2 rated UHS-I). A1/A2 cards are optimised
for random read IOPS, not just sequential throughput.

---

## Audio Format

### ❌ WAVE_FORMAT_EXTENSIBLE (format type 65534) rejected

**What we tried:** Checking `audioFmt == 1` (plain PCM) in the WAV header parser.

**What happened:** ffmpeg writes multi-channel WAV files with format type `65534`
(WAVE_FORMAT_EXTENSIBLE), not `1`. The parser rejected all multi-channel files.

**Solution:** Accept both `1` (plain PCM) and `65534` (extensible) — the actual
sample format is still 16-bit signed PCM in both cases.

### ❌ ffmpeg `join` filter mixing channels

**What we tried:** Using `join=inputs=6:channel_layout=5.1` to assemble multi-channel WAV.

**What happened:** `join` is designed for *upmixing* — it tries to be clever about
routing and can mix multiple inputs together. Haptic content bled into the overlay channels.

**Solution:** Use `amerge=inputs=N` instead. `amerge` strictly interleaves channels in
the exact order specified, with no mixing between them.

---

## LED

### ❌ FastLED.show() impacting audio / haptic

**What we tried:** Calling `FastLED.show()` without rate limiting from `loop()`.

**What happened:** `AudioOutputPWM` (haptic) showed audible stutter correlated with
LED frame updates. The PWM DMA timing is more sensitive than I2S (which has a hardware
FIFO buffer).

**Solution:**
- Hard cap at 25fps (`LED_FRAME_MS = 40`)
- `yield()` after every `FastLED.show()` to let the audio ISR catch up if it was deferred

### ❌ Very low intensity values producing "off" periods

**What we tried:** Idle animation at `intensity: 0.12` with `breathe`.

**What happened:** The breathe animation's minimum (8% of intensity × 0.12 ≈ 1%)
was below the LED's visual threshold. Combined with a slow speed (0.15), the LEDs
appeared to be off for ~10 seconds per cycle.

**Solution:**
- Minimum brightness floor of 25% in the breathe range (`0.25 + t * 0.75`)
- Absolute brightness floor in the `col()` helper (`0.05 + brightness * 0.95`)
- Raised idle intensity to 0.35

### ❌ setBackground() resetting animation phase on every call

**What we tried:** Calling `setBackground()` unconditionally every time a chapter
was entered.

**What happened:** If the same animation was already playing (e.g. entering intro
resets breathe phase to 0), the LED would visibly "restart" the animation —
perceptible as a brightness jump.

**Solution:** Phase and step only reset when the animation *type* changes. Updating
color/intensity/speed while keeping the same animation continues smoothly.

---

## Platform Considerations

### Raspberry Pi Pico 2W — evaluated but not chosen

The Pico 2W (RP2350) was evaluated as an alternative. Key blockers:

- No Teensy Audio Library equivalent — no `AudioMixer4`, no `AudioPlaySdWav`,
  no patch-bay model. Everything would need rewriting.
- `BackgroundAudio` (best Pico audio library) cannot read from SD inside the
  audio ISR — requires main-loop feeding, which makes our SD-read-per-ISR
  architecture impossible.
- No Audio Shield equivalent — external I2S DAC required.
- 520KB RAM vs 1MB — tighter for RAM FX sample buffers.

The Teensy 4.1's Cortex-M7 at 600MHz with DSP extensions, 1MB RAM, and the
mature Teensy Audio Library make it significantly better suited for this application.

**If Pico 2W is needed later:** The built-in WiFi would enable proper
device-to-device sync over UDP — a cleaner solution than NFC proximity detection
for the duet mechanic.