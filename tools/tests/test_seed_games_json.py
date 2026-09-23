"""Tests for tools/seed_games_json.py — matching, media resolution, text folding."""

import json
import xml.etree.ElementTree as ElementTree

import pytest

import gamesdb
import seed_games_json as seed

GAMELIST_XML = """<?xml version="1.0"?>
<gameList>
  <game>
    <path>./01) All but the Best/Foo.zip</path>
    <name>Foo</name>
    <desc>A foo.</desc>
    <releasedate>19920901T000000</releasedate>
    <developer>Beam Software</developer>
    <publisher>Interplay</publisher>
    <genre>Board game</genre>
    <players>1-2</players>
  </game>
  <game>
    <path>./Foo (USA).zip</path>
    <name>Foo (USA)</name>
    <desc>A foo from the states.</desc>
  </game>
  <game>
    <path>./zebra.zip</path>
    <name>Zebra Deluxe</name>
    <desc>Stripes.</desc>
    <releasedate>19900101T000000</releasedate>
    <players>1</players>
  </game>
  <game>
    <path>./nameless.zip</path>
    <desc>No name element, so it is not in the index.</desc>
  </game>
</gameList>
"""


@pytest.fixture
def gamelist(tmp_path):
    path = tmp_path / "gamelist.xml"
    path.write_text(GAMELIST_XML, encoding="utf-8")
    return seed.load_gamelist(path)


def element(path):
    """A bare <game> carrying only the <path> media resolution reads."""
    game = ElementTree.Element("game")
    child = ElementTree.SubElement(game, "path")
    child.text = path
    return game


# --- inputs ---------------------------------------------------------------


def test_curated_stems_are_sorted_and_lose_only_the_gb_suffix(tmp_path):
    for name in ["Zebra.gb", "Snow Bros. Jr..gb", "Alleyway.gb", "notes.txt"]:
        (tmp_path / name).write_bytes(b"")
    assert seed.curated_stems(tmp_path) == ["Alleyway", "Snow Bros. Jr.", "Zebra"]


def test_load_gamelist_is_keyed_by_name_and_skips_a_nameless_entry(gamelist):
    assert sorted(gamelist) == ["Foo", "Foo (USA)", "Zebra Deluxe"]


def test_load_aliases_of_a_missing_file_is_empty(tmp_path):
    assert seed.load_aliases(tmp_path / "esde_aliases.json") == {}


def test_load_aliases_reads_the_map(tmp_path):
    path = tmp_path / "esde_aliases.json"
    path.write_text(json.dumps({"Zebra": "Zebra Deluxe"}), encoding="utf-8")
    assert seed.load_aliases(path) == {"Zebra": "Zebra Deluxe"}


# --- matching -------------------------------------------------------------


def test_exact_name_match_wins_over_an_alias(gamelist):
    matched, unmatched = match_with(gamelist, ["Foo"], {"Foo": "Foo (USA)"})
    assert unmatched == []
    assert matched["Foo"].findtext("desc") == "A foo."


def test_alias_is_used_when_no_name_matches(gamelist):
    matched, unmatched = match_with(gamelist, ["Zebra"], {"Zebra": "Zebra Deluxe"})
    assert unmatched == []
    assert matched["Zebra"].findtext("desc") == "Stripes."


def test_unmatched_stems_are_reported_and_not_invented(gamelist):
    matched, unmatched = match_with(gamelist, ["Zebra", "Foo"], {})
    assert unmatched == ["Zebra"]
    assert list(matched) == ["Foo"]


def test_an_alias_pointing_at_no_gamelist_name_leaves_the_stem_unmatched(gamelist):
    matched, unmatched = match_with(gamelist, ["Zebra"], {"Zebra": "Typo Deluxe"})
    assert matched == {}
    assert unmatched == ["Zebra"]


def match_with(gamelist, stems, aliases):
    return seed.match(stems, gamelist, aliases)


def test_report_names_every_unmatched_stem_with_suggestions(gamelist, capsys):
    seed.report_unmatched(["Zebra"], gamelist, __import__("sys").stdout)
    out = capsys.readouterr().out
    assert "1 curated stems matched no gamelist <name>" in out
    assert "Zebra" in out
    assert "Zebra Deluxe" in out


# --- media resolution -----------------------------------------------------


def test_media_relpath_resolves_through_the_gamelist_path_subfolder(tmp_path):
    cover = tmp_path / "covers" / "01) All but the Best" / "Foo.png"
    cover.parent.mkdir(parents=True)
    cover.write_bytes(b"\x89PNG")

    game = element("./01) All but the Best/Foo.zip")
    assert (
        seed.media_relpath(game, seed.ART_KIND, tmp_path)
        == "covers/01) All but the Best/Foo.png"
    )


