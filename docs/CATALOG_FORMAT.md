# Catalog and `games.json` contract

The specification that ties three things together: the `games.json` entries kept in this repository, the
`/catalog.txt` the firmware reads from the SD card, and the payload string written on a cartridge tag.
They share one set of length caps and one key — the ROM filename — so they are specified here in one
place rather than in three.

## Purpose and ownership

- **`games.json` is the single source of truth.** It lives in the repository, is hand-curated, and is
  validated in CI.
- **`/catalog.txt` is generated from it** by `tools/image_sd.py` and is **never hand-edited**. Because it
  is generated, the two cannot drift.
- **The firmware never parses `games.json`.** It reads only `/catalog.txt`, with a small pure-C reader
  (`lib/gbcore/cart/catalog.c`) that builds a static index of `{offset, filename, title, flags}` and
  reads a description on demand. No JSON document, no extra flash, no document heap, and bounded static
  memory on a board with no PSRAM. `games.json` can therefore grow fields freely without a firmware
  change.
- Every SD card is **identical** — one canonical library cloned to every unit — because a traded
  cartridge must work in any unit.

The generating tool, its validator and the seeding from ES-DE metadata are WS-10's scope and are not
specified here.

## SD card layout

```
/roms/gb/<filename>        the ROM, named exactly as games.json says
/art/<stem>.565            box art, 96x96 raw RGB565, little-endian
/shot/<stem>.565           gameplay snapshot, same format
/manual/<stem>.1bp         scanned manual, 1 bpp pages behind a page table
/saves/<stem>.sav          battery save, written by the emulator
/catalog.txt               generated; never hand-edited
```

`<stem>` is `<filename>` with the `.gb` extension removed. There is no `/desc/` directory —
descriptions live in the catalog. `games.json` stays in the repository and is **not** copied to the card.

Library facts as of this writing: 132 ROMs, about 30 MB; cards are 128 MB; each image at 96x96 is
18,432 bytes, so two per game is about 4.9 MB in total.

## `games.json` entry

One object per game, in an array. Field order is not significant.

| Field | Type | Constraint |
|---|---|---|
| `filename` | string | **The key.** Unique across the file. Ends in `.gb`. At most 63 bytes (`ROM_STORE_NAME_MAX - 1`). Appears **verbatim** on protected tags. |
| `title` | string | Display name. At most 47 bytes (`CATALOG_TITLE_MAX - 1`). Plain ASCII, no tab and no newline. |
| `description` | string | At most 200 bytes. Plain ASCII, no tab and no newline. May be empty. |
| `art` | string | Path to the cover source, relative to the media directory named by `CYD_MEDIA_DIR`; empty when no source exists. |
| `shot` | string | Path to the gameplay snapshot source, relative to the media directory named by `CYD_MEDIA_DIR`; empty when no source exists. |
| `manual` | string | Path to the scanned PDF manual source, relative to the media directory named by `CYD_MEDIA_DIR`; empty when no source exists. |
| `starter` | bool | Offered during first-boot setup. |
| `developer` | string | For the record; not emitted to the catalog. |
| `publisher` | string | For the record; not emitted to the catalog. |
| `year` | number | Integer or null. For the record; not emitted to the catalog. |
| `genre` | string | For the record; not emitted to the catalog. |
| `players` | number | Integer or null; the upper bound of a range such as ES-DE's `1-2`. For the record; not emitted to the catalog. |

### `filename` is frozen once the first tag is written

The filename on a protected tag is that cartridge's identifier for the life of the cart. There is no UID
anywhere and no per-tag table, which is what keeps cartridges tradeable between units — but it also means
a rename after tags are written is recoverable only by rewriting every affected cartridge through the
menu cart. **Freeze the ROM filenames before writing the first tag.** The longest name in the current
library is 30 bytes, well inside the 63-byte cap.

Several real library names contain dots — `Snow Bros. Jr..gb`, `Dr. Mario.gb`, `Super R.C. Pro-Am.gb`,
`Mr. Do!.gb` — so anything that manipulates a filename must test for the `.gb` **suffix** and never for
the presence of a dot.

## Catalog line

`/catalog.txt` is one entry per line, tab-separated, four fields, in file order:

```
<filename>\t<title>\t<flags>\t<description>\n
```

