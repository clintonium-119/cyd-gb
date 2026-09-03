#include "nfc_cart.h"
#include "hw_config.h"
#include "i2c_bus.h"
#include "cart/ntag.h"
#include <Arduino.h>
#include <Wire.h>

// PN532 over I²C, host-controller side only. Frame formats, command codes
// and status codes below are from the NXP PN532 User Manual, UM0701-02
// (Rev. 02, 2007): §6.2.1 for the normal information frame, §6.2.1.3 for the
// ACK frame, §6.2.1.5 for the application error frame, §6.2.4 for the I²C
// status byte, §7.1 for error codes and §7.2 / §7.3 for the commands.
//
// Every exchange is: write one command frame, read the 6-byte ACK, then poll
// the ready bit and read one response frame. The chip answers every command
// but InListPassiveTarget within a few milliseconds; that one blocks while
// the RF search runs, which is why nfc_detect() waits longer for its response
// than for anything else.

// ---- frame constants ------------------------------------------------------

#define PN532_PREAMBLE  0x00
#define PN532_START1    0x00
#define PN532_START2    0xFF
#define PN532_POSTAMBLE 0x00
#define PN532_TFI_HOST  0xD4   // host → PN532
#define PN532_TFI_CHIP  0xD5   // PN532 → host

// Big enough for the largest frame this driver ever moves: an
// InListPassiveTarget answer describing two type A targets (~40 bytes) or an
// InDataExchange carrying a 16-byte READ (26 bytes). Reads pull this many
// bytes plus the status byte in one transaction, which fits Wire's 128-byte
// buffer on arduino-esp32 (I2C_BUFFER_LENGTH in Wire.h).
#define PN532_FRAME_MAX 64

// Command codes (UM0701-02 §7).
#define PN532_CMD_GET_FIRMWARE_VERSION 0x02
#define PN532_CMD_SAM_CONFIGURATION    0x14
#define PN532_CMD_RF_CONFIGURATION     0x32
#define PN532_CMD_IN_DATA_EXCHANGE     0x40
#define PN532_CMD_IN_LIST_PASSIVE_TARGET 0x4A

// InDataExchange status byte, low six bits (§7.1). 0x01 is the RF timeout —
// the target did not answer, or answered a 4-bit NAK the CIU cannot frame —
// which is how a Type 2 NAK reaches the host. (verify) on the bench with a
// wrong PWD_AUTH password that this is the status that comes back.
#define PN532_STATUS_OK      0x00
#define PN532_STATUS_TIMEOUT 0x01
#define PN532_STATUS_MASK    0x3F

// RFConfiguration item 5, MaxRetries (§7.3.1): MxRtyATR, MxRtyPSL,
// MxRtyPassiveActivation. The last bounds how long InListPassiveTarget keeps
// looking; 0xFF is "forever", 0x01 is roughly one polling cycle. The value
// below is a starting point, not a measured one: it must bring an empty-field
// detect back inside NFC_DETECT_TIMEOUT_MS, and the real figure comes off the
// bench.
#define PN532_MX_RTY_PASSIVE_ACTIVATION 0x14

// InListPassiveTarget: two targets at most, 106 kbps ISO14443A (§7.3.5).
#define PN532_MAX_TG 2
#define PN532_BRTY_106A 0x00

static uint8_t frame[PN532_FRAME_MAX + 1];  // +1: the I²C status byte
static uint32_t fw_version = 0;

// ---- I²C primitives -------------------------------------------------------

// The first byte of every I²C read from the PN532 is a status byte whose bit 0
// says whether a frame is waiting (§6.2.4). Poll it, cheaply, until set.
static bool pn532_wait_ready(uint32_t timeout_ms) {
    uint32_t start = millis();
    for (;;) {
        Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)1);
        if (Wire.available() >= 1) {
            uint8_t status = Wire.read();
            if (status & 0x01) {
                return true;
            }
        }
        if (millis() - start >= timeout_ms) {
            return false;
        }
        delay(1);
    }
}

// Build 00 00 FF LEN LCS D4 <body> DCS 00 and write it in one transaction.
// The longest command here is an InDataExchange carrying a 16-byte payload,
// well inside Wire's buffer, so nothing is ever split.
static bool pn532_write_cmd(const uint8_t* body, size_t body_len) {
    if (body_len + 1 > 0xFF || body_len + 8 > sizeof(frame)) {
        return false;
    }

    uint8_t len = (uint8_t)(body_len + 1);   // TFI + body
    uint8_t sum = PN532_TFI_HOST;
    size_t n = 0;

    frame[n++] = PN532_PREAMBLE;
    frame[n++] = PN532_START1;
    frame[n++] = PN532_START2;
    frame[n++] = len;
    frame[n++] = (uint8_t)(0x100 - len);     // LCS: LEN + LCS == 0 mod 256
    frame[n++] = PN532_TFI_HOST;
    for (size_t i = 0; i < body_len; i++) {
        frame[n++] = body[i];
        sum = (uint8_t)(sum + body[i]);
    }
    frame[n++] = (uint8_t)(0x100 - sum);     // DCS: TFI + body + DCS == 0
    frame[n++] = PN532_POSTAMBLE;

    Wire.beginTransmission((uint8_t)PN532_I2C_ADDR);
    Wire.write(frame, n);
    return Wire.endTransmission() == 0;
}

