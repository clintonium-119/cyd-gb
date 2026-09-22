#!/usr/bin/env bash
#
# Blind trial for the panel rate trim.
#
# Two effects on this project were nearly dismissed from unblinded single
# looks, so the trim's claim is scored the way the trial of 2026-09-21 was: the
# arm comes from the kernel's entropy inside this script, is written straight
# to a log, and is never printed until you ask to reveal it. Flash, play,
# score, repeat; read the log only when you are done scoring.
#
# Every arm takes its porch from the build rather than from NVS, so the arms
# differ in exactly two numbers and none depends on what the unit happens to
# have stored.
#
# ARMS is a space-separated list of fpa+ratio pairs. The default set spans the
# question the bench of 2026-09-22 raised: whether nulling the beat is the
# right objective at all. A null makes the seam rare and SLOW, so each visit
# dwells on screen for many seconds; detuning makes it frequent and FAST,
# which vision discards more readily. The share of time a seam is on screen
# may be much the same either way, in which case the null is the worse of the
# two and the whole trim is pointed backwards.
#
# Score these for ANNOYANCE WHILE PLAYING, not for the interval between
# crossings — the interval is what the trial of 2026-09-21 measured, and
# measuring it is what left the preference question open.
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
# porch      beat      crossing   what it is
#  10+54      ~0        minutes    the calibrated null: rare, slow, dwells
#  12+0       -0.20 Hz  5.0 s      the panel's own power-on porch, untrimmed
#   8+0       +0.50 Hz  2.0 s      deliberately detuned
#   5+0       +1.04 Hz  1.0 s      detuned far: a flick rather than a sweep
ARMS=${ARMS:-"10+54 12+0 8+0 5+0"}

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

read -r -a arm_list <<< "$ARMS"
arm=${arm_list[$(( $(od -An -N2 -tu2 < /dev/urandom) % ${#arm_list[@]} ))]}
fpa=${arm%%+*}
ratio=${arm##*+}

mkdir -p "$(dirname "$LOG")"
trial=$(( $(wc -l < "$LOG" 2>/dev/null || echo 0) + 1 ))
printf '%s trial %2d porch=%s/64\n' "$(date -Is)" "$trial" "$arm" >> "$LOG"

# The nested quoting is what turns the ROM name into a C string literal rather
# than a bare token; a name with a space fails in a way that reads like a
# toolchain fault without it.
PLATFORMIO_BUILD_FLAGS="-DPANEL_TRIM_FORCE -DPANEL_TRIM_FPA=$fpa \
-DPANEL_TRIM_RATIO=$ratio -DDEV_ROM_PATH='\"$ROM\"' ${EXTRA:-}" \
    pio run -e cyd-gnuboy -t upload >/dev/null

echo "trial $trial flashed. Play it, score it for annoyance, run the next one."
echo "when the series is done: $0 --reveal"
