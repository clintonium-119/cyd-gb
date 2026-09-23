"""Tests for tools/gamesdb.py — the caps, the validator, the emitter, the parser."""

import pytest

import gamesdb

# The six entries of test/fixtures/catalog.txt, re-expressed as games.json
# objects. The fixture is the only hand-written conforming catalog in the tree,
# so emitting these and comparing bytes is the emitter's conformance check.
FIXTURE_GAMES = [
    {
        "filename": "Tetris.gb",
        "title": "Tetris",
        "description": (
            "Fit the falling blocks into complete rows. One of the best puzzle "
            "games ever made, and the one that sold the Game Boy."
        ),
        "art": "",
        "shot": "",
        "manual": "",
        "starter": True,
        "developer": "",
        "publisher": "",
        "genre": "",
        "year": None,
        "players": None,
    },
    {
        "filename": "Dr. Mario.gb",
        "title": "Dr. Mario",
        "description": (
            "Drop coloured pills onto the viruses to line up four of a colour "
            "and clear them out."
        ),
        "art": "",
        "shot": "",
        "manual": "",
        "starter": True,
        "developer": "",
        "publisher": "",
        "genre": "",
        "year": None,
        "players": None,
    },
    {
        "filename": "Snow Bros. Jr..gb",
        "title": "Snow Bros. Jr.",
        "description": (
            "Roll your enemies into snowballs and send them flying down the level."
        ),
        "art": "",
        "shot": "",
        "manual": "",
        "starter": False,
        "developer": "",
        "publisher": "",
        "genre": "",
        "year": None,
        "players": None,
    },
    {
        "filename": "Super R.C. Pro-Am.gb",
        "title": "Super R.C. Pro-Am",
        "description": (
            "Race radio-controlled cars from above, collecting weapons and "
            "upgrades along the way."
        ),
        "art": "",
        "shot": "",
        "manual": "",
        "starter": False,
        "developer": "",
        "publisher": "",
        "genre": "",
        "year": None,
        "players": None,
    },
    {
        "filename": "Mr. Do!.gb",
        "title": "Mr. Do!",
        "description": (
            "Dig tunnels through the ground, drop apples on the monsters and "
            "collect the cherries."
        ),
        "art": "",
        "shot": "",
        "manual": "",
        "starter": False,
        "developer": "",
        "publisher": "",
        "genre": "",
        "year": None,
        "players": None,
    },
    {
        "filename": "Alleyway.gb",
        "title": "Alleyway",
        "description": "",
        "art": "",
        "shot": "",
        "manual": "",
        "starter": False,
        "developer": "",
        "publisher": "",
        "genre": "",
        "year": None,
        "players": None,
    },
]


def game(**overrides):
    """A conforming entry, with the fields under test overridden."""
    entry = {
        "filename": "Tetris.gb",
        "title": "Tetris",
        "description": "Fit the falling blocks into complete rows.",
        "art": "",
        "shot": "",
        "manual": "",
        "starter": False,
        "developer": "Nintendo",
        "publisher": "Nintendo",
        "genre": "Puzzle",
        "year": 1989,
        "players": 2,
    }
    entry.update(overrides)
    return entry


def problems_for(entry, **kwargs):
    problems, _ = gamesdb.validate([entry], **kwargs)
    return problems


def rejected(entry, needle, **kwargs):
    """True when some problem for this entry mentions `needle` and its filename."""
    problems = problems_for(entry, **kwargs)
    return any(needle in problem for problem in problems)


# --- the caps -------------------------------------------------------------


def test_constants_mirror_the_firmware_headers(repo_root):
    assert gamesdb.read_header_caps(repo_root) == {
        "ROM_STORE_NAME_MAX": gamesdb.ROM_STORE_NAME_MAX,
        "CATALOG_TITLE_MAX": gamesdb.CATALOG_TITLE_MAX,
        "CATALOG_DESC_MAX": gamesdb.CATALOG_DESC_MAX,
        "CATALOG_LINE_MAX": gamesdb.CATALOG_LINE_MAX,
        "CATALOG_MAX": gamesdb.CATALOG_MAX,
    }


