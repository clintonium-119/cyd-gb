"""Tests for tools/image_sd.py — the layout, idempotency, pruning and verify."""

import json
import re
import shutil
import struct
import subprocess
import sys

import lz4.block
import pytest

import gamesdb
import image_sd

ffmpeg_required = pytest.mark.skipif(
    shutil.which("ffmpeg") is None, reason="ffmpeg converts the art and has no substitute"
)
poppler_required = pytest.mark.skipif(
    shutil.which("pdftoppm") is None or shutil.which("pdfinfo") is None,
    reason="poppler renders the manuals and has no substitute",
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
                # Alleyway's is empty, so it gets no /desc file.
                "description": f"{title} is a game." if art else "",
                "art": art,
                "shot": shot,
                "manual": "",
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
    # 3 ROMs + 2 covers (Alleyway has none) + 1 snapshot (Tetris only) + 2
    # descriptions (Alleyway's is empty) + the catalog = 9. /saves holds
    # nothing and is not an expected file.
    assert len(out.strip().splitlines()) == 9
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


# --- manual page encoder --------------------------------------------------


def test_fit_within_fills_the_box_on_the_binding_side():
    # 532 * 1065 / 1440 = 393.46, so the height rounds down to 393.
    assert image_sd.fit_within(1440 / 1065) == (532, 393)
    assert image_sd.fit_within(1.0) == (480, 480)
    assert image_sd.fit_within(0.5) == (240, 480)


def test_a_page_of_exactly_two_to_one_is_not_split():
    assert image_sd.page_outputs(1000, 500) == [(532, 266, 0.532, None)]


def test_a_page_just_wider_than_two_to_one_is_split():
    outputs = image_sd.page_outputs(1005, 500)
    assert [half for _, _, _, half in outputs] == [0, 1]


def test_a_catrap_spread_splits_into_halves_that_each_fit_the_box():
    # Catrap's spreads are about 2.7:1.
    outputs = image_sd.page_outputs(1296, 480)
    assert len(outputs) == 2
    for w, h, scale, _ in outputs:
        assert w <= 532 and h <= 480
        assert w == 532
        assert scale == pytest.approx(532 / 648)


def quantize(pixels):
    """Pixels to their 2 bpp levels, through quantize_page's table for them."""
    grey = bytes(pixels)
    return list(grey.translate(image_sd.quantize_page(grey, len(grey), 1)))


def test_a_page_of_pure_paper_is_all_white_and_pure_ink_all_black():
    assert quantize([255] * 100) == [0] * 100
    assert quantize([0] * 100) == [3] * 100


def test_grey_halfway_between_paper_and_ink_is_a_grey_level():
    # Paper 240, ink 40: 140 is halfway, and must not collapse to either end.
    levels = quantize([240] * 90 + [40] * 9 + [140])
    assert levels[-1] in (1, 2)


def test_light_grey_paper_is_still_white():
    assert quantize([200] * 90 + [20] * 10)[:90] == [0] * 90


def test_a_dark_page_keeps_its_light_box_white():
    # A dark back cover with a small light logo: the logo is the paper point,
    # since the paper search starts at 128, so it stays white.
    levels = quantize([20] * 950 + [200] * 50)
    assert levels[:950] == [3] * 950
    assert levels[950:] == [0] * 50


def test_a_nine_pixel_row_packs_msb_first_at_two_bits_with_zero_padding():
    # Levels 3 0 1 2 | 3 3 0 0 | 1 then three pad pixels.
    table = bytes(range(4)) + bytes(252)
    grey = bytes([3, 0, 1, 2, 3, 3, 0, 0, 1])
    assert image_sd.pack_page_2bpp(grey, 9, 1, table) == bytes(
        [0b11000110, 0b11110000, 0b01000000]
    )


def test_rows_are_padded_independently():
    # Two 3-pixel rows, 3 2 1 and 1 2 3, each in its own byte.
    table = bytes(range(4)) + bytes(252)
    grey = bytes([3, 2, 1, 1, 2, 3])
    assert image_sd.pack_page_2bpp(grey, 3, 2, table) == bytes([0b11100100, 0b01101100])


def decode_manual(data):
    """A reader written from the format alone, to check the writer against.

    Checks every structural rule the C reader enforces and returns each page's
    raster with its bands decompressed and joined.
    """
    magic, version, count = struct.unpack_from("<4sHH", data, 0)
    assert (magic, version) == (b"GBMN", 2)
    sizes = [struct.unpack_from("<HH", data, 8 + 4 * index) for index in range(count)]
    table = 8 + 4 * count
    blocks_start = table + sum(4 * (-(-h // 16) + 1) for _, h in sizes)
    previous_end = blocks_start
    pages = []
    for w, h in sizes:
        bands = -(-h // 16)
        offsets = struct.unpack_from(f"<{bands + 1}I", data, table)
        table += 4 * (bands + 1)
        assert offsets[0] == previous_end
        row_bytes = (w + 3) // 4
        raster = b""
        for band in range(bands):
            rows = min(16, h - 16 * band)
            assert offsets[band] <= offsets[band + 1]
            block = lz4.block.decompress(
                data[offsets[band]:offsets[band + 1]],
                uncompressed_size=rows * row_bytes,
            )
            assert len(block) == rows * row_bytes
            raster += block
        previous_end = offsets[-1]
        pages.append((w, h, raster))
    assert table == blocks_start
    assert previous_end == len(data)
    return pages


def test_encode_manual_lays_out_the_header_sizes_band_tables_and_blocks():
    # A 9x20 page is two bands, 16 rows and a short 4; a 16x1 page is one.
    pages = [(9, 20, bytes(3 * 20)), (16, 1, b"\xff\x01\x02\x03")]
    data = image_sd.encode_manual(pages)

    assert data[:4] == b"GBMN"
    assert data[4:6] == b"\x02\x00"
    assert data[6:8] == b"\x02\x00"
    assert data[8:12] == b"\x09\x00\x14\x00"
    assert data[12:16] == b"\x10\x00\x01\x00"
    # Three offsets for the first page, two for the second, then the blocks.
    first = struct.unpack_from("<3I", data, 16)
    second = struct.unpack_from("<2I", data, 28)
    assert first[0] == 16 + 4 * 5
    assert first[2] == second[0]
    assert second[1] == len(data)


def test_every_band_decompresses_to_exactly_its_rows_including_a_short_last_one():
    raster = bytes(range(3 * 37))
    pages = [(10, 37, raster)]
    assert decode_manual(image_sd.encode_manual(pages)) == pages


def test_encode_manual_refuses_a_raster_of_the_wrong_length():
    with pytest.raises(ValueError):
        image_sd.encode_manual([(9, 2, bytes(5))])


def test_a_two_page_file_decodes_back_to_what_was_encoded():
    table = image_sd.quantize_page(bytes(range(0, 256, 4)), 8, 8)
    first = image_sd.pack_page_2bpp(bytes([0, 255] * 15), 10, 3, table)
    second = image_sd.pack_page_2bpp(bytes(range(0, 256, 4)), 8, 8, table)
    pages = [(10, 3, first), (8, 8, second)]
    assert decode_manual(image_sd.encode_manual(pages)) == pages


def fixture_raster(w, h, level):
    """A raster of level(x, y) at 2 bpp, packed without the encoder's helpers."""
    raster = bytearray()
    for y in range(h):
        row = bytearray((w + 3) // 4)
        for x in range(w):
            row[x // 4] |= level(x, y) << (6 - 2 * (x % 4))
        raster += row
    return bytes(raster)


# The two pages behind test/fixtures/manual_two_pages.2bp, which the C reader's
# suite parses with the same two formulas: a 10x20 page, so its rows carry pad
# bits and its second band is a short 4 rows, using all four levels in
# diagonal stripes that LZ4 matches with overlapping copies; and a 5x3 page.
FIXTURE_MANUAL_PAGES = [
    (10, 20, fixture_raster(10, 20, lambda x, y: (x + y) % 4)),
    (5, 3, fixture_raster(5, 3, lambda x, y: (3 * x + y) % 4)),
]


def test_the_encoder_still_writes_the_c_suites_fixture_byte_for_byte(repo_root):
    fixture = repo_root / "test" / "fixtures" / "manual_two_pages.2bp"
    assert image_sd.encode_manual(FIXTURE_MANUAL_PAGES) == fixture.read_bytes()


def test_the_committed_fixture_decodes_back_to_its_source_rasters(repo_root):
    fixture = repo_root / "test" / "fixtures" / "manual_two_pages.2bp"
    assert decode_manual(fixture.read_bytes()) == FIXTURE_MANUAL_PAGES


def c_define(path, name):
    """A #define's value as written, quotes and all."""
    match = re.search(r"^#define\s+" + name + r"\s+(\S+)", path.read_text(), re.M)
    assert match, f"{path} does not #define {name}"
    return match.group(1)


def test_the_manual_directory_and_suffix_match_the_firmware(repo_root):
    header = repo_root / "include" / "sd_manager.h"
    assert c_define(header, "MANUAL_PATH") == f'"/{image_sd.MANUAL_DIR}"'
    assert c_define(header, "MANUAL_SUFFIX") == f'"{image_sd.MANUAL_SUFFIX}"'


def test_the_description_directory_and_suffix_match_the_firmware(repo_root):
    header = repo_root / "include" / "sd_manager.h"
    assert c_define(header, "DESC_PATH") == f'"/{image_sd.DESC_DIR}"'
    assert c_define(header, "DESC_SUFFIX") == f'"{image_sd.DESC_SUFFIX}"'


def test_the_manual_magic_version_and_band_height_match_the_firmware(repo_root):
    header = repo_root / "lib" / "gbcore" / "ui" / "manual.h"
    assert c_define(header, "MANUAL_MAGIC") == f'"{image_sd.MANUAL_MAGIC.decode()}"'
    assert c_define(header, "MANUAL_VERSION") == str(image_sd.MANUAL_VERSION)
    assert c_define(header, "MANUAL_BAND_ROWS") == str(image_sd.MANUAL_BAND_ROWS)

# --- manual rendering -----------------------------------------------------

# Three fixture pages, in points: a plain page with a black box; a spread wider
# than 2:1 with a black box on each half; and a dark page carrying a lighter
# box, which a fixed threshold would flatten to black.
MANUAL_PAGES = [
    (300, 400, "1 g 0 0 300 400 re f 0 g 50 50 100 100 re f"),
    (1000, 300, "1 g 0 0 1000 300 re f 0 g 100 100 100 100 re f 600 100 100 100 re f"),
    (300, 400, "0.1 g 0 0 300 400 re f 0.3 g 100 150 100 100 re f"),
]


def make_pdf(path, pages):
    """A minimal PDF of filled rectangles, one content stream per page, no fonts."""
    count = len(pages)
    kids = " ".join(f"{3 + 2 * index} 0 R" for index in range(count))
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        f"<< /Type /Pages /Kids [{kids}] /Count {count} >>".encode(),
    ]
    for index, (w, h, content) in enumerate(pages):
        objects.append(
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {w} {h}] "
            f"/Contents {4 + 2 * index} 0 R >>".encode()
        )
        stream = content.encode()
        objects.append(
            b"<< /Length %d >>\nstream\n%s\nendstream" % (len(stream), stream)
        )
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n%s\nendobj\n" % (number, body)
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objects) + 1, xref
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))


@pytest.fixture
def manual_sources(sources):
    """The sources, with a manual for Tetris and none for the other two."""
    make_pdf(sources["media_dir"] / "manuals/Tetris.pdf", MANUAL_PAGES)
    for entry in sources["entries"]:
        if entry["filename"] == "Tetris.gb":
            entry["manual"] = "manuals/Tetris.pdf"
    sources["games"].write_text(
        json.dumps(sources["entries"], indent=2) + "\n", encoding="utf-8"
    )
    return sources


def transitions(w, raster):
    """Changes of level along the rows, the structure quantizing keeps."""
    row_bytes = (w + 3) // 4
    count = 0
    for start in range(0, len(raster), row_bytes):
        row = raster[start:start + row_bytes]
        levels = [(row[x // 4] >> (6 - 2 * (x % 4))) & 3 for x in range(w)]
        count += sum(1 for a, b in zip(levels, levels[1:]) if a != b)
    return count


@ffmpeg_required
@poppler_required
def test_one_run_makes_a_manual_only_for_the_entry_with_one(manual_sources):
    assert image(manual_sources) == 0
    card = manual_sources["card"]
    assert sorted(path.name for path in (card / "manual").iterdir()) == ["Tetris.2bp"]


@ffmpeg_required
@poppler_required
def test_a_manual_is_exactly_the_size_its_table_implies(manual_sources):
    assert image(manual_sources) == 0
    decode_manual((manual_sources["card"] / "manual/Tetris.2bp").read_bytes())


@ffmpeg_required
@poppler_required
def test_the_wide_page_is_stored_as_two_halves_within_the_box(manual_sources):
    assert image(manual_sources) == 0
    pages = decode_manual((manual_sources["card"] / "manual/Tetris.2bp").read_bytes())

    assert [(w, h) for w, h, _ in pages] == [(360, 480), (532, 319), (532, 319), (360, 480)]
    for w, h, raster in pages:
        assert w <= 532 and h <= 480
        assert transitions(w, raster) > 0


@ffmpeg_required
@poppler_required
def test_the_dark_page_keeps_its_light_box(manual_sources):
    assert image(manual_sources) == 0
    pages = decode_manual((manual_sources["card"] / "manual/Tetris.2bp").read_bytes())
    w, h, raster = pages[3]

    # The box is 100 pt tall, 120 rows at this fit, two edges per row. No grey
    # on this page reaches the paper search, so the box is a dark grey on black
    # rather than white, but it is still there; a fixed mid-grey threshold
    # would leave the page black with no edges at all.
    assert transitions(w, raster) >= 200


@ffmpeg_required
@poppler_required
def test_a_second_run_leaves_the_manual_and_the_manifest_alone(manual_sources, capsys):
    assert image(manual_sources) == 0
    first = capsys.readouterr()
    assert "1 manuals written" in first.err

    assert image(manual_sources) == 0
    second = capsys.readouterr()
    assert "0 manuals written" in second.err
    assert second.out == first.out
    assert "manual/Tetris.2bp" in second.out


@ffmpeg_required
@poppler_required
def test_a_stray_manual_is_pruned_and_saves_survive(manual_sources, capsys):
    card = manual_sources["card"]
    (card / "manual").mkdir()
    (card / "manual/Bootleg.2bp").write_bytes(b"GBMN")
    (card / "saves").mkdir()
    (card / "saves/Tetris.sav").write_bytes(b"x")

    assert image(manual_sources) == 0

    assert not (card / "manual/Bootleg.2bp").exists()
    assert (card / "saves/Tetris.sav").is_file()
    assert "removed: manual/Bootleg.2bp" in capsys.readouterr().err


@ffmpeg_required
@poppler_required
def test_a_manual_left_in_the_old_format_is_pruned(manual_sources, capsys):
    card = manual_sources["card"]
    (card / "manual").mkdir()
    (card / "manual/Tetris.1bp").write_bytes(b"GBMN\x01\x00")

    assert image(manual_sources) == 0

    assert not (card / "manual/Tetris.1bp").exists()
    assert (card / "manual/Tetris.2bp").is_file()
    assert "removed: manual/Tetris.1bp" in capsys.readouterr().err


@ffmpeg_required
@poppler_required
def test_check_catches_a_single_corrupted_byte_of_a_manual(manual_sources, capsys):
    assert image(manual_sources) == 0
    capsys.readouterr()

    path = manual_sources["card"] / "manual/Tetris.2bp"
    data = bytearray(path.read_bytes())
    data[len(data) // 2] ^= 0x01
    path.write_bytes(bytes(data))

    assert image(manual_sources, "--check") == 1
    assert "failed verify: differs from its source: manual/Tetris.2bp" in (
        capsys.readouterr().err
    )


@ffmpeg_required
@poppler_required
def test_rendering_a_manual_leaves_no_temp_file(manual_sources):
    assert image(manual_sources) == 0
    assert list(manual_sources["card"].rglob("*.tmp")) == []


def test_pixels_with_whitespace_values_survive_the_pgm_header(monkeypatch):
    # A page whose first pixels are 0x20 and 0x0a, which a whitespace split of
    # the header would swallow.
    pgm = b"P5\n3 1\n255\n\x20\x0a\xff"
    monkeypatch.setattr(
        image_sd.subprocess,
        "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(args, 0, pgm, b""),
    )
    assert image_sd.pdftoppm_grey("x.pdf", 1, 3, 1, None) == b"\x20\x0a\xff"


def test_a_library_with_a_manual_needs_poppler(manual_sources, monkeypatch, capsys):
    real_which = shutil.which
    monkeypatch.setattr(
        image_sd.shutil,
        "which",
        lambda tool: None if tool == "pdftoppm" else real_which(tool) or tool,
    )
    assert image(manual_sources) == 1
    assert "pdftoppm is not on PATH" in capsys.readouterr().err
    assert not (manual_sources["card"] / "roms").exists()


def test_a_library_with_a_manual_needs_lz4(manual_sources, monkeypatch, capsys):
    # A None entry in sys.modules makes the import raise ImportError.
    monkeypatch.setitem(sys.modules, "lz4.block", None)
    assert image(manual_sources) == 1
    assert "the lz4 package is not installed" in capsys.readouterr().err
    assert not (manual_sources["card"] / "roms").exists()


@ffmpeg_required
def test_a_library_with_no_manual_images_without_lz4(sources, monkeypatch):
    monkeypatch.setitem(sys.modules, "lz4.block", None)
    assert image(sources) == 0


# --- descriptions ---------------------------------------------------------


@pytest.fixture
def desc_sources(sources):
    """The three entries, Tetris with a two-paragraph description."""
    entries = sources["entries"]
    entries[0]["description"] = "Fit the blocks.\n\nClear the rows."
    sources["games"].write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    return sources


@ffmpeg_required
def test_one_run_writes_each_description_exactly_and_none_when_empty(desc_sources):
    assert image(desc_sources) == 0
    card = desc_sources["card"]

    assert (card / "desc/Tetris.txt").read_bytes() == b"Fit the blocks.\n\nClear the rows."
    assert (card / "desc/Dr. Mario.txt").read_bytes() == b"Dr. Mario is a game."
    assert not (card / "desc/Alleyway.txt").exists()


@ffmpeg_required
def test_a_second_run_leaves_the_descriptions_alone(desc_sources, capsys):
    assert image(desc_sources) == 0
    first = capsys.readouterr()
    assert "2 descriptions written" in first.err

    assert image(desc_sources) == 0
    second = capsys.readouterr()
    assert "0 descriptions written" in second.err
    assert first.out == second.out
    assert "desc/Tetris.txt" in second.out


@ffmpeg_required
def test_a_stray_description_is_pruned_and_saves_survive(desc_sources, capsys):
    card = desc_sources["card"]
    (card / "desc").mkdir()
    (card / "desc/Bootleg.txt").write_bytes(b"stray")
    (card / "saves").mkdir()
    (card / "saves/Tetris.sav").write_bytes(b"x")

    assert image(desc_sources) == 0

    assert not (card / "desc/Bootleg.txt").exists()
    assert (card / "saves/Tetris.sav").is_file()
    assert "removed: desc/Bootleg.txt" in capsys.readouterr().err


@ffmpeg_required
def test_check_catches_a_single_corrupted_byte_of_a_description(desc_sources, capsys):
    assert image(desc_sources) == 0
    capsys.readouterr()

    path = desc_sources["card"] / "desc/Tetris.txt"
    data = bytearray(path.read_bytes())
    data[3] ^= 0x01
    path.write_bytes(bytes(data))

    assert image(desc_sources, "--check") == 1
    assert "failed verify: differs from its source: desc/Tetris.txt" in (
        capsys.readouterr().err
    )
