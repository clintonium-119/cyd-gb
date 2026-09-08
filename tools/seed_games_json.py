"""One-shot seed for games.json from the curated ROM directory and ES-DE's gamelist.

Matches each curated ROM stem to a gamelist <name> — exactly first, then through
tools/esde_aliases.json — pulls the scraped metadata, resolves the cover and
snapshot paths the way ES-DE names its media, truncates the description to the
catalog's 200-byte cap at a sentence boundary, normalises it to plain ASCII, and
writes a deterministic games.json.

Media paths are relative to the media directory named by CYD_MEDIA_DIR, and are
built from the gamelist entry's <path> rather than from the curated stem: ES-DE
files its media under the ROM's own subfolder, so resolving through <path> finds
art for aliased titles too.

This is run ONCE. After it, games.json is hand-curated — the descriptions it
writes are mechanical truncations of scraped text, not wording written for kids,
and no entry is marked starter. It refuses to overwrite an existing games.json
without --force for that reason.

Unmatched stems are reported with suggestions, never guessed at: a wrong match
would put another game's description on a cartridge.

Run from the project root:

    python tools/seed_games_json.py --report
    python tools/seed_games_json.py

Standard library only.
"""

import argparse
import difflib
import json
import os
import re
import sys
import unicodedata
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import gamesdb

REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = Path(__file__).resolve().parent

DEFAULT_GAMELIST = Path.home() / "ES-DE" / "gamelists" / "gb" / "gamelist.xml"
DEFAULT_ALIASES = TOOLS_DIR / "esde_aliases.json"
DEFAULT_OUT = REPO_ROOT / "games.json"

# The media subdirectory each games.json field is resolved under.
ART_KIND = "covers"
SHOT_KIND = "screenshots"

# ES-DE's scraper writes PNG for nearly everything, but not for everything: the
# screenshots directory of the library this was measured against holds two JPEGs
# among 641 files. Both are tried, in this order.
MEDIA_EXTENSIONS = (".png", ".jpg")

# The catalog's description cap, in bytes.
DESCRIPTION_LIMIT = gamesdb.CATALOG_DESC_MAX - 1

# Characters the scraped descriptions carry that have a plain-ASCII equivalent.
# Accents are handled by decomposition instead; see normalise_ascii().
ASCII_REPLACEMENTS = {
    "‘": "'",
    "’": "'",
    "‚": "'",
    "‛": "'",
    "′": "'",
    "“": '"',
    "”": '"',
    "„": '"',
    "″": '"',
    "–": "-",
    "—": "-",
    "―": "-",
    "…": "...",
    " ": " ",
}

# Sentence ends truncate_description() is willing to cut after.
SENTENCE_ENDS = ".!?"

# How close a gamelist <name> has to be for --report to suggest it. Most of the
# curated stems that need an alias are shortenings, and difflib scores a pure
# truncation surprisingly low: against the measured library, 0.6 left 4 of the
# 47 unmatched stems with no suggestion at all and 0.5 leaves 1.
SUGGESTION_CUTOFF = 0.5


def curated_stems(rom_dir):
    """Every curated ROM's stem, sorted — the library's own names, frozen."""
    names = [path.name for path in Path(rom_dir).glob("*" + gamesdb.ROM_SUFFIX)]
    return sorted(gamesdb.stem(name) for name in names)


def load_gamelist(path):
    """The gamelist's <game> elements keyed by <name>.

    Keyed by name and not by path because the paths are zips inside curated
    subfolders and never equal a curated filename.
    """
    root = ElementTree.parse(path).getroot()
    games = {}
    for game in root.findall("game"):
        name = game.findtext("name")
        if name:
            games[name] = game
    return games


