#include "sd_manager.h"
#include "hw_config.h"
#include "cart/match.h"
#include <SD.h>
#include <SPI.h>
#include <Arduino.h>

static SPIClass sdSPI(VSPI);
static bool ready = false;

bool sd_init() {
    sdSPI.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    if(!SD.begin(SD_PIN_CS, sdSPI, 20000000)){Serial.println("[SD] Mount fail!");return false;}

    Serial.printf("[SD] Type:%d Size:%lluMB\n",SD.cardType(),SD.cardSize()/(1024*1024));
    if(!SD.exists(ROM_PATH_GB)) SD.mkdir(ROM_PATH_GB);
    if(!SD.exists(SAVE_PATH)) SD.mkdir(SAVE_PATH);
    ready=true; return true;
}

// ─── ROM resolution ─────────────────────────────────────────────────────────

static bool has_gb_suffix(const char* name) {
    size_t n = strlen(name);
    if (n < 3) {
        return false;
    }
    const char* s = name + n - 3;
    return s[0] == '.' && tolower((unsigned char)s[1]) == 'g'
           && tolower((unsigned char)s[2]) == 'b';
}

bool sd_rom_path(const char* filename, char* out, size_t out_sz) {
    if (!ready || !filename || !out || !out_sz) {
        return false;
    }
    // snprintf would truncate, and a truncated path can name a different
    // file that really exists — so the length is checked before the build.
    int n = snprintf(out, out_sz, "%s/%s", ROM_PATH_GB, filename);
    if (n < 0 || (size_t)n >= out_sz) {
        out[0] = '\0';
        return false;
    }
    if (!SD.exists(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool sd_rom_find_legacy(const char* title, char* out_path, size_t out_sz) {
    if (!ready || !title || !out_path || !out_sz) {
        return false;
    }
    char norm[ROM_STORE_NAME_MAX];
    if (match_normalise(title, norm, sizeof(norm)) != MATCH_OK) {
        return false;
    }

    File d = SD.open(ROM_PATH_GB);
    if (!d || !d.isDirectory()) {
        if (d) {
            d.close();
        }
        return false;
    }

    bool found = false;
    File e;
    // One entry at a time: the predicate is per-entry precisely so no
    // listing has to exist in RAM.
    while (!found && (e = d.openNextFile())) {
        if (!e.isDirectory()) {
            const char* name = e.name();
            if (has_gb_suffix(name) && match_legacy(norm, name)) {
                found = sd_rom_path(name, out_path, out_sz);
            }
        }
        e.close();
    }
    d.close();
    return found;
}

// ─── Catalog ────────────────────────────────────────────────────────────────
// Opened once and left open: the index build reads the whole file and every
// later description read seeks back into it, so a per-call open would pay
// the directory walk again for nothing.

static File catalog_file;

static int catalog_read(void* ctx, uint32_t off, void* dst, size_t cap,
                        size_t* got) {
    (void)ctx;
    *got = 0;
    if (!catalog_file) {
        return -1;
    }
    // Reading at or past the end is end-of-file, not an error: seek() on a
    // FAT file can refuse an offset past the end, and the reader contract
    // spells that case *got == 0.
    if (off >= (uint32_t)catalog_file.size()) {
        return 0;
    }
    if (!catalog_file.seek(off)) {
        return -1;
    }
    int n = catalog_file.read((uint8_t*)dst, cap);
    if (n < 0) {
        return -1;
    }
    *got = (size_t)n;
    return 0;
}

bool sd_catalog_reader(catalog_reader_t* out) {
    if (!ready || !out) {
        return false;
    }
    if (!catalog_file) {
        if (!SD.exists(CATALOG_PATH)) {
            return false;
        }
        catalog_file = SD.open(CATALOG_PATH, FILE_READ);
        if (!catalog_file) {
            return false;
        }
    }
    out->ctx = NULL;
    out->read = catalog_read;
    return true;
}

void sd_get_save_path(const char* rp, char* sp, int mx) {
    const char* fn=strrchr(rp,'/'); if(!fn)fn=rp; else fn++;
    char base[ROM_STORE_NAME_MAX];
    strncpy(base, fn, ROM_STORE_NAME_MAX - 1);
    base[ROM_STORE_NAME_MAX - 1] = 0;
    char* dot=strrchr(base,'.'); if(dot)*dot=0;
    snprintf(sp,mx,"%s/%s.sav",SAVE_PATH,base);
}

// The save is written to a sibling temp file and renamed over the real one,
// so the card always holds one complete save. Writing in place would mean
// removing the old file and then spending the whole write — up to 32 KB —
// with nothing valid on the card, and the flush most likely to be cut short
// is the low-battery one, which fires when power is about to go.
//
// The window is not closed, only narrowed: a power loss between the remove
// and the rename still loses the save. Both are single directory-entry
// operations, against a write that is thousands of times longer.
bool sd_save_state(const char* rp, const uint8_t* data, uint32_t sz) {
    if (!ready || !data || !sz) {
        return false;
    }

    char sp[96];
    sd_get_save_path(rp, sp, 96);

    char tp[96 + 4];
    int n = snprintf(tp, sizeof(tp), "%s%s", sp, SAVE_TMP_SUFFIX);
    if (n < 0 || (size_t)n >= sizeof(tp)) {
        return false;
    }

    // A temp file left by an earlier interrupted save is stale by definition.
    // FILE_WRITE truncates, so this is belt and braces, not a correctness
    // requirement — and it keeps the card from accumulating them.
    if (SD.exists(tp)) {
        SD.remove(tp);
    }

    File f = SD.open(tp, FILE_WRITE);
    if (!f) {
        return false;
    }
    size_t w = f.write(data, sz);
    f.close();

    // A short write never becomes the save file. The old one stays untouched
    // and the caller keeps the RAM dirty for the next flush.
    if (w != sz) {
        Serial.printf("[SD] Save short: %s (%u of %u)\n", tp, (uint32_t)w, sz);
        SD.remove(tp);
        return false;
    }

    // rename() over an existing name is not portable across the FAT layers
    // this could sit on, so the destination is removed first rather than
    // relied on to be replaced.
    if (SD.exists(sp)) {
        SD.remove(sp);
    }
    if (!SD.rename(tp, sp)) {
        Serial.printf("[SD] Save rename failed: %s -> %s\n", tp, sp);
        return false;
    }

    Serial.printf("[SD] Save: %s (%u)\n", sp, (uint32_t)w);
    return true;
}

bool sd_load_state(const char* rp, uint8_t* data, uint32_t sz) {
    if(!ready||!data||!sz) return false;
    char sp[96]; sd_get_save_path(rp,sp,96);
    if(!SD.exists(sp)) {
        Serial.printf("[SD] Load miss: %s\n", sp);
        return false;
    }
    File f=SD.open(sp,FILE_READ); if(!f) return false;
    size_t r=f.read(data,sz); f.close();
    Serial.printf("[SD] Load: %s (%u)\n",sp,r);
    if (r != sz) {
        Serial.printf("[SD] Load size mismatch: expected=%u got=%u\n", sz, (uint32_t)r);
    }
    return r==sz;
}