def test_media_relpath_is_empty_when_the_file_is_absent(tmp_path):
    game = element("./01) All but the Best/Foo.zip")
    assert seed.media_relpath(game, seed.ART_KIND, tmp_path) == ""


def test_media_relpath_finds_the_snapshot_under_screenshots(tmp_path):
    shot = tmp_path / "screenshots" / "01) All but the Best" / "Foo.png"
    shot.parent.mkdir(parents=True)
    shot.write_bytes(b"\x89PNG")

    game = element("./01) All but the Best/Foo.zip")
    assert (
        seed.media_relpath(game, seed.SHOT_KIND, tmp_path)
        == "screenshots/01) All but the Best/Foo.png"
    )
    assert seed.media_relpath(game, seed.ART_KIND, tmp_path) == ""


def test_media_relpath_falls_back_to_jpg(tmp_path):
    shot = tmp_path / "screenshots" / "Foo.jpg"
    shot.parent.mkdir(parents=True)
    shot.write_bytes(b"\xff\xd8")

    game = element("./Foo.zip")
    assert seed.media_relpath(game, seed.SHOT_KIND, tmp_path) == "screenshots/Foo.jpg"


def test_media_relpath_of_an_entry_with_no_path_is_empty(tmp_path):
    assert seed.media_relpath(ElementTree.Element("game"), seed.ART_KIND, tmp_path) == ""


def test_build_entries_seeds_the_manual_found_under_manuals(tmp_path):
    manual = tmp_path / "manuals" / "Foo.pdf"
    manual.parent.mkdir(parents=True)
    manual.write_bytes(b"%PDF")

    entries = seed.build_entries({"Foo": element("./Foo.zip")}, tmp_path)
    assert entries[0]["manual"] == "manuals/Foo.pdf"


def test_build_entries_seeds_an_empty_manual_when_there_is_none(tmp_path):
    entries = seed.build_entries({"Foo": element("./Foo.zip")}, tmp_path)
    assert entries[0]["manual"] == ""


@pytest.mark.parametrize("extension", [".png", ".jpg"])
def test_an_image_in_the_manuals_directory_is_not_a_manual(tmp_path, extension):
    image = tmp_path / "manuals" / f"Foo{extension}"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"\x89PNG")

    entries = seed.build_entries({"Foo": element("./Foo.zip")}, tmp_path)
    assert entries[0]["manual"] == ""


# --- text folding ---------------------------------------------------------


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        ("‘quoted’", "'quoted'"),
        ("“quoted”", '"quoted"'),
        ("a — b", "a - b"),
        ("a – b", "a - b"),
        ("wait…", "wait..."),
        ("Pokémon", "Pokemon"),
        ("Rouge édition", "Rouge edition"),
        ("a b", "a b"),
        ("a  \n\t b", "a b"),
        ("  padded  ", "padded"),
    ],
)
def test_normalise_ascii(source, expected):
    assert seed.normalise_ascii(source) == expected


def test_normalise_ascii_leaves_what_it_cannot_fold(seed_text="中"):
    assert seed.normalise_ascii(seed_text) == "中"


def test_text_under_the_cap_is_unchanged():
    text = "Fit the falling blocks into complete rows."
    assert seed.truncate_description(text) == text


def test_truncation_cuts_at_the_last_sentence_end_within_the_cap():
    first = "One of the best puzzle games ever made, and the one that sold it. "
    second = "It has four buttons. "
    tail = "x" * 200
    result = seed.truncate_description(first + second + tail)
    assert result == (first + second).rstrip()
    assert len(result.encode("utf-8")) <= 200


def test_truncation_falls_back_to_a_word_boundary():
    text = " ".join(["word"] * 80)
    result = seed.truncate_description(text)
    assert len(result.encode("utf-8")) <= 200
    assert result.endswith("word")
    assert text.startswith(result)


def test_truncation_counts_bytes_not_characters():
    text = seed.normalise_ascii("Pokémon " * 60)
    result = seed.truncate_description(text)
    assert len(result.encode("utf-8")) <= 200


def test_truncation_of_unfoldable_multibyte_text_still_fits_the_cap():
    result = seed.truncate_description("中" * 200)
    assert len(result.encode("utf-8")) <= 200


@pytest.mark.parametrize(
    ("source", "expected"),
    [("1-2", 2), ("1", 1), ("1-4", 4), ("2", 2), (None, None), ("", None), ("n/a", None)],
)
def test_players_max(source, expected):
    assert seed.players_max(source) == expected


@pytest.mark.parametrize(
    ("source", "expected"),
    [("19920901T000000", 1992), ("19900101T000000", 1990), (None, None), ("", None),
     ("not-a-date", None)],
)
def test_year_of(source, expected):
    assert seed.year_of(source) == expected


# --- entries and output ---------------------------------------------------


def test_build_entries_field_order_matches_the_contract(gamelist, tmp_path):
    entries = seed.build_entries({"Foo": gamelist["Foo"]}, tmp_path)
    assert list(entries[0]) == list(gamesdb.GAME_FIELDS)


