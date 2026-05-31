#!/bin/bash
# Assembles stems into a 5.1 WAV for the Teensy player.
#
# Output channel order (5.1 standard):
#   [0] FL  = base L       (stereo input, left)
#   [1] FR  = base R       (stereo input, right)
#   [2] FC  = narration    (mono input)
#   [3] LFE = haptic       (mono input)
#   [4] BL  = overlay L    (stereo input, left)
#   [5] BR  = overlay R    (stereo input, right)
#
# Usage: ./assemble_51.sh base.wav narr.wav overlay.wav haptic.wav [output.wav]

BASE=$1
NARR=$2
OVERLAY=$3
HAPTIC=$4
OUTPUT=${5:-experience.wav}

if [ -z "$BASE" ] || [ -z "$NARR" ] || [ -z "$OVERLAY" ] || [ -z "$HAPTIC" ]; then
    echo "Usage: $0 base.wav narr.wav overlay.wav haptic.wav [output.wav]"
    exit 1
fi

for f in "$BASE" "$NARR" "$OVERLAY" "$HAPTIC"; do
    if [ ! -f "$f" ]; then echo "ERROR: not found: $f"; exit 1; fi
done

echo "Assembling 5.1 WAV..."
echo "  [0][1] FL/FR base:    $BASE"
echo "  [2]    FC  narr:      $NARR"
echo "  [3]    LFE haptic:    $HAPTIC"
echo "  [4][5] BL/BR overlay: $OVERLAY"
echo "  Output:               $OUTPUT"

# Strategy: convert all inputs to mono streams, then amerge in order.
# amerge strictly interleaves — no mixing between channels.
#
# Input order for amerge:
#   base_L, base_R, narr, haptic, overlay_L, overlay_R
#
ffmpeg -y \
    -i "$BASE" \
    -i "$NARR" \
    -i "$OVERLAY" \
    -i "$HAPTIC" \
    -filter_complex "
        [0:a]channelsplit=channel_layout=stereo[base_l][base_r];
        [2:a]channelsplit=channel_layout=stereo[ov_l][ov_r];
        [base_l][base_r][1:a][3:a][ov_l][ov_r]amerge=inputs=6[out]
    " \
    -map "[out]" \
    -ar 44100 \
    -c:a pcm_s16le \
    "$OUTPUT"

if [ $? -eq 0 ]; then
    SIZE=$(du -h "$OUTPUT" | cut -f1)
    echo "Done: $OUTPUT ($SIZE)"
    echo ""
    echo "Verifying channel order..."
    ffprobe -v quiet -print_format json -show_streams "$OUTPUT" 2>/dev/null | \
        grep -E "channel_layout|channels"
    echo ""
    echo "Copy to SD:  cp \"$OUTPUT\" /Volumes/SDCARD/ && dot_clean /Volumes/SDCARD/"
else
    echo "ERROR: ffmpeg failed. Install: brew install ffmpeg"
fi