# --- validate -------------------------------------------------------------


def test_conforming_library_has_no_problems():
    problems, notices = gamesdb.validate(FIXTURE_GAMES)
    assert problems == []
    assert any("ROM existence not checked" in notice for notice in notices)
    assert any(
        "art, shot and manual existence not checked" in notice for notice in notices
    )
    assert "6 entries have no cover source" in notices
    assert "6 entries have no snapshot source" in notices
    assert "6 entries have no manual source" in notices


def test_notices_count_only_the_entries_with_no_manual():
    entries = [
        game(filename="A.gb", manual="manuals/A.pdf"),
        game(filename="B.gb"),
        game(filename="C.gb"),
    ]
    _, notices = gamesdb.validate(entries)
    assert "2 entries have no manual source" in notices


def test_top_level_object_is_a_problem_not_a_crash():
    problems, _ = gamesdb.validate({"games": []})
    assert problems == ["games.json must be a top-level array, not a dict"]


def test_entry_that_is_not_an_object_is_reported():
    problems, _ = gamesdb.validate(["Tetris.gb"])
    assert problems == ["entry 0 is not an object, it is a str"]


def test_missing_field_is_reported_by_name():
    entry = game()
    del entry["genre"]
    assert rejected(entry, "genre is missing")


def test_missing_manual_is_reported_by_name():
    entry = game()
    del entry["manual"]
    assert rejected(entry, "manual is missing")


def test_problems_name_the_entry():
    assert all(
        problem.startswith("Tetris.gb: ")
        for problem in problems_for(game(title=""))
    )


def test_filename_without_the_gb_suffix_is_rejected():
    assert rejected(game(filename="Tetris.gbc"), "does not end in .gb")


@pytest.mark.parametrize(
    "filename",
    ["Snow Bros. Jr..gb", "Dr. Mario.gb", "Super R.C. Pro-Am.gb", "Mr. Do!.gb"],
)
def test_dotted_library_names_are_accepted(filename):
    assert problems_for(game(filename=filename)) == []


def test_filename_at_the_cap_is_accepted_and_one_over_is_rejected():
    accepted = "a" * 60 + ".gb"
    assert gamesdb.byte_len(accepted) == 63
    assert problems_for(game(filename=accepted)) == []

    too_long = "a" * 61 + ".gb"
    assert gamesdb.byte_len(too_long) == 64
    assert rejected(game(filename=too_long), "filename is 64 bytes, the cap is 63")


def test_duplicate_filename_is_rejected():
    problems, _ = gamesdb.validate([game(), game(title="Tetris DX")])
    assert problems == ["Tetris.gb: filename duplicates entry 0"]


def test_filename_with_a_path_separator_is_rejected():
    assert rejected(game(filename="gb/Tetris.gb"), "contains a path separator")


def test_empty_filename_is_rejected():
    assert rejected(game(filename=""), "filename is empty")


def test_title_at_the_cap_is_accepted_and_one_over_is_rejected():
    assert problems_for(game(title="t" * 47)) == []
    assert rejected(game(title="t" * 48), "title is 48 bytes, the cap is 47")


def test_description_at_the_cap_and_empty_are_accepted():
    assert problems_for(game(description="d" * 200)) == []
    assert problems_for(game(description="")) == []
    assert rejected(
        game(description="d" * 201), "description is 201 bytes, the cap is 200"
    )


@pytest.mark.parametrize("field", ["title", "description"])
def test_non_ascii_is_rejected(field):
    assert rejected(game(**{field: "Pokemon Rouge édition"}), "not plain ASCII")


