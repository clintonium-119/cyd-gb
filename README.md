# CYD-GB

A Game Boy (DMG) emulator for the **ESP32-2432S024** — the 2.4" variant of the "Cheap Yellow Display"
board. Ten of these are being built into original Game Boy shells, with physical buttons and games chosen
by inserting an NFC-tagged cartridge. It replaces the upstream touchscreen fork's on-screen controls and
ROM browser: on the finished units there is no way to pick a game from the device.

## Status

Early. No hardware is on hand yet — every part is on order, so the firmware is being taken as far as it
can go without a board. Right now it builds for the target and loads a single hard-coded test ROM at startup; the
cartridge reader, the button driver for the real expander, audio, and the landscape renderer are all
still ahead. See [`ROADMAP.md`](ROADMAP.md) for what is built, what is next, and what is waiting on the
bench.

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

FAT32. The firmware expects:

```
/roms/gb/     Game Boy ROMs
/saves/       cartridge RAM saves, written on Save from the pause menu
```

Until the cartridge reader lands, `loop()` loads exactly one path — `/roms/gb/test.gb` — and reloads it
on quit. That stub is deliberately not a browser and is not meant to grow into one; it disappears when
NFC cartridge matching arrives.

## Design docs

- [`reference/ORIGINAL_ROADMAP.md`](reference/ORIGINAL_ROADMAP.md) — the settled design: hardware,
  rendering, audio, the cartridge system, and the open questions that need a bench to answer.
- [`ROADMAP.md`](ROADMAP.md) — the work breakdown: which workstream does what, in what order, and what
  each one defers.
- [`docs/CATALOG_FORMAT.md`](docs/CATALOG_FORMAT.md) — the catalog contract: the `games.json` entry
  schema, the generated `/catalog.txt` line format, the SD layout and art naming, and the cartridge tag
  payload grammar.

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