- **Encoding** plain ASCII. **Line ending** LF. A trailing `\r` is tolerated by the reader but is not
  emitted.
- **Field order is fixed** and there are exactly three tabs. A line with fewer than three tabs is
  rejected; tabs after the third are part of the description.
- **`flags`** is a comma-separated token list, and may be empty. Only `starter` is understood today.
  **Unknown tokens are ignored by the firmware**, so the generator may add more without a firmware
  change.
- **`description`** runs to the end of the line and may be empty.
- **Line length** at most 383 bytes (`CATALOG_LINE_MAX - 1`) including the three tabs but not the
  newline. The caps above put the worst permitted line at 321 bytes.
- **At most 160 entries** (`CATALOG_MAX`). A longer file is read up to that limit and reported as full.
- **File order is display order.** The writer's list, and "the first `starter`", both follow it.
- An empty line is not an entry — including the one a trailing newline leaves at the end of the file.
- A line whose `filename` is empty or over its cap, or whose `title` is over its cap, is **rejected
  rather than truncated**: a truncated name would match the wrong ROM.

Example, with tabs shown as `→`:

```
Tetris.gb→Tetris→starter→Fit the falling blocks into complete rows.
Alleyway.gb→Alleyway→→
```

## Art

Two images per game, both in the same format and both optional. The writer's detail page shows the box
art beside the description and the gameplay snapshot below it, so a kid choosing a game sees the cover
that identifies it and a frame of what playing it looks like.

- 96x96 pixels, raw RGB565, **little-endian**, no header: exactly 18,432 bytes.
- Named `<stem>.565`, the ROM filename without its `.gb`, under `/art` for the box art and `/shot` for
  the gameplay snapshot.
- Pre-converted during imaging, never decoded on the device — PNG decoding on the ESP32 is slow and
  heap-hungry. Loading is one `fread` into a static buffer and one `pushImage`.
- Little-endian is the same byte order the frame path pushes with `setSwapBytes(true)`, so one push
  configuration serves both art and game frames.
- Sources are RGBA; flatten onto a solid background before converting:

```
ffmpeg -i tetris.png -vf "scale=96:96:force_original_aspect_ratio=decrease,pad=96:96:-1:-1:color=black,format=rgb24" \
       -f rawvideo -pix_fmt rgb565le /art/Tetris.565
```

The imaging tool runs that command twice per game — the `covers` source to `/art` and the
`screenshots` source to `/shot` — the source and the output path being the only difference. The ES-DE
media set the imaging tool seeds from carries `covers` and `screenshots` for the same stems, so the two
directories gain and lose games together.

- Either file missing draws the same placeholder. It is never a failure, and one image present with the
  other absent is an ordinary case.

## Manuals

One file per game that has a manual, `/manual/<stem>.1bp`, rendered from the `manual` PDF during
imaging. A game without one has no file, and the in-game menu offers no manual for it.

Every integer is little-endian. The file is, in order:

| Bytes | Field |
|---|---|
| 4 | Magic `GBMN`. |
| 2 | Version, `1`. |
| 2 | Page count `n`. |
| 4 × `n` | Per page, a `u16` width then a `u16` height, in pixels. |
| Σ `ceil(w / 8) × h` | Every page's raster, back to back, in page order. |

- A raster is `h` rows top to bottom. Each row is `ceil(w / 8)` bytes, pixels **most-significant bit
  first**; a set bit is black, a clear bit white. The pad bits at the end of a row are zero.
- A page's offset is `8 + 4n` plus the raster sizes of the pages before it; nothing else is stored.
- **Exact-size rule:** a file whose size is not exactly `8 + 4n + Σ ceil(w / 8) × h` is refused, the same
  rule a `.565` file meets. Uncompressed on purpose, so the reader seeks straight to the rows it shows.
- Every page fits within **532 × 480**, twice the game window in each direction, at its own aspect: a tile
  of it is one window, and it decimates 2× to an overview of the whole page.
- A source page wider than **2:1** is a spread, and is stored as two pages — its left half, then its right
  — each fitted within 532 × 480 on its own aspect.
- Each page is reduced to one bit at its own Otsu threshold from its grey histogram, a pixel being black
  when its grey level is at or below it. No fixed threshold is used.

