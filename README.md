# CYD-GB

A Game Boy (DMG) emulator for the **ESP32-2432S024** — the 2.4" variant of the "Cheap Yellow Display"
board. Ten of these are being built into original Game Boy shells, with physical buttons and games chosen
by inserting an NFC-tagged cartridge. It replaces the upstream touchscreen fork's on-screen controls and
ROM browser: on the finished units there is no way to pick a game from the device.

## Status

In progress, and still ahead of the hardware. The cartridge reader, the button driver, the landscape
renderer, the menu and saves, and the on-device cartridge writer have landed; the host tooling that images
the SD cards and flashes the boards is built. Audio has landed — the emulated APU reaches the onboard
amplifier — and the boot-combo diagnostics screen has landed. A set of items are parked until there is a
board on the bench, and that bench pass is what is next. See [`ROADMAP.md`](ROADMAP.md) for what is built, what is next, and what is waiting on the bench.

## Hardware

Full pin map and part choices are in [`reference/ORIGINAL_ROADMAP.md`](reference/ORIGINAL_ROADMAP.md) §1;
the pins the firmware actually declares are in [`include/hw_config.h`](include/hw_config.h).

| | |
|---|---|
| Board | ESP32-2432S024 (ESP32-D0WD-V3, 4 MB flash, no PSRAM) |
| Panel | ST7789 240×320 SPI, backlight on IO27 |
| SD card | onboard slot on IO5 / IO18 / IO19 / IO23 |
| Buttons | 8-way PCB via an MCP23017 expander at I²C 0x20, polled once per frame |
| Cartridges | PN532 NFC reader at I²C 0x24 — *planned* |
| Audio | onboard amp on the DAC (IO26) through I2S built-in-DAC DMA; MiniGB APU, summed to mono; four volume states, off parks the DAC at mid-scale (there is no amp-enable pin) |
| I²C bus | SDA IO22, SCL IO21 — the whole bus, power included, on the 4-pin CN1 plug, pins as silkscreened on the board |
| Power | 3.7 V LiPo with integrated protection, charged through the board's own charger |

## Building

```sh
pio run -e cyd                   # build (Peanut-GB core, the default)
pio run -e cyd -t upload         # flash
pio run -e cyd-gnuboy            # build (gnuboy core)
pio run -e cyd-gnuboy -t upload  # flash
pio device monitor               # serial, 115200
```

Two emulator cores are vendored and exactly one is linked per image, so the environment picks the
core. `cyd` builds Peanut-GB (`include/peanut_gb.h`) and is the default. `cyd-gnuboy` builds gnuboy
(`lib/gnuboy/`), which is GPL-2.0-or-later and therefore makes that image GPL-2.0-or-later — see
[`LICENSE`](LICENSE). The two exist side by side so the renderers can be compared on one board with
the core as the only variable; neither has been chosen over the other.

Every `cyd` build stamps a version from `git describe --tags --always --dirty` and a UTC build time into
the firmware, and names the copy in `builds/` from the same two values. Both show on the diagnostic
screen's System page, so a unit in hand can be matched to a commit without a computer.

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

## Diagnostics

Hold **Start + Select while switching the unit on** to enter the diagnostic screen. It is a halt: there is
no way back, and the power switch is the way out. No computer is needed, and no cartridge is read on the
way in.

**Select + Left/Right** moves between eight pages; the bare D-pad belongs to whichever page is up.

| Page | Shows |
|---|---|
| Buttons | All eight switches live, each with the expander pin behind it, so a dead button names its GPA line |
| SD card | Whether the card mounted, how many ROMs it holds, how many catalog entries, and how full it is |
| NFC tag | The reader's firmware, and — on **A** — one cart's UID, `GET_VERSION`, `AUTH0`/`ACCESS`, raw NDEF and class. It reads; it has no way to write |
| Battery | Raw ADC counts, pin millivolts and cell millivolts through the divider |
| Audio | A test tone through the same mixer the emulator uses. **A** toggles it, **Up/Down** step the four volume states — "off" parks the DAC at mid-scale, because the board has no hardware mute |
| Display | Colour bars, a one-pixel border on the window's edge, and a checkerboard pushed through the real scaler so the blend you judge is the blend a game gets. **Up/Down** cycle them |
| Nudge | Moves the game window a pixel at a time with the D-pad, so it sits square behind the shell's bezel. **A** saves to NVS, **B** restores the compile-time default |
| System | Frameskip (**Up/Down**), the firmware version and the UTC build time |

The version and build time come from `scripts/pre_build_info.py`, which stamps `git describe` and a UTC
time into every `cyd` build and names the copy in `builds/` from the same two values — so a unit in hand can
be matched to a commit.

There is no in-game FPS overlay: the once-a-second `[PERF]` serial line is the fps readout.

[`docs/DIAGNOSTICS.md`](docs/DIAGNOSTICS.md) is the one-page checklist to work through on a freshly built
unit.

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
- [gnuboy](https://github.com/ducalex/retro-go) — the second emulator core, taken from retro-go's
  `retro-core/components/gnuboy`. Vendored at `lib/gnuboy/`, pinned to an upstream commit recorded in
  `lib/gnuboy/gnuboy.h`; refresh it with `scripts/update_gnuboy.sh <sha>`. Authorship is in
  `lib/gnuboy/CREDITS`. Only the core is vendored — retro-go's launcher and system layer are not.
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [artanergin44-collab/cyd-gb](https://github.com/artanergin44-collab/cyd-gb) — the upstream fork this
  started from
- [CYD Community](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) — hardware docs

## License

The first-party code is MIT, and Peanut-GB is also MIT, copyright 2018-2023 Mahyar Koshkouei, so a
`cyd` image is MIT. gnuboy is GPL-2.0-or-later, so a `cyd-gnuboy` image is GPL-2.0-or-later. Full
terms and the exact boundary are in [`LICENSE`](LICENSE).
