#!/usr/bin/env bash
#
# Blind A/B for the panel rate trim.
#
# Two effects on this project were nearly dismissed from unblinded single
# looks, so the trim's claim is scored the way the trial of 2026-09-21 was: the
# arm comes from the kernel's entropy inside this script, is written straight
# to a log, and is never printed until you ask to reveal it. Flash, play,
# score, repeat; read the log only when you are done scoring.
#
# Both arms take their porch from the build rather than from NVS, so the two
# differ in exactly two numbers and neither depends on what the unit happens to
# have stored.
#
#   ROM='Black Castle.gb' scripts/trim_ab.sh          one trial
#   scripts/trim_ab.sh --reveal                       the arms, after scoring
#   scripts/trim_ab.sh --reset                        start a new series
#
# Battery switch OFF before flashing: ON plus a USB reset gives the tick boot
# loop (BUG-0006).
#
# Do not open a serial monitor before scoring — the driver prints the porch it
# applied, which would tell you the arm.
set -euo pipefail

cd "$(dirname "$0")/.."
LOG=logs/trim_ab.log

# The calibrated porch this unit stores, and the panel's power-on porch, which
# is what "off" means: ST7789's PORCTRL default front porch is 0x0C with no
# fractional dither.
ON_FPA=${ON_FPA:-10}
ON_RATIO=${ON_RATIO:-54}
OFF_FPA=${OFF_FPA:-12}
OFF_RATIO=${OFF_RATIO:-0}

case "${1:-}" in
--reveal)
    [ -s "$LOG" ] || { echo "no trials recorded yet"; exit 1; }
    cat "$LOG"
    exit 0
    ;;
--reset)
    rm -f "$LOG"
    echo "series cleared"
    exit 0
    ;;
esac

if [ -z "${ROM:-}" ]; then
    echo "ROM is not set, and a build without one halts on 'Unreadable tag'" >&2
    echo "  ROM='Black Castle.gb' $0" >&2
    exit 1
fi

if (( $(od -An -N1 -tu1 < /dev/urandom) % 2 )); then
    arm=on;  fpa=$ON_FPA;  ratio=$ON_RATIO
else
    arm=off; fpa=$OFF_FPA; ratio=$OFF_RATIO
fi

mkdir -p "$(dirname "$LOG")"
trial=$(( $(wc -l < "$LOG" 2>/dev/null || echo 0) + 1 ))
printf '%s trial %2d arm=%-3s porch=%d+%d/64\n' \
    "$(date -Is)" "$trial" "$arm" "$fpa" "$ratio" >> "$LOG"

# The nested quoting is what turns the ROM name into a C string literal rather
# than a bare token; a name with a space fails in a way that reads like a
# toolchain fault without it.
PLATFORMIO_BUILD_FLAGS="-DPANEL_TRIM_FORCE -DPANEL_TRIM_FPA=$fpa \
-DPANEL_TRIM_RATIO=$ratio -DDEV_ROM_PATH='\"$ROM\"' ${EXTRA:-}" \
    pio run -e cyd-gnuboy -t upload >/dev/null

echo "trial $trial flashed. Play it, score it, then run the next one."
echo "when the series is done: $0 --reveal"
