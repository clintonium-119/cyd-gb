"""Shared library for games.json: caps, validation, catalog emission, mirror parser.

The one module the validator, the ES-DE seed and the SD imaging tool all import,
so every cap in docs/CATALOG_FORMAT.md is enforced from a single definition and
/catalog.txt is emitted by a single function.

The caps below mirror the #defines in the firmware headers named beside each
one; read_header_caps() re-reads those headers so a test can fail when the two
drift apart. Nothing here parses or writes the firmware's files — it only has to
agree with them.

parse_catalog_line() is a mirror of the reader in lib/gbcore/cart/catalog.c: it
applies the same rules so a round trip can be asserted on the host, and so a
catalog the device would refuse cannot pass a host test.

Run from the project root. Standard library only.
"""

import json
import re
from pathlib import Path

# lib/gbcore/cart/rom_store.h — ROM_STORE_NAME_MAX, including the NUL.
ROM_STORE_NAME_MAX = 64

# lib/gbcore/cart/catalog.h — all four include the NUL where they cap a string.
CATALOG_TITLE_MAX = 48
CATALOG_DESC_MAX = 201
CATALOG_LINE_MAX = 384
CATALOG_MAX = 160

# The only flag token the firmware understands; unknown tokens are ignored
# there, so the generator may add more without a firmware change.
FLAG_STARTER = "starter"

# The ROM extension. Library names contain dots, so anything that manipulates a
# filename tests for this suffix and never for the presence of a dot.
ROM_SUFFIX = ".gb"

ROM_DIR_ENV = "CYD_ROM_DIR"
MEDIA_DIR_ENV = "CYD_MEDIA_DIR"

# The catalog line's fields, in the order they are emitted.
CATALOG_FIELDS = ("filename", "title", "flags", "description")

# Every field a games.json entry carries, in the order the seed writes them.
GAME_FIELDS = (
    "filename",
    "title",
    "description",
    "art",
    "shot",
    "manual",
    "starter",
    "developer",
    "publisher",
    "year",
    "genre",
    "players",
)

# Where read_header_caps() looks for each cap.
HEADER_SOURCES = {
    "ROM_STORE_NAME_MAX": "lib/gbcore/cart/rom_store.h",
    "CATALOG_TITLE_MAX": "lib/gbcore/cart/catalog.h",
    "CATALOG_DESC_MAX": "lib/gbcore/cart/catalog.h",
    "CATALOG_LINE_MAX": "lib/gbcore/cart/catalog.h",
    "CATALOG_MAX": "lib/gbcore/cart/catalog.h",
}


def byte_len(text):
    """Length of a string in bytes, which is what every cap counts."""
    return len(text.encode("utf-8"))


def read_header_caps(repo_root):
    """The five caps as the firmware headers declare them, for the drift guard."""
    caps = {}
    for name, relative in HEADER_SOURCES.items():
        source = Path(repo_root) / relative
        text = source.read_text(encoding="utf-8")
        match = re.search(r"^#define\s+" + name + r"\s+(\d+)\s*$", text, re.MULTILINE)
        if match is None:
            raise ValueError(f"{relative} does not #define {name}")
        caps[name] = int(match.group(1))
    return caps


def stem(filename):
    """The filename with its .gb removed — the name /art, /shot and /saves use."""
    if filename.endswith(ROM_SUFFIX):
        return filename[: -len(ROM_SUFFIX)]
    return filename


def load_games(path):
    """Load games.json.

    The decoded document is returned as it was found, including a top-level
    object: "the array is missing" is a validate() problem naming the shape, not
    an exception raised from underneath the caller.
    """
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def _label(index, game):
    """How a problem names an entry: by filename when it has a usable one."""
    if isinstance(game, dict):
        filename = game.get("filename")
        if isinstance(filename, str) and filename != "":
            return filename
    return f"entry {index}"


def _check_string(problems, label, field, value, cap, allow_empty):
    """The checks every emitted string shares. True when the value is usable."""
    if not isinstance(value, str):
        problems.append(f"{label}: {field} must be a string, not {type(value).__name__}")
        return False
    if value == "" and not allow_empty:
        problems.append(f"{label}: {field} is empty")
        return False
    if byte_len(value) > cap:
        problems.append(
            f"{label}: {field} is {byte_len(value)} bytes, the cap is {cap}"
        )
    if not value.isascii():
        problems.append(f"{label}: {field} is not plain ASCII")
    if "\t" in value:
        problems.append(f"{label}: {field} contains a tab")
    if "\n" in value:
        problems.append(f"{label}: {field} contains a newline")
    return True


