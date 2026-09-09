# Build-day assembly checklist

Ten units, assembled once, by kids with an adult at the flashing station. This is the running order and
the checks that catch a mistake while it is still cheap — before a shell is closed, and before a
cartridge is written.

**This is a skeleton.** Every blank marked `____` is a value or an outcome that needs a board on the
bench; WS-11 fills them in from what actually happens. Do not guess them.

## Before the shell

Everything here is reversible. Nothing below is, once the shell is on.

- [ ] **Meter the battery polarity** at the JST plug before it goes anywhere near the board. The
      vendor datasheet has been wrong twice on this board, so measure, do not assume. Red is
      `____ V` on the `____` pin.
- [ ] **Remove the RF shield** from the ESP32 module if the shell will not close over it. Record
      whether it was needed: `____`.
- [ ] **Solder-bridge SW1's two pads.** SW1 is a momentary that latches the battery rail with no off
      path, so an unbridged unit cannot start once it is sealed. The DMG slide switch remains the real
      power switch. Confirm the bridge with continuity before going further — a sealed unit without it
      is a disassembly.
- [ ] **Check switch-on-to-charge.** Confirm the unit charges with the slide switch in the `____`
      position and note whether it charges in the other: `____`.
- [ ] **Flash the board, before the shell goes on.** From a named release, never an unversioned local
      build:

      python tools/flash.py --release ____

      The tool verifies `SHA256SUMS` before it writes anything and refuses a release that does not
      match. Expected duration: `____`.
- [ ] **Image the SD card** and keep its manifest. Every card is identical, so one command per card
      and the manifests must match:

      CYD_ROM_DIR=____ CYD_MEDIA_DIR=____ \
        python tools/image_sd.py --target ____ --manifest ____

      A second run on the same card should copy and convert nothing. Compare each card's manifest to
      the first one; they are byte-identical or the card is wrong.
- [ ] **Confirm the card in the unit** before closing anything: the unit reads `/catalog.txt` and finds
      its entries. Serial at 115200 shows `____`.

## Closing the shell

- [ ] Nothing on this list is confirmed by looking at it. Every check above is done and recorded.
- [ ] Screw pattern and torque: `____`.
- [ ] Button feel through the shell: `____`.
- [ ] Cartridge slot alignment and the reader's range through the closed shell: `____`.

## First boot

Deliberately **after** the shell is closed, because the first-boot wizard exercises the cartridge
reader end to end. If the wizard completes, the reader works at final geometry — which is the thing a
bench test on an open board cannot tell you.

- [ ] **Diagnostics first.** Hold Start+Select while switching on and work through
      [`DIAGNOSTICS.md`](DIAGNOSTICS.md). It reads no cartridge and changes nothing but the window nudge,
      so a fault found here is found before the wizard writes anything. Then power off. Result: `____`.
- [ ] Power on. The wizard runs because NVS is empty on a freshly flashed unit.
- [ ] **MENU cartridge.** The wizard writes it, or adopts one that already carries `MENU`. Result:
      `____`.
- [ ] **Wildcard cartridge.** Written with a `WILD:` prefix. Result: `____`.
- [ ] **Starter cartridges.** The wizard offers the titles flagged `starter` in `games.json`. Write
      the ones this unit's owner picked: `____`.
- [ ] **Finish setup.** The unit boots to a game from a cartridge from here on, and there is no way to
      pick a game from the device.
- [ ] Read range through the closed shell, backlight at full: `____`.

## Recovery

The only route back to the wizard. There is no button combo and no on-device path, by design — the
firmware cannot write a tag except through the menu cartridge, and it cannot re-enter setup except
through this.

    python tools/factory_reset.py --port ____

It erases the `nvs` region only, with the offset and size read from `partitions.csv`.

**It drops:** the wizard flags, so setup runs again; the carts-made list; **any pending cartridge
write, silently**; the palette, frameskip, brightness and volume; the per-unit screen nudge.

**It keeps:** the firmware, so no reflash; the ROM already written into flash; every save on the SD
card and the card's whole contents; and the data already on the kid's cartridges — which is why the
wizard re-adopts an existing MENU and wildcard rather than asking for blanks.

- [ ] After a reset, the next boot enters the wizard and re-adopts the existing carts: `____`.
- [ ] Saves on the card survived: `____`.

## What went wrong

Empty on purpose. WS-11 fills this in from the bench and from build day — what needed doing twice,
what a kid found confusing, what the checklist above should have said.
