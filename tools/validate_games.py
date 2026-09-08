"""Validate games.json against the catalog contract. The command CI runs.

A thin CLI over gamesdb: every rule lives there, so the firmware's caps are
enforced from one definition and this file only decides what to print and what
to exit with. It also round-trips the emitted catalog through the mirror parser,
which catches an entry that validates but would not survive the reader.

The ROM and media directories come from the environment, and their existence
checks are skipped and reported when they are not set — CI has neither
directory, and a skipped check is not a failure there. Use --strict before
imaging a card, where both directories are present and a skipped check means the
run proved less than it looked like it did.

    python tools/validate_games.py
    CYD_ROM_DIR=... CYD_MEDIA_DIR=... python tools/validate_games.py --strict

Exits 0 when there are no problems and 1 when there are. Standard library only.
"""

import argparse
import json
import os
import sys
from pathlib import Path

import gamesdb

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GAMES = REPO_ROOT / "games.json"


def resolve_dir(value):
    """An env-var or flag directory: None when unset, so validate() skips it."""
    if value is None or value == "":
        return None
    return Path(value)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--games", default=str(DEFAULT_GAMES))
    parser.add_argument(
        "--rom-dir",
        default=os.environ.get(gamesdb.ROM_DIR_ENV, ""),
        help=f"the curated ROM directory; defaults to ${gamesdb.ROM_DIR_ENV}",
    )
    parser.add_argument(
        "--media-dir",
        default=os.environ.get(gamesdb.MEDIA_DIR_ENV, ""),
        help=f"the ES-DE media directory; defaults to ${gamesdb.MEDIA_DIR_ENV}",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="require both directories, so no existence check is skipped",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    rom_dir = resolve_dir(args.rom_dir)
    media_dir = resolve_dir(args.media_dir)

    problems = []
    notices = []
    games = []

    try:
        games = gamesdb.load_games(args.games)
    except FileNotFoundError:
        problems.append(f"no such file: {args.games}")
    except json.JSONDecodeError as error:
        problems.append(f"{args.games} is not valid JSON: {error}")

    if not problems:
        # --strict is expressed as "both directories must be given" rather than
        # by matching the text of a notice: it is the same condition, and the
        # exit status should not depend on prose.
        if args.strict and rom_dir is None:
            problems.append(
                f"--strict needs a ROM directory: pass --rom-dir or set "
                f"${gamesdb.ROM_DIR_ENV}"
            )
        if args.strict and media_dir is None:
            problems.append(
                f"--strict needs a media directory: pass --media-dir or set "
                f"${gamesdb.MEDIA_DIR_ENV}"
            )

        found, notices = gamesdb.validate(games, rom_dir=rom_dir, media_dir=media_dir)
        problems.extend(found)

        # Only worth asking once the shapes are known good: the emitter needs
        # the fields validate() has just checked for.
        if not found:
            problems.extend(gamesdb.round_trip(games))

    for problem in problems:
        print(f"problem: {problem}")
    for notice in notices:
        print(f"notice: {notice}")

    count = len(games) if isinstance(games, list) else 0
    print(
        f"{count} entries, {len(problems)} problems, {len(notices)} notices"
    )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