@pytest.mark.parametrize("field", ["filename", "title", "description"])
@pytest.mark.parametrize(
    ("character", "message"), [("\t", "contains a tab"), ("\n", "contains a newline")]
)
def test_tab_and_newline_are_rejected(field, character, message):
    value = f"a{character}b.gb" if field == "filename" else f"a{character}b"
    assert rejected(game(**{field: value}), message)


def test_players_range_string_is_rejected_and_an_integer_or_null_accepted():
    assert rejected(
        game(players="1-2"), "players must be an integer or null, not str"
    )
    assert problems_for(game(players=2)) == []
    assert problems_for(game(players=None)) == []


def test_year_string_is_rejected():
    assert rejected(game(year="1990"), "year must be an integer or null, not str")


def test_boolean_is_not_an_integer_for_year_or_players():
    assert rejected(game(players=True), "players must be an integer or null, not bool")


def test_starter_must_be_a_bool():
    assert rejected(game(starter="yes"), "starter must be true or false, not str")


@pytest.mark.parametrize("field", ["art", "shot", "manual"])
def test_media_paths_must_be_strings(field):
    assert rejected(game(**{field: None}), f"{field} must be a string, not NoneType")


@pytest.mark.parametrize("field", ["developer", "publisher", "genre"])
def test_recorded_fields_must_be_strings(field):
    assert rejected(game(**{field: 1989}), f"{field} must be a string, not int")


def test_more_entries_than_the_catalog_holds_is_rejected():
    at_cap = [game(filename=f"g{index:04d}.gb") for index in range(160)]
    problems, _ = gamesdb.validate(at_cap)
    assert problems == []

    over_cap = [game(filename=f"g{index:04d}.gb") for index in range(161)]
    problems, _ = gamesdb.validate(over_cap)
    assert "161 entries, but the catalog holds at most 160" in problems


def test_emitted_line_over_the_cap_is_rejected():
    # 4 + 1 + 1 + 1 + 0 + 1 + 376 = 384 bytes, one over what the reader holds.
    entry = game(filename="A.gb", title="T", description="d" * 376)
    assert gamesdb.byte_len(gamesdb.catalog_line(entry)) == 384
    assert rejected(entry, "its catalog line is 384 bytes, the cap is 383")


def test_rom_existence_is_checked_when_the_directory_is_given(tmp_path):
    problems = problems_for(game(), rom_dir=tmp_path)
    assert problems == [f"Tetris.gb: no ROM at {tmp_path / 'Tetris.gb'}"]

    (tmp_path / "Tetris.gb").write_bytes(b"\x00")
    assert problems_for(game(), rom_dir=tmp_path) == []


def test_rom_notice_is_absent_when_the_directory_is_given(tmp_path):
    _, notices = gamesdb.validate([game()], rom_dir=tmp_path)
    assert not any("ROM existence not checked" in notice for notice in notices)


def test_media_existence_is_checked_only_for_a_non_empty_path(tmp_path):
    assert problems_for(game(art="", shot=""), media_dir=tmp_path) == []

    problems = problems_for(game(art="covers/Tetris.png"), media_dir=tmp_path)
    assert problems == [
        f"Tetris.gb: no art source at {tmp_path / 'covers/Tetris.png'}"
    ]

    cover = tmp_path / "covers" / "Tetris.png"
    cover.parent.mkdir()
    cover.write_bytes(b"\x89PNG")
    assert problems_for(game(art="covers/Tetris.png"), media_dir=tmp_path) == []


def test_manual_existence_is_checked_only_when_the_directory_is_given(tmp_path):
    entry = game(manual="manuals/Tetris.pdf")
    assert problems_for(entry) == []

    assert problems_for(entry, media_dir=tmp_path) == [
        f"Tetris.gb: no manual source at {tmp_path / 'manuals/Tetris.pdf'}"
    ]

    manual = tmp_path / "manuals" / "Tetris.pdf"
    manual.parent.mkdir()
    manual.write_bytes(b"%PDF")
    assert problems_for(entry, media_dir=tmp_path) == []


