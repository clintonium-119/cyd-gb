# CYD-GB

A Game Boy (DMG) emulator for the **ESP32-2432S024** — the 2.4" variant of the "Cheap Yellow Display"
board. Ten of these are being built into original Game Boy shells, with physical buttons and games chosen
by inserting an NFC-tagged cartridge. It replaces the upstream touchscreen fork's on-screen controls and
ROM browser: on the finished units there is no way to pick a game from the device.

## Status

In progress, and still ahead of the hardware. The cartridge reader, the button driver, the landscape
renderer, the menu and saves, and the on-device cartridge writer have landed; the host tooling that images
the SD cards and flashes the boards is built. Audio and the diagnostics screen are next, and a set of
items are parked until there is a board on the bench. See [`ROADMAP.md`](ROADMAP.md) for what is built,
what is next, and what is waiting on the bench.

## Hardware

Full pin map and part choices are in [`reference/ORIGINAL_ROADMAP.md`](reference/ORIGINAL_ROADMAP.md) §1;
the pins the firmware actually declares are in [`include/hw_config.h`](include/hw_config.h).

| | |
|---|---|
| Board | ESP32-2432S024 (ESP32-D0WD-V3, 4 MB flash, no PSRAM) |
| Panel | ST7789 240×320 SPI, backlight on IO21 |
| SD card | onboard slot on IO5 / IO18 / IO19 / IO23 |
| Buttons | 8-way PCB via an MCP23017 expander at I²C 0x20, polled once per frame |
| Cartridges | PN532 NFC reader at I²C 0x24 — *planned* |
| Audio | onboard amp on the DAC (IO26) — bench-verified; no amp-enable pin exists, playback lands with the audio workstream |
| I²C bus | SDA IO22, SCL IO27 — the whole bus, power included, on the 4-pin CN1 plug (bench-verified) |
| Power | 3.7 V LiPo with integrated protection, charged through the board's own charger |

## Building

```sh
pio run -e cyd            # build
pio run -e cyd -t upload  # flash
pio device monitor        # serial, 115200
```

## SD card

FAT32, and every card is identical — a traded cartridge has to work in any unit. The firmware expects:

```
/roms/gb/<filename>        the ROM, named exactly as games.json says
/art/<stem>.565            box art, 96x96 raw RGB565, little-endian
/shot/<stem>.565           gameplay snapshot, same format
/saves/<stem>.sav          battery save, written by the emulator
/catalog.txt               generated; never hand-edited
```

A card is produced by `tools/image_sd.py`, never by hand: it copies the ROMs, converts the art, emits
`/catalog.txt` from `games.json`, removes anything the catalog does not name, and prints a manifest so
two cards can be compared. It never touches `/saves`. The full contract — the caps, the line format and
the tag payload grammar — is in [`docs/CATALOG_FORMAT.md`](docs/CATALOG_FORMAT.md).

## Tools

Host-side, standard library only, run from the project root. The ROMs and the scraped art are private
and are never committed: `CYD_ROM_DIR` names the curated ROM directory and `CYD_MEDIA_DIR` the ES-DE
media directory, and `art`/`shot` paths in `games.json` are relative to the latter.

| | |
|---|---|
| `tools/gamesdb.py` | the shared library: the caps mirrored from the C headers, `games.json` validation, catalog emission, and a parser mirroring the firmware's reader |
| `tools/seed_games_json.py` | one-shot seed of `games.json` from an ES-DE gamelist through `tools/esde_aliases.json`; after it, the file is hand-curated |
| `tools/validate_games.py` | what CI runs; `--strict` before imaging a card, where both directories are present |
| `tools/image_sd.py` | images a card idempotently; `--check` verifies one, `--catalog-only` writes just the catalog |
| `tools/flash.py` | flashes a board from a local build or a tagged release, verifying `SHA256SUMS` first |
| `tools/factory_reset.py` | erases only the `nvs` region, re-arming the first-boot wizard without a reflash |

```sh
python tools/validate_games.py                          # what CI runs
python tools/image_sd.py --target /run/media/you/GB     # image a card
python tools/flash.py --release v0.1.0                  # flash a board
pytest tools/tests                                      # the host test suite
```

## Design docs

- [`reference/ORIGINAL_ROADMAP.md`](reference/ORIGINAL_ROADMAP.md) — the settled design: hardware,
  rendering, audio, the cartridge system, and the open questions that need a bench to answer.
- [`ROADMAP.md`](ROADMAP.md) — the work breakdown: which workstream does what, in what order, and what
  each one defers.
- [`docs/CATALOG_FORMAT.md`](docs/CATALOG_FORMAT.md) — the catalog contract: the `games.json` entry
  schema, the generated `/catalog.txt` line format, the SD layout and art naming, and the cartridge tag
  payload grammar.
- [`docs/ASSEMBLY.md`](docs/ASSEMBLY.md) — the build-day checklist: what to check before the shell goes
  on, what the first boot should do, and how to get a unit back to the wizard.

## Credits

- [Peanut-GB](https://github.com/deltabeard/Peanut-GB) — emulator core by Mahyar Koshkouei. Vendored at
  `include/peanut_gb.h`, pinned to an upstream commit recorded in that file's header; refresh it with
  `scripts/update_peanut_gb.sh <sha>`.
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [artanergin44-collab/cyd-gb](https://github.com/artanergin44-collab/cyd-gb) — the upstream fork this
  started from
- [CYD Community](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) — hardware docs

## License

MIT. Peanut-GB is also MIT, copyright 2018-2023 Mahyar Koshkouei.