// The chip acknowledges every well-formed command with 00 00 FF 00 FF 00
// before it starts working on it (§6.2.1.3).
static bool pn532_read_ack() {
    if (!pn532_wait_ready(NFC_READY_TIMEOUT_MS)) {
        return false;
    }

    static const uint8_t ack[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
    Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)(1 + sizeof(ack)));
    if (Wire.available() < (int)(1 + sizeof(ack))) {
        return false;
    }
    (void)Wire.read();   // status byte, already known to be ready
    for (size_t i = 0; i < sizeof(ack); i++) {
        if (Wire.read() != ack[i]) {
            return false;
        }
    }
    return true;
}

// Sending an ACK frame from the host aborts the command in progress
// (§6.2.1.3). Used only when a response wait times out, so the chip is not
// left mid-command when the next exchange starts.
static void pn532_abort() {
    static const uint8_t ack[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
    Wire.beginTransmission((uint8_t)PN532_I2C_ADDR);
    Wire.write(ack, sizeof(ack));
    Wire.endTransmission();
}

// Read one response frame for `cmd` and copy its body — the bytes after
// D5 <cmd+1> — into out. Returns the body length, or -1 on a ready timeout,
// a malformed frame, a checksum error, a wrong TFI or command echo, an
// application error frame, or a body larger than out_cap.
//
// The whole frame is pulled in one transaction of a fixed size: the PN532
// clocks out its frame and then padding for as long as the master keeps
// reading, and LEN says where the real bytes stop.
static int pn532_read_response(uint8_t cmd, uint8_t* out, size_t out_cap,
                               uint32_t timeout_ms) {
    if (!pn532_wait_ready(timeout_ms)) {
        pn532_abort();
        return -1;
    }

    Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)sizeof(frame));
    size_t got = 0;
    while (Wire.available() > 0 && got < sizeof(frame)) {
        frame[got++] = (uint8_t)Wire.read();
    }
    if (got < 8) {
        return -1;
    }

    // frame[0] is the status byte; the frame proper starts at [1].
    const uint8_t* f = frame + 1;
    size_t f_len = got - 1;
    if (f[0] != PN532_PREAMBLE || f[1] != PN532_START1 || f[2] != PN532_START2) {
        return -1;
    }

    // Application error frame: 00 00 FF 01 FF 7F 81 00 (§6.2.1.5). The chip
    // could not parse or run the command; for this driver that is a transport
    // failure, never a tag NAK.
    if (f[3] == 0x01 && f[4] == 0xFF && f[5] == 0x7F) {
        return -1;
    }

    uint8_t len = f[3];
    uint8_t lcs = f[4];
    if ((uint8_t)(len + lcs) != 0 || len < 2) {
        return -1;
    }
    if ((size_t)len + 7 > f_len) {   // preamble(3) LEN LCS <len bytes> DCS post
        return -1;
    }

    const uint8_t* data = f + 5;      // TFI, cmd+1, body...
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + data[i]);
    }
    uint8_t dcs = data[len];
    if ((uint8_t)(sum + dcs) != 0) {
        return -1;
    }
    if (data[0] != PN532_TFI_CHIP || data[1] != (uint8_t)(cmd + 1)) {
        return -1;
    }

    size_t body_len = (size_t)len - 2;
    if (body_len > out_cap) {
        return -1;
    }
    for (size_t i = 0; i < body_len; i++) {
        out[i] = data[2 + i];
    }
    return (int)body_len;
}

// One full exchange. body[0] is the command code.
static int pn532_command(const uint8_t* body, size_t body_len, uint8_t* out,
                         size_t out_cap, uint32_t response_timeout_ms) {
    if (!pn532_write_cmd(body, body_len)) {
        return -1;
    }
    if (!pn532_read_ack()) {
        return -1;
    }
    return pn532_read_response(body[0], out, out_cap, response_timeout_ms);
}

// ---- public surface -------------------------------------------------------