# --- emission -------------------------------------------------------------


def test_stem_strips_only_the_gb_suffix():
    assert gamesdb.stem("Snow Bros. Jr..gb") == "Snow Bros. Jr."
    assert gamesdb.stem("Mr. Do!.gb") == "Mr. Do!"


def test_catalog_line_of_a_starter_carries_the_flag():
    line = gamesdb.catalog_line(game(starter=True))
    assert line == (
        "Tetris.gb\tTetris\tstarter\tFit the falling blocks into complete rows."
    )


def test_catalog_line_of_a_non_starter_has_an_empty_flags_field():
    line = gamesdb.catalog_line(game(starter=False))
    assert line == (
        "Tetris.gb\tTetris\t\tFit the falling blocks into complete rows."
    )
    assert line.count("\t") == 3


def test_emit_catalog_reproduces_the_hand_fixture_byte_for_byte(repo_root):
    fixture = repo_root / "test" / "fixtures" / "catalog.txt"
    assert gamesdb.emit_catalog(FIXTURE_GAMES).encode("ascii") == fixture.read_bytes()


def test_emit_catalog_ends_in_one_newline_and_carries_no_carriage_return():
    text = gamesdb.emit_catalog(FIXTURE_GAMES)
    assert "\r" not in text
    assert text.endswith("\n")
    assert not text.endswith("\n\n")


# --- the mirror parser ----------------------------------------------------


def test_parse_catalog_line_needs_three_tabs():
    with pytest.raises(ValueError, match="it needs three"):
        gamesdb.parse_catalog_line("Tetris.gb\tTetris\tstarter")


def test_description_keeps_a_fourth_tab():
    entry = gamesdb.parse_catalog_line("Tetris.gb\tTetris\t\tone\ttwo")
    assert entry["description"] == "one\ttwo"


def test_trailing_carriage_return_is_stripped():
    entry = gamesdb.parse_catalog_line("Tetris.gb\tTetris\t\tblocks\r")
    assert entry["description"] == "blocks"


def test_parse_catalog_line_rejects_an_over_long_filename():
    filename = "a" * 61 + ".gb"
    with pytest.raises(ValueError, match="filename is 64 bytes"):
        gamesdb.parse_catalog_line(f"{filename}\tTetris\t\t")


def test_parse_catalog_line_rejects_an_empty_filename():
    with pytest.raises(ValueError, match="empty filename"):
        gamesdb.parse_catalog_line("\tTetris\t\t")


def test_parse_catalog_line_rejects_an_over_long_title():
    with pytest.raises(ValueError, match="title is 48 bytes"):
        gamesdb.parse_catalog_line(f"Tetris.gb\t{'t' * 48}\t\t")


def test_unknown_flag_tokens_are_ignored():
    assert gamesdb.parse_catalog_line("Tetris.gb\tTetris\twibble\t")["starter"] is False
    assert (
        gamesdb.parse_catalog_line("Tetris.gb\tTetris\twibble,starter\t")["starter"]
        is True
    )


def test_parse_catalog_skips_empty_lines_including_the_trailing_one():
    text = "Tetris.gb\tTetris\t\t\n\nAlleyway.gb\tAlleyway\t\t\n"
    entries = gamesdb.parse_catalog(text)
    assert [entry["filename"] for entry in entries] == ["Tetris.gb", "Alleyway.gb"]


def test_parse_catalog_rejects_a_line_that_fills_the_readers_buffer():
    line = "A.gb\tT\t\t" + "d" * 376
    assert gamesdb.byte_len(line) == 384
    with pytest.raises(ValueError, match="line is 384 bytes"):
        gamesdb.parse_catalog(line + "\n")


def test_round_trip_of_a_conforming_library_has_no_mismatches():
    assert gamesdb.round_trip(FIXTURE_GAMES) == []
