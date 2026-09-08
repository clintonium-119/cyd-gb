"""Tests for tools/image_sd.py — the layout, idempotency, pruning and verify."""

import json
import shutil
import subprocess

import pytest

import gamesdb
import image_sd

ffmpeg_required = pytest.mark.skipif(
    shutil.which("ffmpeg") is None, reason="ffmpeg converts the art and has no substitute"
)

ENTRIES = [
    ("Tetris.gb", "Tetris", True, "covers/Tetris.png", "screenshots/Tetris.png"),
    ("Dr. Mario.gb", "Dr. Mario", False, "covers/sub/Dr. Mario.png", ""),
    ("Alleyway.gb", "Alleyway", False, "", ""),
]


def make_png(path, seed):
    """A distinct PNG per game, so a swapped conversion cannot pass unnoticed."""
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi",
         "-i", f"testsrc=size=200x150:rate=1:duration=1,hue=h={seed}",
         "-frames:v", "1", str(path)],
        check=True,
        capture_output=True,
    )


@pytest.fixture
def sources(tmp_path):
    """A ROM directory, a media tree, a games.json and an empty card."""
    rom_dir = tmp_path / "roms"
    media_dir = tmp_path / "media"
    card = tmp_path / "card"
    rom_dir.mkdir()
    card.mkdir()

    games = []
    for index, (filename, title, starter, art, shot) in enumerate(ENTRIES):
        (rom_dir / filename).write_bytes(bytes([index]) * 1024)
        if art and shutil.which("ffmpeg"):
            make_png(media_dir / art, index * 40)
        if shot and shutil.which("ffmpeg"):
            make_png(media_dir / shot, index * 40 + 13)
        games.append(
            {
                "filename": filename,
                "title": title,
                "description": f"{title} is a game.",
                "art": art,
                "shot": shot,
                "starter": starter,
                "developer": "Nintendo",
                "publisher": "Nintendo",
                "year": 1989,
                "genre": "Puzzle",
                "players": 2,
            }
        )

    games_path = tmp_path / "games.json"
    games_path.write_text(json.dumps(games, indent=2) + "\n", encoding="utf-8")
    return {
        "rom_dir": rom_dir,
        "media_dir": media_dir,
        "card": card,
        "games": games_path,
        "entries": games,
    }


def image(sources, *extra):
    return image_sd.main(
        [
            "--target", str(sources["card"]),
            "--games", str(sources["games"]),
            "--rom-dir", str(sources["rom_dir"]),
            "--media-dir", str(sources["media_dir"]),
            *extra,
        ]
    )


# --- the layout -----------------------------------------------------------


@ffmpeg_required
def test_one_run_makes_the_whole_layout(sources, capsys):
    assert image(sources) == 0
    card = sources["card"]

    assert (card / "roms/gb/Tetris.gb").is_file()
    assert (card / "roms/gb/Dr. Mario.gb").is_file()
    assert (card / "roms/gb/Alleyway.gb").is_file()
    assert (card / "art/Tetris.565").is_file()
    assert (card / "shot/Tetris.565").is_file()
    assert (card / "art/Dr. Mario.565").is_file()
    assert not (card / "shot/Dr. Mario.565").exists()
    assert not (card / "art/Alleyway.565").exists()
    assert (card / "catalog.txt").is_file()
    assert (card / "saves").is_dir()
    assert list((card / "saves").iterdir()) == []


@ffmpeg_required
def test_every_565_is_exactly_the_contracts_size(sources):
    assert image(sources) == 0
    for path in sources["card"].rglob("*.565"):
        assert path.stat().st_size == 18432, path


@ffmpeg_required
def test_the_written_catalog_parses_and_matches_the_emitter(sources):
    assert image(sources) == 0
    text = (sources["card"] / "catalog.txt").read_text(encoding="ascii")

    assert text == gamesdb.emit_catalog(sources["entries"])
    parsed = gamesdb.parse_catalog(text)
    assert [entry["filename"] for entry in parsed] == [e[0] for e in ENTRIES]
    assert parsed[0]["starter"] is True
    assert parsed[1]["starter"] is False


@ffmpeg_required
def test_no_temp_file_is_left_behind(sources):
    assert image(sources) == 0
    assert list(sources["card"].rglob("*.tmp")) == []


# --- idempotency ----------------------------------------------------------