def load_aliases(path):
    """The curated stem -> gamelist <name> map, or {} when it has not been written."""
    alias_path = Path(path)
    if not alias_path.is_file():
        return {}
    with open(alias_path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def match(stems, gamelist, aliases):
    """Pair each stem with its gamelist entry. Returns (matched, unmatched).

    An exact <name> wins over an alias, so adding an alias can never shadow a
    title that already matches.
    """
    matched = {}
    unmatched = []
    for stem in stems:
        if stem in gamelist:
            matched[stem] = gamelist[stem]
            continue
        alias = aliases.get(stem)
        if alias is not None and alias in gamelist:
            matched[stem] = gamelist[alias]
            continue
        unmatched.append(stem)
    return matched, unmatched


def media_relpath(game, kind, media_dir):
    """The media path for one entry, relative to media_dir, or "" when absent.

    ES-DE mirrors the ROM's own location: a <path> of
    `./01) All but the Best/Foo.zip` puts its cover at
    `covers/01) All but the Best/Foo.png`.
    """
    path = game.findtext("path") or ""
    if path.startswith("./"):
        path = path[2:]
    if path == "":
        return ""
    base = os.path.splitext(path)[0]
    for extension in MEDIA_EXTENSIONS:
        relative = f"{kind}/{base}{extension}"
        if (Path(media_dir) / relative).is_file():
            return relative
    return ""


def normalise_ascii(text):
    """Fold scraped text towards plain ASCII and collapse its whitespace.

    Anything with no ASCII equivalent is left in place rather than dropped, so
    the validator fails on it and a human sees exactly what to reword.
    """
    for source, target in ASCII_REPLACEMENTS.items():
        text = text.replace(source, target)
    decomposed = unicodedata.normalize("NFKD", text)
    stripped = "".join(
        character
        for character in decomposed
        if not unicodedata.combining(character)
    )
    return " ".join(stripped.split())


def truncate_description(text, limit=DESCRIPTION_LIMIT):
    """Cut text to at most `limit` BYTES, at a sentence end, else at a word.

    Bytes and not characters, because the cap the firmware enforces is a byte
    count. Never mid-word: a blurb that stops mid-word reads as a bug.
    """
    if len(text.encode("utf-8")) <= limit:
        return text

    head = text.encode("utf-8")[:limit].decode("utf-8", "ignore")

    cut = -1
    for index, character in enumerate(head):
        if character not in SENTENCE_ENDS:
            continue
        following = text[index + 1] if index + 1 < len(text) else ""
        if following in ("", " "):
            cut = index
    if cut >= 0:
        return head[: cut + 1].rstrip()

    space = head.rfind(" ")
    if space > 0:
        return head[:space].rstrip()
    return head.rstrip()


def players_max(text):
    """The upper bound of ES-DE's players field: "1-2" -> 2, "1" -> 1, absent -> None."""
    if not text:
        return None
    numbers = re.findall(r"\d+", text)
    if not numbers:
        return None
    return max(int(number) for number in numbers)


def year_of(releasedate):
    """The year out of an ES-DE releasedate such as `19920901T000000`."""
    if not releasedate:
        return None
    match_ = re.match(r"(\d{4})", releasedate)
    if match_ is None:
        return None
    return int(match_.group(1))


def bare_entry(stem):
    """The entry for a stem the gamelist does not describe at all.

    Every curated ROM needs a line in the catalog: the catalog is generated from
    games.json, so a stem missing from it would be imaged onto the card with no
    entry, and a cartridge written for it would read as "not found" on a board
    that is holding the game. An empty description and no art are ordinary
    cases; a missing entry is not. Nothing is invented — the description is left
    empty for a human to write.
    """
    return {
        "filename": stem + gamesdb.ROM_SUFFIX,
        "title": stem,
        "description": "",
        "art": "",
        "shot": "",
        "starter": False,
        "developer": "",
        "publisher": "",
        "year": None,
        "genre": "",
        "players": None,
    }


def display_title(stem, game):
    """The title the catalog shows for a matched stem.

    The gamelist <name>, normalised, rather than the ROM stem: the stems were
    truncated to fit a smaller display in an earlier life of this library, and
    the catalog keeps `filename` and `title` apart precisely so the key can stay
    frozen while the display name reads properly. A name that will not fit the
    title cap falls back to the stem — the seed never emits a title it would
    then have to reject, and it never truncates one, because a truncated display
    name is the thing this is fixing.
    """
    name = normalise_ascii(game.findtext("name") or "")
    if name == "" or gamesdb.byte_len(name) > gamesdb.CATALOG_TITLE_MAX - 1:
        return stem
    return name


def build_entries(matched, media_dir, unmatched=()):
    """One games.json object per curated stem, in display order.

    Nothing is marked starter — that is curation.
    """
    entries = {}
    for stem in matched:
        game = matched[stem]
        entries[stem] = {
            "filename": stem + gamesdb.ROM_SUFFIX,
            "title": display_title(stem, game),
            "description": truncate_description(
                normalise_ascii(game.findtext("desc") or "")
            ),
            "art": media_relpath(game, ART_KIND, media_dir),
            "shot": media_relpath(game, SHOT_KIND, media_dir),
            "starter": False,
            "developer": game.findtext("developer") or "",
            "publisher": game.findtext("publisher") or "",
            "year": year_of(game.findtext("releasedate")),
            "genre": game.findtext("genre") or "",
            "players": players_max(game.findtext("players")),
        }
    for stem in unmatched:
        entries[stem] = bare_entry(stem)
    # By the displayed title, not the stem: file order is display order, and a
    # list sorted by anything other than what it shows reads as unsorted.
    return sorted(entries.values(), key=lambda entry: entry["title"].lower())


def write_games(entries, out, force=False):
    """Write games.json. Refuses to overwrite unless force, because this is one-shot."""
    out_path = Path(out)
    if out_path.exists() and not force:
        raise FileExistsError(
            f"{out_path} already exists; the seed is one-shot and would discard "
            f"the curation in it. Pass --force to overwrite it anyway."
        )
    text = json.dumps(entries, indent=2, ensure_ascii=True) + "\n"
    out_path.write_text(text, encoding="utf-8")
    return text


def report_unmatched(unmatched, gamelist, stream):
    """Print the stems no <name> or alias matched, with suggestions to alias them."""
    names = list(gamelist)
    print(f"{len(unmatched)} curated stems matched no gamelist <name>:", file=stream)
    for stem in unmatched:
        suggestions = difflib.get_close_matches(
            stem, names, n=3, cutoff=SUGGESTION_CUTOFF
        )
        if suggestions:
            print(f"  {stem}  ->  {' | '.join(suggestions)}", file=stream)
        else:
            print(f"  {stem}  ->  (no close match)", file=stream)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gamelist", default=str(DEFAULT_GAMELIST))
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
    parser.add_argument("--aliases", default=str(DEFAULT_ALIASES))
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument(
        "--report",
        action="store_true",
        help="list the unmatched stems with suggestions and write nothing",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="overwrite an existing games.json",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    if args.rom_dir == "":
        print(
            f"no ROM directory: pass --rom-dir or set ${gamesdb.ROM_DIR_ENV}",
            file=sys.stderr,
        )
        return 2
    if args.media_dir == "":
        print(
            f"no media directory: pass --media-dir or set ${gamesdb.MEDIA_DIR_ENV}",
            file=sys.stderr,
        )
        return 2

    stems = curated_stems(args.rom_dir)
    if not stems:
        print(f"no {gamesdb.ROM_SUFFIX} files under {args.rom_dir}", file=sys.stderr)
        return 2

    gamelist = load_gamelist(args.gamelist)
    aliases = load_aliases(args.aliases)
    matched, unmatched = match(stems, gamelist, aliases)

    print(
        f"{len(stems)} curated stems, {len(gamelist)} gamelist names, "
        f"{len(aliases)} aliases: {len(matched)} matched, {len(unmatched)} not",
        file=sys.stderr,
    )

    if args.report:
        report_unmatched(unmatched, gamelist, sys.stderr)
        return 0

    if unmatched:
        report_unmatched(unmatched, gamelist, sys.stderr)
        print(
            "those stems got a bare entry — filename and title only, no "
            f"description and no art. Alias what you can in {args.aliases} and "
            "rerun with --force; write the rest by hand.",
            file=sys.stderr,
        )

    entries = build_entries(matched, args.media_dir, unmatched)
    write_games(entries, args.out, force=args.force)
    print(f"wrote {len(entries)} entries to {args.out}", file=sys.stderr)

    problems, notices = gamesdb.validate(
        entries, rom_dir=args.rom_dir, media_dir=args.media_dir
    )
    for notice in notices:
        print(f"note: {notice}", file=sys.stderr)
    for problem in problems:
        print(f"problem: {problem}", file=sys.stderr)
    if problems:
        print(f"{len(problems)} problems; curate the file and revalidate", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