def test_build_entries_pulls_the_metadata_and_leaves_curation_alone(gamelist, tmp_path):
    entries = seed.build_entries({"Foo": gamelist["Foo"]}, tmp_path)
    assert entries[0] == {
        "filename": "Foo.gb",
        "title": "Foo",
        "description": "A foo.",
        "art": "",
        "shot": "",
        "manual": "",
        "starter": False,
        "developer": "Beam Software",
        "publisher": "Interplay",
        "year": 1992,
        "genre": "Board game",
        "players": 2,
    }


def test_build_entries_is_sorted_by_display_title_case_insensitively(tmp_path):
    def named(name):
        game = ElementTree.Element("game")
        ElementTree.SubElement(game, "name").text = name
        return game

    # Keyed by stem, but the order must follow the title the list displays —
    # a list sorted by anything other than what it shows reads as unsorted.
    matched = {
        "z-stem": named("apple"),
        "a-stem": named("Banana"),
        "m-stem": named("cherry"),
    }
    entries = seed.build_entries(matched, tmp_path)
    assert [entry["title"] for entry in entries] == ["apple", "Banana", "cherry"]


def test_build_entries_output_passes_validation(gamelist, tmp_path):
    entries = seed.build_entries({"Foo": gamelist["Foo"]}, tmp_path)
    problems, _ = gamesdb.validate(entries)
    assert problems == []


def test_the_same_input_twice_gives_identical_json_text(gamelist, tmp_path):
    matched = {"Foo": gamelist["Foo"], "Zebra": gamelist["Zebra Deluxe"]}
    out = tmp_path / "games.json"
    first = seed.write_games(seed.build_entries(matched, tmp_path), out)
    second = seed.write_games(seed.build_entries(matched, tmp_path), out, force=True)
    assert first == second
    assert first.endswith("\n")
    assert json.loads(first)[0]["title"] == "Foo"


def test_write_games_refuses_to_overwrite_without_force(gamelist, tmp_path):
    out = tmp_path / "games.json"
    seed.write_games([], out)
    with pytest.raises(FileExistsError, match="one-shot"):
        seed.write_games([], out)
    seed.write_games([], out, force=True)


def test_write_games_is_ascii_only(gamelist, tmp_path):
    entries = seed.build_entries({"Foo": gamelist["Foo"]}, tmp_path)
    entries[0]["developer"] = "Björn"
    text = seed.write_games(entries, tmp_path / "games.json")
    assert text.isascii()


def test_a_stem_with_no_gamelist_entry_still_gets_one_bare_entry(gamelist, tmp_path):
    entries = seed.build_entries(
        {"Foo": gamelist["Foo"]}, tmp_path, unmatched=["Swordbird Song"]
    )
    assert [entry["title"] for entry in entries] == ["Foo", "Swordbird Song"]

    bare = entries[1]
    assert list(bare) == list(gamesdb.GAME_FIELDS)
    assert bare == {
        "filename": "Swordbird Song.gb",
        "title": "Swordbird Song",
        "description": "",
        "art": "",
        "shot": "",
        "manual": "",
        "starter": False,
        "developer": "",
        "publisher": "",
        "year": None,
        "genre": "",
        "players": None,
    }
    problems, _ = gamesdb.validate(entries)
    assert problems == []


def test_display_title_comes_from_the_gamelist_name(gamelist):
    assert seed.display_title("Foo", gamelist["Foo (USA)"]) == "Foo (USA)"


def test_display_title_is_normalised_to_ascii(tmp_path):
    game = ElementTree.Element("game")
    ElementTree.SubElement(game, "name").text = "Pokémon Blue Version"
    assert seed.display_title("Pokemon - Blue", game) == "Pokemon Blue Version"


def test_display_title_falls_back_to_the_stem_when_over_the_cap():
    game = ElementTree.Element("game")
    ElementTree.SubElement(game, "name").text = "t" * 48
    assert seed.display_title("TMNT", game) == "TMNT"

    game = ElementTree.Element("game")
    ElementTree.SubElement(game, "name").text = "t" * 47
    assert seed.display_title("TMNT", game) == "t" * 47


def test_display_title_falls_back_to_the_stem_when_the_name_is_absent():
    assert seed.display_title("Swordbird Song", ElementTree.Element("game")) == (
        "Swordbird Song"
    )


def test_the_seed_never_emits_a_title_over_the_cap(gamelist, tmp_path):
    matched = {"F": gamelist["Foo"], "Z": gamelist["Zebra Deluxe"]}
    entries = seed.build_entries(matched, tmp_path, unmatched=["Bare"])
    assert all(
        len(entry["title"].encode("utf-8")) <= gamesdb.CATALOG_TITLE_MAX - 1
        for entry in entries
    )
