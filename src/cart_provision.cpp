#include "cart_provision.h"
#include "hw_config.h"
#include "nfc_cart.h"
#include "settings.h"
#include "cart/ndef.h"
#include "cart/ntag.h"
#include <Arduino.h>

// The build-wide password. Not a secret — hw_config.h says why at length.
static const uint8_t pwd[NTAG_PWD_SIZE] = NTAG_PWD;
static const uint8_t pack[NTAG_PACK_SIZE] = NTAG_PACK;

// The tag layer over the PN532. nfc_transceive is shaped as gbcore's
// ntag_xcv_fn precisely so this binding is one struct and no adapter.
static const ntag_dev_t dev = { NULL, nfc_transceive };

enum boot_auth_e provision_auth_state() {
    uint8_t auth0 = 0;
    int rc = ntag_read_auth0(&dev, &auth0);
    if (rc != NTAG_OK) {
        Serial.printf("[CART] auth0 read failed (%d) - treating as foreign\n", rc);
        return BOOT_AUTH_FOREIGN;
    }
    if (auth0 == NTAG215_AUTH0_OPEN) {
        return BOOT_AUTH_OPEN;
    }

    rc = ntag_pwd_auth(&dev, pwd, pack);
    if (rc == NTAG_OK) {
        return BOOT_AUTH_OURS;
    }
    // A refused password means someone else's tag; a transport failure means
    // we do not know. Both answer FOREIGN, because both must stop a write.
    Serial.printf("[CART] auth failed (%d) - foreign\n", rc);
    return BOOT_AUTH_FOREIGN;
}

// Payload string -> framed NDEF message, ready for user memory.
static int compose(enum boot_class_e cls, const char* rom, uint8_t* buf,
                   size_t* len) {
    char payload[ROM_STORE_NAME_MAX + 8];
    int rc = boot_compose_payload(cls, rom, payload, sizeof(payload));
    if (rc != BOOT_CLASSIFY_OK) {
        Serial.printf("[CART] payload compose failed (%d)\n", rc);
        return NTAG_ERR_ARGS;
    }
    rc = ndef_compose_text(payload, buf, NDEF_BUF_MAX, len);
    if (rc != NDEF_OK) {
        Serial.printf("[CART] ndef compose failed (%d)\n", rc);
        return NTAG_ERR_ARGS;
    }
    return NTAG_OK;
}

// Compose for `cls`, then write, verify and protect in one sequence. The
// class is always one the decision table asked for; a menu cart is never a
// write target, because the table checks MENU before it checks pending.
static int write_cart(enum boot_class_e cls, const char* rom) {
    uint8_t msg[NDEF_BUF_MAX];
    size_t len = 0;
    int rc = compose(cls, rom, msg, &len);
    if (rc != NTAG_OK) {
        return rc;
    }
    rc = ntag_provision(&dev, msg, len, pwd, pack);
    Serial.printf("[CART] write cls=%d rom='%s' -> %d\n", (int)cls,
                  rom ? rom : "", rc);
    return rc;
}

int provision_wizard_menu(boot_flags_t* flags) {
    if (!flags) {
        return NTAG_ERR_ARGS;
    }
    int rc = write_cart(BOOT_CLASS_MENU, NULL);
    if (rc != NTAG_OK) {
        return rc;
    }
    // Only past the verified write: a failed write must leave the wizard on
    // this step so the next boot repeats it.
    flags->menu_done = true;
    settings_wizard_save(flags);
    return NTAG_OK;
}

int provision_wizard_adopt(enum boot_class_e which, boot_flags_t* flags) {
    if (!flags) {
        return NTAG_ERR_ARGS;
    }
    if (which == BOOT_CLASS_MENU) {
        flags->menu_done = true;
    } else if (which == BOOT_CLASS_WILD) {
        flags->wild_done = true;
    } else {
        return NTAG_ERR_ARGS;
    }
    Serial.printf("[CART] adopted cls=%d\n", (int)which);
    settings_wizard_save(flags);
    return NTAG_OK;
}

int provision_wizard_write(enum boot_pick_action_e pick_action,
                           const boot_selection_t* pick, boot_flags_t* flags) {
    if (!pick || !flags) {
        return NTAG_ERR_ARGS;
    }
    if (pick_action == BOOT_PICK_WRITE_WILD) {
        int rc = write_cart(BOOT_CLASS_WILD, pick->rom);
        if (rc != NTAG_OK) {
            return rc;
        }
        flags->wild_done = true;
        settings_wizard_save(flags);
        return NTAG_OK;
    }
    if (pick_action == BOOT_PICK_WRITE_GAME) {
        // No flag: which game carts the wizard wrote is deliberately not
        // recorded anywhere.
        return write_cart(BOOT_CLASS_GAME, pick->rom);
    }
    return NTAG_ERR_ARGS;
}

int provision_wizard_finish(boot_flags_t* flags) {
    if (!flags) {
        return NTAG_ERR_ARGS;
    }
    flags->setup_done = true;
    Serial.println("[CART] setup finished");
    settings_wizard_save(flags);
    return NTAG_OK;
}

int provision_execute_pending(const boot_selection_t* pending,
                              enum boot_class_e presented) {
    if (!pending) {
        return NTAG_ERR_ARGS;
    }
    enum boot_class_e cls = boot_target_class(pending->target, presented);
    int rc = write_cart(cls, pending->rom);
    if (rc != NTAG_OK) {
        // Left in place on purpose: repeating the write is safe, and losing
        // the record would lose the user's selection instead.
        Serial.printf("[CART] pending write failed (%d) - record kept\n", rc);
        return rc;
    }
    settings_pending_clear();
    return NTAG_OK;
}

int provision_heal() {
    int rc = ntag_protect(&dev, pwd, pack);
    if (rc != NTAG_OK) {
        Serial.printf("[CART] heal protect failed (%d)\n", rc);
        return rc;
    }
    // The only available proof the protection took: PWD and PACK read back
    // as zeros, so authenticating against them is the read.
    rc = ntag_pwd_auth(&dev, pwd, pack);
    Serial.printf("[CART] heal -> %d\n", rc);
    return rc;
}