@ffmpeg_required
def test_a_second_run_changes_nothing_and_the_manifests_are_identical(sources, capsys):
    assert image(sources) == 0
    first = capsys.readouterr()

    assert image(sources) == 0
    second = capsys.readouterr()

    assert first.out == second.out
    assert "0 ROMs copied, 0 images converted" in second.err
    assert "catalog unchanged" in second.err


@ffmpeg_required
def test_the_manifest_holds_no_absolute_path_and_no_saves_entry(sources, capsys):
    assert image(sources) == 0
    out = capsys.readouterr().out

    assert str(sources["card"]) not in out
    assert "saves" not in out
    # 3 ROMs + 2 covers (Alleyway has none) + 1 snapshot (Tetris only) + the
    # catalog = 7. /saves holds nothing and is not an expected file.
    assert len(out.strip().splitlines()) == 7
    for line in out.strip().splitlines():
        digest, size, path = line.split("  ", 2)
        assert len(digest) == 64
        assert size.isdigit()
        assert not path.startswith("/")


# --- pruning --------------------------------------------------------------


@ffmpeg_required
def test_a_stray_rom_is_pruned_and_a_save_survives(sources, capsys):
    assert image(sources) == 0
    card = sources["card"]

    stray = card / "roms/gb/Bootleg.gb"
    stray.write_bytes(b"\xff" * 16)
    stray_art = card / "art/Bootleg.565"
    stray_art.write_bytes(b"\x00" * 18432)
    save = card / "saves/Tetris.sav"
    save.write_bytes(b"savedata")

    assert image(sources) == 0
    err = capsys.readouterr().err

    assert not stray.exists()
    assert not stray_art.exists()
    assert save.is_file()
    assert save.read_bytes() == b"savedata"
    assert "removed: roms/gb/Bootleg.gb" in err
    assert "removed: art/Bootleg.565" in err


def test_prune_never_removes_a_directory_or_touches_saves(tmp_path):
    card = tmp_path
    (card / "roms/gb/keep").mkdir(parents=True)
    (card / "saves").mkdir()
    (card / "saves/Tetris.sav").write_bytes(b"x")
    (card / "roms/gb/stray.gb").write_bytes(b"x")

    removed = image_sd.prune(card, [])

    assert removed == ["roms/gb/stray.gb"]
    assert (card / "roms/gb/keep").is_dir()
    assert (card / "saves/Tetris.sav").is_file()


# --- verify ---------------------------------------------------------------


@ffmpeg_required
def test_check_catches_a_single_corrupted_byte_of_art(sources, capsys):
    assert image(sources) == 0
    capsys.readouterr()

    art = sources["card"] / "art/Tetris.565"
    data = bytearray(art.read_bytes())
    data[9000] ^= 0xFF
    art.write_bytes(bytes(data))

    assert image(sources, "--check") == 1
    err = capsys.readouterr().err
    assert "failed verify: differs from its source: art/Tetris.565" in err


@ffmpeg_required
def test_check_catches_a_truncated_art_file(sources, capsys):
    assert image(sources) == 0
    capsys.readouterr()

    (sources["card"] / "art/Tetris.565").write_bytes(b"\x00" * 100)

    assert image(sources, "--check") == 1
    assert "is 100 bytes, expected 18432" in capsys.readouterr().err


@ffmpeg_required
def test_check_catches_a_corrupted_rom(sources, capsys):
    assert image(sources) == 0
    capsys.readouterr()

    (sources["card"] / "roms/gb/Tetris.gb").write_bytes(b"\xde" * 1024)

    assert image(sources, "--check") == 1
    assert "differs from its source: roms/gb/Tetris.gb" in capsys.readouterr().err


@ffmpeg_required
def test_check_writes_nothing(sources, capsys):
    assert image(sources) == 0
    before = capsys.readouterr().out
    (sources["card"] / "roms/gb/Bootleg.gb").write_bytes(b"\xff")

    assert image(sources, "--check") == 0
    # --check does not prune, so the stray is still there afterwards.
    assert (sources["card"] / "roms/gb/Bootleg.gb").is_file()
    assert capsys.readouterr().out == before


# --- failing closed -------------------------------------------------------


