#!/bin/bash
# Assembles stems into a 7.1 WAV for the Teensy player.
#
# Output channel order (7.1 standard):
#   [0] FL  = base L        (stereo)
#   [1] FR  = base R        (stereo)
#   [2] FC  = narration     (mono)
#   [3] LFE = haptic        (mono)
#   [4] BL  = overlay 1 L   (stereo)
#   [5] BR  = overlay 1 R   (stereo)
#   [6] SL  = overlay 2 L   (stereo)
#   [7] SR  = overlay 2 R   (stereo)
#
# Usage: ./assemble_71.sh base.wav narr.wav ov1.wav ov2.wav haptic.wav [output.wav]
# All inputs: 44100Hz 16-bit PCM WAV
# Stereo: base, ov1, ov2 — Mono: narr, haptic

BASE=$1
NARR=$2
OV1=$3
OV2=$4
HAPTIC=$5
OUTPUT=${6:-experience.wav}

if [ -z "$BASE" ] || [ -z "$NARR" ] || [ -z "$OV1" ] || [ -z "$OV2" ] || [ -z "$HAPTIC" ]; then
    echo "Usage: $0 base.wav narr.wav ov1.wav ov2.wav haptic.wav [output.wav]"
    echo ""
    echo "  base.wav    stereo  FL+FR channels (main music bed)"
    echo "  narr.wav    mono    FC channel (narration/cues)"
    echo "  ov1.wav     stereo  BL+BR channels (overlay stem 1)"
    echo "  ov2.wav     stereo  SL+SR channels (overlay stem 2)"
    echo "  haptic.wav  mono    LFE channel (haptic transducer)"
    exit 1
fi

for f in "$BASE" "$NARR" "$OV1" "$OV2" "$HAPTIC"; do
    if [ ! -f "$f" ]; then echo "ERROR: not found: $f"; exit 1; fi
done

echo "Assembling 7.1 WAV (FL FR FC LFE BL BR SL SR)..."
echo "  [0][1] FL/FR  base:     $BASE"
echo "  [2]    FC     narr:     $NARR"
echo "  [3]    LFE    haptic:   $HAPTIC"
echo "  [4][5] BL/BR  overlay1: $OV1"
echo "  [6][7] SL/SR  overlay2: $OV2"
echo "  Output: $OUTPUT"
echo ""

ffmpeg -y \
    -i "$BASE" \
    -i "$NARR" \
    -i "$OV1" \
    -i "$OV2" \
    -i "$HAPTIC" \
    -filter_complex "
        [0:a]channelsplit=channel_layout=stereo[fl][fr];
        [2:a]channelsplit=channel_layout=stereo[bl][br];
        [3:a]channelsplit=channel_layout=stereo[sl][sr];
        [fl][fr][1:a][4:a][bl][br][sl][sr]amerge=inputs=8[out]
    " \
    -map "[out]" \
    -ar 44100 \
    -c:a pcm_s16le \
    "$OUTPUT"

if [ $? -eq 0 ]; then
    SIZE=$(du -h "$OUTPUT" | cut -f1)
    BITRATE=$(echo "scale=1; 44100 * 8 * 2 / 1000" | bc)
    echo ""
    echo "Done: $OUTPUT ($SIZE)"
    echo "Bitrate: ${BITRATE} KB/sec — well within SD bandwidth"
    echo ""
    echo "Copy to SD:"
    echo "  cp \"$OUTPUT\" /Volumes/SDCARD/"
    echo "  dot_clean /Volumes/SDCARD/"
else
    echo "ERROR: ffmpeg failed. Install: brew install ffmpeg"
fi