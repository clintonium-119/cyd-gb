#pragma once

#include <stdint.h>
#include <stddef.h>

// Minimal PN532 driver over the shared I²C bus (hw_config.h: PN532_I2C_ADDR).
//
// This module moves bytes and nothing else. It knows how to frame a PN532
// command, wait for the chip, and unpack the answer; it knows nothing about
// NTAG pages, passwords or NDEF — lib/gbcore/cart/ntag.c composes every tag
// command and calls back through nfc_transceive(), which is shaped exactly as
// gbcore's ntag_xcv_fn so the host-tested layer runs unchanged on the device.
//
// Read once at boot, before display and SD init, never during emulation:
// nfc_detect() blocks for up to NFC_DETECT_TIMEOUT_MS while the PN532 looks
// for a target, and the bus-sharing contract in i2c_bus.h assumes the tag read
// is over before the per-frame button poll starts. CN1 carries no IRQ line,
// so readiness is polled through the I²C status byte.
//
// No NFC library is used. PWD_AUTH needs a raw InDataExchange that the
// Adafruit driver does not expose cleanly, and that driver drags in the SPI
// and HSU transports this board never uses.

// Bring the bus and the chip up: i2c_bus_init(), wake the PN532, read and log
// its firmware version, SAMConfiguration to normal mode, and bound the tag
// search so one nfc_detect() returns inside NFC_DETECT_TIMEOUT_MS. False when
// the chip does not answer; the caller treats that as "reader missing".
bool nfc_init();

enum nfc_detect_e {
    NFC_DETECT_NONE,   // no tag in the field
    NFC_DETECT_ONE,    // exactly one tag, selected and ready for transceive
    NFC_DETECT_MULTI,  // two tags — the caller halts, it never picks one
    NFC_DETECT_ERR,    // the reader did not answer or answered nonsense
};

// One InListPassiveTarget with MaxTg = 2 at 106 kbps type A. On ONE, the
// tag's UID is copied to uid (up to 7 bytes, NTAG215's length) and its length
// to *uid_len — for display only; nothing here or upstream stores it. On MULTI
// nothing is copied. Either pointer may be NULL.
enum nfc_detect_e nfc_detect(uint8_t uid[7], uint8_t* uid_len);

// The ntag_xcv_fn: InDataExchange with the selected target. Returns
// NTAG_XCV_OK when the PN532 reports status 0x00, NTAG_XCV_NAK when it
// reports 0x01 (a timeout, which is how a Type 2 NAK or a silent tag
// surfaces), and NTAG_XCV_IO for anything else — another status, a missing
// ACK, a checksum error or a ready timeout. ctx is unused.
int nfc_transceive(void* ctx, const uint8_t* tx, size_t tx_len,
                   uint8_t* rx, size_t rx_cap, size_t* rx_len);

// GetFirmwareVersion's four bytes packed IC << 24 | Ver << 16 | Rev << 8 |
// Support, 0 before a successful nfc_init(). For the WS-09 inspector.
uint32_t nfc_firmware_version();