def test_a_validation_problem_writes_nothing(sources, capsys):
    broken = json.loads(sources["games"].read_text())
    broken[0]["filename"] = "Tetris.gbc"
    sources["games"].write_text(json.dumps(broken), encoding="utf-8")

    assert image(sources) == 1
    assert "nothing was written" in capsys.readouterr().err
    assert list(sources["card"].iterdir()) == []


def test_a_missing_ffmpeg_fails_before_any_write(sources, capsys, monkeypatch):
    monkeypatch.setattr(image_sd.shutil, "which", lambda name: None)

    assert image(sources) == 1
    assert "ffmpeg is not on PATH" in capsys.readouterr().err
    assert list(sources["card"].iterdir()) == []


def test_a_missing_target_is_a_usage_error(sources, capsys):
    assert image_sd.main(["--games", str(sources["games"])]) == 2
    assert "no card" in capsys.readouterr().err


def test_a_target_that_is_not_a_directory_is_a_usage_error(sources, capsys):
    assert image(sources | {"card": sources["card"] / "absent"}) == 2
    assert "no such card directory" in capsys.readouterr().err


# --- catalog-only ---------------------------------------------------------


def test_catalog_only_writes_just_the_catalog(sources, tmp_path, capsys):
    out = tmp_path / "catalog_library.txt"
    status = image_sd.main(
        ["--catalog-only", str(out), "--games", str(sources["games"])]
    )

    assert status == 0
    assert out.read_text(encoding="ascii") == gamesdb.emit_catalog(sources["entries"])
    assert list(sources["card"].iterdir()) == []
    assert list(tmp_path.glob("*.tmp")) == []


def test_catalog_only_refuses_a_file_that_does_not_validate(sources, tmp_path, capsys):
    broken = json.loads(sources["games"].read_text())
    broken[0]["title"] = "t" * 48
    sources["games"].write_text(json.dumps(broken), encoding="utf-8")
    out = tmp_path / "catalog_library.txt"

    assert image_sd.main(["--catalog-only", str(out), "--games", str(sources["games"])]) == 1
    assert not out.exists()


# --- the primitives -------------------------------------------------------


def test_write_atomic_leaves_no_temp_and_replaces_the_old_bytes(tmp_path):
    path = tmp_path / "nested" / "file.bin"
    image_sd.write_atomic(path, b"first")
    assert path.read_bytes() == b"first"

    image_sd.write_atomic(path, b"second")
    assert path.read_bytes() == b"second"
    assert list(tmp_path.rglob("*.tmp")) == []


def test_copy_if_changed_skips_an_identical_file(tmp_path):
    source = tmp_path / "src.gb"
    destination = tmp_path / "dst.gb"
    source.write_bytes(b"rom")

    assert image_sd.copy_if_changed(source, destination) is True
    assert image_sd.copy_if_changed(source, destination) is False

    destination.write_bytes(b"different")
    assert image_sd.copy_if_changed(source, destination) is True
    assert destination.read_bytes() == b"rom"


@ffmpeg_required
def test_convert_565_is_deterministic_and_skips_a_matching_file(tmp_path):
    png = tmp_path / "cover.png"
    make_png(png, 30)
    out = tmp_path / "cover.565"

    assert image_sd.convert_565(png, out) is True
    assert out.stat().st_size == 18432
    first = out.read_bytes()

    assert image_sd.convert_565(png, out) is False
    assert out.read_bytes() == first


@ffmpeg_required
def test_convert_565_letterboxes_rather_than_stretching(tmp_path):
    # A 200x150 source padded onto black leaves black rows top and bottom.
    png = tmp_path / "wide.png"
    make_png(png, 0)
    data = image_sd.ffmpeg_to_565(png)

    assert len(data) == 18432
    assert data[: 96 * 2] == b"\x00" * (96 * 2)
    assert data[-96 * 2 :] == b"\x00" * (96 * 2)


def test_plan_lists_only_the_media_that_games_json_names(sources):
    items = image_sd.plan(sources["entries"], sources["rom_dir"], sources["media_dir"])
    paths = [item.relpath for item in items]

    assert paths.count("catalog.txt") == 1
    assert "art/Alleyway.565" not in paths
    assert "shot/Dr. Mario.565" not in paths
    assert "art/Dr. Mario.565" in paths
    assert sorted(p for p in paths if p.startswith("roms/")) == [
        "roms/gb/Alleyway.gb",
        "roms/gb/Dr. Mario.gb",
        "roms/gb/Tetris.gb",
    ]
