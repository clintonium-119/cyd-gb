"""Erase NVS so the first-boot wizard re-arms. The flashing station's recovery gate.

An adult at the flashing station clears NVS; the next boot enters the wizard,
which re-adopts the kid's existing MENU and WILD cartridges, and any pending
write is dropped. This is the only way back to the wizard — there is
deliberately no on-device or button-combo route to it.

It erases exactly the `nvs` region, with the offset and size read from
partitions.csv rather than typed, so `app0` and `romdata` are untouched: the
unit keeps its firmware and its written ROM and needs no reflash. Not
`erase_flash`, which would take both of those with it.

Erased flash is all 0xFF, which is what the ESP-IDF NVS layer treats as an
empty partition and formats on the next `nvs_flash_init` — so there is no blank
image to write.

    python tools/factory_reset.py --dry-run
    python tools/factory_reset.py --port /dev/ttyUSB0

Standard library only, plus esptool from PlatformIO. Run from the project root.
"""

import argparse
import sys
from pathlib import Path

from flash import (
    AFTER_RESET,
    BEFORE_RESET,
    CHIP,
    DEFAULT_BAUD,
    find_esptool,
    parse_partitions,
)

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PARTITIONS = REPO_ROOT / "partitions.csv"

# The region to erase. Its offset and size are never written down here.
NVS_PARTITION = "nvs"

# What NVS holds, so the operator knows what they are dropping. Every key is in
# the "settings" namespace in src/settings.cpp — wz_menu, wz_wild, wz_done for
# the wizard flags, made for the carts-made list, p_rom and p_tgt for a pending
# write, and pal, fskip, bright, vol, gx, gy for the per-unit settings. A rename
# there should be findable from here.
DROPS = (
    "the first-boot wizard flags, so setup runs again",
    "the carts-made list",
    "any pending cartridge write",
    "the palette, frameskip, brightness and volume",
    "the per-unit GAME_X / GAME_Y screen nudge",
)

KEEPS = (
    "the firmware — no reflash needed",
    "the ROM written into the romdata partition",
    "every save on the SD card, and the card's whole contents",
    "the data already written on the kid's cartridges",
)


def reset_command(esptool, port, baud, offset, size):
    """The esptool argv that erases one region, and only one."""
    command = [*esptool, "--chip", CHIP]
    if port:
        command += ["--port", str(port)]
    command += [
        "--baud", str(baud),
        "--before", BEFORE_RESET,
        "--after", AFTER_RESET,
        "erase_region", f"0x{offset:x}", f"0x{size:x}",
    ]
    return command


def describe(offset, size, stream):
    """Say plainly what is lost and what survives, before asking."""
    print(
        f"Erasing the {NVS_PARTITION} partition at 0x{offset:x}, "
        f"0x{size:x} bytes.",
        file=stream,
    )
    print("\nThis drops:", file=stream)
    for line in DROPS:
        print(f"  - {line}", file=stream)
    print("\nThis keeps:", file=stream)
    for line in KEEPS:
        print(f"  - {line}", file=stream)
    print("", file=stream)


def confirm(offset, size):
    """Ask before erasing. Anything but a plain y is a no."""
    answer = input(
        f"Erase NVS at 0x{offset:x} (0x{size:x} bytes)? [y/N] "
    )
    return answer.strip().lower() == "y"


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--port",
        help="serial port; omitted means esptool's own auto-detect",
    )
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--partitions", default=str(DEFAULT_PARTITIONS))
    parser.add_argument(
        "--yes",
        action="store_true",
        help="skip the confirmation prompt",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the esptool command and exit without touching a board",
    )
    return parser.parse_args(argv)


def main(argv=None, runner=None):
    """`runner` is the subprocess entry point, injected so tests can watch it."""
    args = parse_args(argv)

    try:
        table = parse_partitions(args.partitions)
    except OSError as error:
        print(f"cannot read {args.partitions}: {error}", file=sys.stderr)
        return 1

    if NVS_PARTITION not in table:
        print(
            f"{args.partitions} has no {NVS_PARTITION} partition, so there is "
            f"nothing to erase; the table lists {', '.join(sorted(table)) or 'nothing'}",
            file=sys.stderr,
        )
        return 1

    offset, size = table[NVS_PARTITION]

    try:
        esptool = find_esptool()
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 1

    command = reset_command(esptool, args.port, args.baud, offset, size)

    if args.dry_run:
        describe(offset, size, sys.stderr)
        print(" ".join(command))
        return 0

    describe(offset, size, sys.stderr)
    if not args.yes and not confirm(offset, size):
        print("nothing was erased", file=sys.stderr)
        return 1

    if runner is None:
        import subprocess

        runner = subprocess.run

    print(" ".join(command), file=sys.stderr)
    status = runner(command).returncode
    if status == 0:
        print(
            "\nNVS erased. Power-cycle the unit with a blank or existing MENU "
            "cartridge in the slot to enter the wizard.",
            file=sys.stderr,
        )
    return status


if __name__ == "__main__":
    sys.exit(main())