def _check_optional_int(problems, label, field, value):
    """year and players: an integer or null. A bool is not an integer here."""
    if value is None:
        return
    if isinstance(value, bool) or not isinstance(value, int):
        problems.append(
            f"{label}: {field} must be an integer or null, not {type(value).__name__}"
        )


def _check_recorded_string(problems, label, field, value):
    """developer, publisher and genre are not emitted, so only the type matters."""
    if not isinstance(value, str):
        problems.append(f"{label}: {field} must be a string, not {type(value).__name__}")


def _check_entry(problems, index, game, seen):
    """Every rule for one entry. Returns True when its catalog line can be built."""
    label = _label(index, game)

    for field in GAME_FIELDS:
        if field not in game:
            problems.append(f"{label}: {field} is missing")

    filename = game.get("filename")
    filename_ok = _check_string(
        problems, label, "filename", filename, ROM_STORE_NAME_MAX - 1, allow_empty=False
    )
    if filename_ok:
        if not filename.endswith(ROM_SUFFIX):
            problems.append(f"{label}: filename does not end in {ROM_SUFFIX}")
        if "/" in filename:
            problems.append(f"{label}: filename contains a path separator")
        if filename in seen:
            problems.append(f"{label}: filename duplicates entry {seen[filename]}")
        else:
            seen[filename] = index

    title_ok = _check_string(
        problems, label, "title", game.get("title"), CATALOG_TITLE_MAX - 1,
        allow_empty=False
    )
    description_ok = _check_string(
        problems, label, "description", game.get("description"), CATALOG_DESC_MAX - 1,
        allow_empty=True
    )

    for field in ("art", "shot", "manual"):
        value = game.get(field)
        if not isinstance(value, str):
            problems.append(
                f"{label}: {field} must be a string, not {type(value).__name__}"
            )

    if not isinstance(game.get("starter"), bool):
        problems.append(
            f"{label}: starter must be true or false, not "
            f"{type(game.get('starter')).__name__}"
        )

    for field in ("developer", "publisher", "genre"):
        _check_recorded_string(problems, label, field, game.get(field))

    for field in ("year", "players"):
        _check_optional_int(problems, label, field, game.get(field))

    return filename_ok and title_ok and description_ok


def _check_line_length(problems, label, game):
    """The emitted line, with its three tabs but not its newline, against the cap."""
    length = byte_len(catalog_line(game))
    if length > CATALOG_LINE_MAX - 1:
        problems.append(
            f"{label}: its catalog line is {length} bytes, the cap is "
            f"{CATALOG_LINE_MAX - 1}"
        )


def _check_sources(problems, label, game, rom_dir, media_dir):
    """The ROM and the three media sources exist, when their directory was given."""
    if rom_dir is not None:
        rom = Path(rom_dir) / game["filename"]
        if not rom.is_file():
            problems.append(f"{label}: no ROM at {rom}")
    if media_dir is None:
        return
    for field in ("art", "shot", "manual"):
        relative = game.get(field)
        if not isinstance(relative, str) or relative == "":
            continue
        source = Path(media_dir) / relative
        if not source.is_file():
            problems.append(f"{label}: no {field} source at {source}")


def validate(games, rom_dir=None, media_dir=None):
    """Check a loaded games.json against docs/CATALOG_FORMAT.md.

    Returns (problems, notices). Problems are contract violations and each one
    names the entry it came from. Notices are things the caller should know but
    that do not fail the file: a check that was skipped because its directory was
    not given, and how many entries have no cover, snapshot or manual source.

    rom_dir holds the ROM files directly — the /roms/gb of the card is built by
    the imaging tool, not expected of the source directory. media_dir is the
    directory art, shot and manual are relative to.
    """
    problems = []
    notices = []

    if not isinstance(games, list):
        problems.append(
            f"games.json must be a top-level array, not a "
            f"{type(games).__name__}"
        )
        return problems, notices

    if len(games) > CATALOG_MAX:
        problems.append(
            f"{len(games)} entries, but the catalog holds at most {CATALOG_MAX}"
        )

    seen = {}
    empty_art = 0
    empty_shot = 0
    empty_manual = 0

    for index, game in enumerate(games):
        if not isinstance(game, dict):
            problems.append(
                f"entry {index} is not an object, it is a {type(game).__name__}"
            )
            continue
        label = _label(index, game)
        emittable = _check_entry(problems, index, game, seen)
        if emittable:
            _check_line_length(problems, label, game)
            _check_sources(problems, label, game, rom_dir, media_dir)
        if game.get("art") == "":
            empty_art += 1
        if game.get("shot") == "":
            empty_shot += 1
        if game.get("manual") == "":
            empty_manual += 1

    if rom_dir is None:
        notices.append(
            f"ROM existence not checked: no ROM directory given (${ROM_DIR_ENV})"
        )
    if media_dir is None:
        notices.append(
            f"art, shot and manual existence not checked: no media directory given "
            f"(${MEDIA_DIR_ENV})"
        )
    notices.append(f"{empty_art} entries have no cover source")
    notices.append(f"{empty_shot} entries have no snapshot source")
    notices.append(f"{empty_manual} entries have no manual source")

    return problems, notices