The imaging tool renders each page with poppler's `pdftoppm -gray` directly at its stored size, a half
by cropping a render at twice that width. The library as imaged on 2026-09-23: 114 manuals, 2,680
stored pages, 70.7 MB.

## Tag payload grammar

A cartridge carries an NDEF Text record whose payload is one of:

| Payload | Meaning |
|---|---|
| `MENU` | The menu cartridge. Boots the writer. **Never a write target.** |
| `WILD:<filename>` | The wildcard. Loads `<filename>`; the default target of a pending write. |
| `<filename>` | An ordinary game cartridge. Loads `<filename>`. |
| *(blank)* | An empty NDEF message (`03 00 FE`) **or** an all-zero user area. Both are blank, and both are valid write targets. |

- `MENU` is matched **exactly**. A payload of `menu` is a filename, not the menu cart.
- `WILD:` requires a non-empty remainder. A bare `WILD:` names no game and is treated as unreadable — and
  what was actually read is what gets displayed, because "Not found: tetrsi.gb" is fixable and a black
  screen is not.
- `<filename>` is matched against the ROM directory **exactly and case-sensitively**, because the tag was
  written from this catalog's own filename. A normaliser and substring search exist only as a legacy path
  for tags hand-written from a phone before the device could write them.
- The wildcard is identified by its `WILD:` prefix alone. No UID is stored anywhere.

### Record framing

```
03 <mlen> D1 01 <plen> 54 02 65 6E <payload...> FE  [00 pad to a 4-byte page]
```

- When the **device** composes a record it always writes language code `en`, two bytes, status byte
  `0x02`.
- When **reading**, the language-code length is taken from the low 6 bits of the status byte and is
  **never assumed to be 2**. A phone sets its own code from its locale, so a tag hand-written from a
  French device carries `fr-CA` and assuming two bytes would turn the filename into `CATetris.gb`.
- UTF-16 text is refused rather than interpreted, so a misread can never become a filename.

## Caps

Every cap in this document is a named constant in the firmware. The generator must enforce the same
values.

| Cap | Value | Constant | Header |
|---|---|---|---|
| `filename` length, with NUL | 64 | `ROM_STORE_NAME_MAX` | `lib/gbcore/cart/rom_store.h` |
| `title` length, with NUL | 48 | `CATALOG_TITLE_MAX` | `lib/gbcore/cart/catalog.h` |
| `description` length, with NUL | 201 | `CATALOG_DESC_MAX` | `lib/gbcore/cart/catalog.h` |
| Catalog line length, with NUL | 384 | `CATALOG_LINE_MAX` | `lib/gbcore/cart/catalog.h` |
| Catalog entries | 160 | `CATALOG_MAX` | `lib/gbcore/cart/catalog.h` |
| `starter` flag bit | `0x01` | `CATALOG_FLAG_STARTER` | `lib/gbcore/cart/catalog.h` |
| On-tag NDEF region | 96 | `NDEF_BUF_MAX` | `lib/gbcore/cart/ndef.h` |
| Longest composable payload | 84 | `NDEF_TEXT_MAX` | `lib/gbcore/cart/ndef.h` |

The two length caps are stated in this document as the usable byte count, one less than the constant,
because each constant includes room for the NUL.

## Fixture

There are two, and `test/test_catalog/` reads both.

- **`test/fixtures/catalog.txt`** is hand-written: six lines covering a flagged entry, the dotted
  library names, and an entry with an empty description. It exists for the format's edge cases, which
  the real library does not necessarily contain. It is **never edited** — the six-entry assertions in
  `test/test_catalog/test_main.c` depend on its exact contents.
- **`test/fixtures/catalog_library.txt`** is generated from `games.json`, and is the exact bytes a card
  carries:

  ```
  python tools/image_sd.py --catalog-only test/fixtures/catalog_library.txt
  ```

  It is **never hand-edited**, and it is regenerated in the same commit as any change to `games.json`:
  `tools/tests/test_catalog_fixture.py` re-emits the catalog and requires byte-identical output, so a
  stale fixture fails CI. The reader's cases over it derive every expectation from the file — the entry
  count, the descriptions and the starter set are all curated in `games.json`, so none of them is
  hard-coded in a test.