bool nfc_init() {
    i2c_bus_init();
    fw_version = 0;

    // Wake. After power-up the PN532 sits in a low-power state and can miss
    // the first command; its I²C address match wakes it, so the first attempt
    // may fail and the second is the real one. Two tries, a short pause
    // between, is all the wake sequence there is over I²C.
    uint8_t body[8];
    uint8_t resp[8];
    int n = -1;
    for (int attempt = 0; attempt < 2 && n < 0; attempt++) {
        body[0] = PN532_CMD_GET_FIRMWARE_VERSION;
        n = pn532_command(body, 1, resp, sizeof(resp), NFC_READY_TIMEOUT_MS);
        if (n < 0) {
            delay(10);
        }
    }
    if (n != 4) {
        Serial.println("[nfc] PN532 did not answer GetFirmwareVersion");
        return false;
    }
    fw_version = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                 ((uint32_t)resp[2] << 8) | (uint32_t)resp[3];
    Serial.printf("[nfc] PN532 IC 0x%02X firmware %u.%u support 0x%02X\n",
                  resp[0], resp[1], resp[2], resp[3]);

    // SAMConfiguration (§7.2.10): mode 0x01 normal (no SAM), timeout 0x14
    // (only meaningful in virtual-card mode, kept at the manual's example
    // value), IRQ 0x00 because CN1 has no IRQ line.
    body[0] = PN532_CMD_SAM_CONFIGURATION;
    body[1] = 0x01;
    body[2] = 0x14;
    body[3] = 0x00;
    if (pn532_command(body, 4, resp, sizeof(resp), NFC_READY_TIMEOUT_MS) < 0) {
        Serial.println("[nfc] SAMConfiguration failed");
        return false;
    }

    // RFConfiguration MaxRetries (§7.3.1): ATR and PSL retries stay at their
    // default 0xFF; passive activation is bounded so InListPassiveTarget
    // returns without a tag rather than searching forever.
    body[0] = PN532_CMD_RF_CONFIGURATION;
    body[1] = 0x05;
    body[2] = 0xFF;
    body[3] = 0xFF;
    body[4] = PN532_MX_RTY_PASSIVE_ACTIVATION;
    if (pn532_command(body, 5, resp, sizeof(resp), NFC_READY_TIMEOUT_MS) < 0) {
        Serial.println("[nfc] RFConfiguration failed");
        return false;
    }

    return true;
}

enum nfc_detect_e nfc_detect(uint8_t uid[7], uint8_t* uid_len) {
    if (uid_len) {
        *uid_len = 0;
    }

    // InListPassiveTarget (§7.3.5): MaxTg, BrTy, no InitiatorData.
    // Response: NbTg, then per target Tg, SENS_RES(2), SEL_RES, NFCIDLength,
    // NFCID1[NFCIDLength], optional ATS. Two targets fit in `resp`.
    uint8_t body[3] = { PN532_CMD_IN_LIST_PASSIVE_TARGET, PN532_MAX_TG,
                        PN532_BRTY_106A };
    uint8_t resp[PN532_FRAME_MAX - 8];

    // Whether or not a tag is present, the chip searches until it has MaxTg
    // targets or the passive-activation retries run out, so this is the one
    // response that takes real time. The ACK still arrives at once.
    int n = pn532_command(body, sizeof(body), resp, sizeof(resp),
                          NFC_DETECT_TIMEOUT_MS + NFC_READY_TIMEOUT_MS);
    if (n < 1) {
        return NFC_DETECT_ERR;
    }

    uint8_t nb_tg = resp[0];
    if (nb_tg == 0) {
        return NFC_DETECT_NONE;
    }
    if (nb_tg >= 2) {
        return NFC_DETECT_MULTI;
    }

    // Exactly one target. Layout check before touching the UID bytes.
    if (n < 6) {
        return NFC_DETECT_ERR;
    }
    uint8_t id_len = resp[5];
    if (id_len == 0 || (size_t)6 + id_len > (size_t)n) {
        return NFC_DETECT_ERR;
    }
    if (uid) {
        uint8_t copy = id_len > 7 ? 7 : id_len;
        for (uint8_t i = 0; i < copy; i++) {
            uid[i] = resp[6 + i];
        }
        if (uid_len) {
            *uid_len = copy;
        }
    }
    return NFC_DETECT_ONE;
}

int nfc_transceive(void* ctx, const uint8_t* tx, size_t tx_len,
                   uint8_t* rx, size_t rx_cap, size_t* rx_len) {
    (void)ctx;
    if (rx_len) {
        *rx_len = 0;
    }
    if (!tx || tx_len == 0 || (!rx && rx_cap > 0) || !rx_len) {
        return NTAG_XCV_IO;
    }

    // InDataExchange (§7.3.8): Tg = 1 (the target nfc_detect() selected),
    // then the raw tag command. Response: Status, then the tag's answer.
    uint8_t body[2 + NTAG_READ_SIZE + 4];
    if (tx_len > sizeof(body) - 2) {
        return NTAG_XCV_IO;
    }
    body[0] = PN532_CMD_IN_DATA_EXCHANGE;
    body[1] = 0x01;
    for (size_t i = 0; i < tx_len; i++) {
        body[2 + i] = tx[i];
    }

    uint8_t resp[1 + NTAG_READ_SIZE + 8];
    int n = pn532_command(body, 2 + tx_len, resp, sizeof(resp),
                          NFC_READY_TIMEOUT_MS);
    if (n < 1) {
        return NTAG_XCV_IO;
    }

    uint8_t status = resp[0] & PN532_STATUS_MASK;
    if (status == PN532_STATUS_TIMEOUT) {
        return NTAG_XCV_NAK;
    }
    if (status != PN532_STATUS_OK) {
        return NTAG_XCV_IO;
    }

    size_t data_len = (size_t)n - 1;
    if (data_len > rx_cap) {
        return NTAG_XCV_IO;
    }
    for (size_t i = 0; i < data_len; i++) {
        rx[i] = resp[1 + i];
    }
    *rx_len = data_len;
    return NTAG_XCV_OK;
}

uint32_t nfc_firmware_version() {
    return fw_version;
}