def catalog_flags(game):
    """The flags field: a comma-separated token list, empty when nothing is set."""
    tokens = []
    if game.get("starter"):
        tokens.append(FLAG_STARTER)
    return ",".join(tokens)


def catalog_line(game):
    """One /catalog.txt line, without its newline."""
    return "\t".join(
        (
            game["filename"],
            game["title"],
            catalog_flags(game),
            game["description"],
        )
    )


def emit_catalog(games):
    """The whole /catalog.txt: LF-separated lines and a trailing newline, no CR."""
    return "".join(catalog_line(game) + "\n" for game in games)


def parse_flags(field):
    """The reader's flag parse: comma tokens, unknown ones ignored."""
    return FLAG_STARTER in field.split(",")


def parse_catalog_line(line):
    """Parse one line the way catalog_parse_line() in catalog.c does.

    A trailing CR is stripped, as read_line_at() strips it. Only the first three
    tabs separate fields, so the description keeps any tab after them. An
    over-long filename or title is rejected rather than truncated: a truncated
    name would match the wrong ROM.
    """
    if line.endswith("\r"):
        line = line[:-1]

    parts = line.split("\t", 3)
    if len(parts) < 4:
        raise ValueError(f"line has {len(parts) - 1} tabs, it needs three: {line!r}")

    filename, title, flags, description = parts
    if filename == "":
        raise ValueError(f"line has an empty filename: {line!r}")
    if byte_len(filename) >= ROM_STORE_NAME_MAX:
        raise ValueError(
            f"filename is {byte_len(filename)} bytes, the reader holds "
            f"{ROM_STORE_NAME_MAX - 1}: {filename!r}"
        )
    if byte_len(title) >= CATALOG_TITLE_MAX:
        raise ValueError(
            f"title is {byte_len(title)} bytes, the reader holds "
            f"{CATALOG_TITLE_MAX - 1}: {title!r}"
        )

    return {
        "filename": filename,
        "title": title,
        "starter": parse_flags(flags),
        "description": description,
    }


def parse_catalog(text):
    """Parse a whole catalog the way for_each_line() walks one.

    An empty line is not an entry, including the one the trailing newline leaves
    at the end of the file. A line that would fill the reader's buffer is
    rejected, as read_line_at() rejects it.
    """
    entries = []
    for line in text.split("\n"):
        stripped = line[:-1] if line.endswith("\r") else line
        if stripped == "":
            continue
        if byte_len(stripped) >= CATALOG_LINE_MAX:
            raise ValueError(
                f"line is {byte_len(stripped)} bytes, the reader's buffer is "
                f"{CATALOG_LINE_MAX}: {stripped[:40]!r}..."
            )
        entries.append(parse_catalog_line(line))
    return entries


def round_trip(games):
    """Emit a catalog, read it back, and report every field that did not survive."""
    mismatches = []
    parsed = parse_catalog(emit_catalog(games))

    if len(parsed) != len(games):
        mismatches.append(
            f"emitted {len(games)} entries but parsed back {len(parsed)}"
        )
        return mismatches

    for game, entry in zip(games, parsed):
        for field in ("filename", "title", "starter", "description"):
            want = bool(game.get(field)) if field == "starter" else game[field]
            if entry[field] != want:
                mismatches.append(
                    f"{game['filename']}: {field} became {entry[field]!r}, "
                    f"was {want!r}"
                )
    return mismatches

