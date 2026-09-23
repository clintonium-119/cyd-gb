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

uint16_t sd_rom_count() {
    if (!ready) {
        return 0;
    }

    File d = SD.open(ROM_PATH_GB);
    if (!d || !d.isDirectory()) {
        if (d) {
            d.close();
        }
        return 0;
    }

    uint16_t n = 0;
    File e;
    // One entry at a time, for the same reason the legacy lookup does it that
    // way: a listing of the 132-title library has no business existing in RAM
    // on a board with no PSRAM.
    while ((e = d.openNextFile())) {
        if (!e.isDirectory() && has_gb_suffix(e.name())) {
            n++;
        }
        e.close();
    }
    d.close();
    return n;
}

bool sd_card_stats(uint32_t* total_mb, uint32_t* used_mb) {
    if (!ready) {
        return false;
    }
    if (total_mb) {
        *total_mb = (uint32_t)(SD.totalBytes() / (1024ULL * 1024ULL));
    }
    if (used_mb) {
        *used_mb = (uint32_t)(SD.usedBytes() / (1024ULL * 1024ULL));
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

// The chunk reader behind both the catalog and a manual; ctx is the File.
static int file_read(void* ctx, uint32_t off, void* dst, size_t cap,
                     size_t* got) {
    File& f = *(File*)ctx;
    *got = 0;
    if (!f) {
        return -1;
    }
    // Reading at or past the end is end-of-file, not an error: seek() on a
    // FAT file can refuse an offset past the end, and the reader contract
    // spells that case *got == 0.
    if (off >= (uint32_t)f.size()) {
        return 0;
    }
    if (!f.seek(off)) {
        return -1;
    }
    int n = f.read((uint8_t*)dst, cap);
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
    out->ctx = &catalog_file;
    out->read = file_read;
    return true;
}

// ─── Art ────────────────────────────────────────────────────────────────────
// One pair of functions for both /art and /shot: the stem rule, the exact-size
// check and the open-read-close shape are identical, and the directory is the
// only thing that differs.
//
// Unlike the catalog above, a media file is not held open. The catalog is read
// once for the index and then seeked into for every description; a cover or a
// snapshot is read once when a title is opened and never again.

// <dir>/<stem><suffix>, the rule every per-game file on the card follows.
static bool stem_path(const char* dir, const char* rom_filename,
                      const char* suffix, char* out, size_t out_sz) {
    if (!ready || !dir || !rom_filename || !out || !out_sz) {
        return false;
    }
    out[0] = '\0';

    // The stem is the filename without a trailing ".gb". Case-sensitive: the
    // filename is the frozen key the cartridge carries, and a name that does
    // not end in ".gb" is used whole.
    size_t stem = strlen(rom_filename);
    if (stem >= 3 && strcmp(rom_filename + stem - 3, ".gb") == 0) {
        stem -= 3;
    }
    if (stem == 0) {
        return false;
    }

    // Length checked through snprintf's return before the path is used, the
    // same rule sd_rom_path() follows: a truncated path can name a different
    // file that really exists.
    int n = snprintf(out, out_sz, "%s/%.*s%s", dir, (int)stem, rom_filename,
                     suffix);
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

bool sd_media_path(const char* dir, const char* rom_filename, char* out,
                   size_t out_sz) {
    return stem_path(dir, rom_filename, ART_SUFFIX, out, out_sz);
}

bool sd_media_stream(const char* dir, const char* rom_filename, uint16_t* buf,
                     size_t row_w, size_t total_rows, size_t band_rows,
                     sd_media_band_fn fn, void* ctx) {
    char path[ART_PATH_MAX];

    if (!buf || !fn || !row_w || !total_rows || !band_rows) {
        return false;
    }
    if (!sd_media_path(dir, rom_filename, path, sizeof(path))) {
        // A missing file is the ordinary case, not worth a line of log per
        // title the imaging tool has not covered yet.
        return false;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[SD] art open failed: %s\n", path);
        return false;
    }

    // The whole file's size is still the contract, checked before a single
    // band is read: a short file would otherwise be discovered halfway down
    // the image, with the top of it already on the panel.
    size_t want = row_w * total_rows * sizeof(uint16_t);
    if (f.size() != want) {
        Serial.printf("[SD] art size %u, want %u: %s\n", (unsigned)f.size(),
                      (unsigned)want, path);
        f.close();
        return false;
    }

    for (size_t row0 = 0; row0 < total_rows; row0 += band_rows) {
        size_t rows = total_rows - row0;
        if (rows > band_rows) {
            rows = band_rows;
        }

        size_t need = row_w * rows * sizeof(uint16_t);
        uint8_t* dst = (uint8_t*)buf;
        size_t got = 0;
        // The SD library may return a short read; loop until the band is
        // full or a read stops making progress.
        while (got < need) {
            int n = f.read(dst + got, need - got);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        if (got != need) {
            Serial.printf("[SD] art short read %u of %u at row %u: %s\n",
                          (unsigned)got, (unsigned)need, (unsigned)row0, path);
            f.close();
            return false;
        }
        fn(ctx, buf, row0, rows);
    }

    f.close();
    return true;
}

bool sd_media_read(const char* dir, const char* rom_filename, uint16_t* out,
                   size_t px_count) {
    char path[ART_PATH_MAX];

    if (!out || !px_count) {
        return false;
    }
    if (!sd_media_path(dir, rom_filename, path, sizeof(path))) {
        // A missing file is the ordinary case, not worth a line of log per
        // title the imaging tool has not covered yet.
        return false;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[SD] art open failed: %s\n", path);
        return false;
    }

    size_t want = px_count * sizeof(uint16_t);
    if (f.size() != want) {
        Serial.printf("[SD] art size %u, want %u: %s\n", (unsigned)f.size(),
                      (unsigned)want, path);
        f.close();
        return false;
    }

    // The SD library may return a short read; loop until the buffer is full
    // or a read stops making progress.
    uint8_t* dst = (uint8_t*)out;
    size_t got = 0;
    while (got < want) {
        int n = f.read(dst + got, want - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    f.close();

    if (got != want) {
        Serial.printf("[SD] art short read %u of %u: %s\n", (unsigned)got,
                      (unsigned)want, path);
        return false;
    }
    return true;
}

// ─── Manuals ────────────────────────────────────────────────────────────────
// Held open while the reader is on screen, like the catalog, because every
// band it draws seeks back into the file; closed when the reader exits, unlike
// the catalog, because at game time nothing else wants it.

static File manual_file;

bool sd_manual_path(const char* rom_filename, char* out, size_t out_sz) {
    return stem_path(MANUAL_PATH, rom_filename, MANUAL_SUFFIX, out, out_sz);
}

bool sd_manual_reader(const char* rom_filename, manual_reader_t* out,
                      uint32_t* size) {
    char path[ART_PATH_MAX];

    sd_manual_close();
    if (!out || !size || !sd_manual_path(rom_filename, path, sizeof(path))) {
        return false;
    }
    manual_file = SD.open(path, FILE_READ);
    if (!manual_file) {
        Serial.printf("[SD] manual open failed: %s\n", path);
        return false;
    }
    *size = (uint32_t)manual_file.size();
    out->ctx = &manual_file;
    out->read = file_read;
    return true;
}

void sd_manual_close() {
    if (manual_file) {
        manual_file.close();
    }
}

// ─── Descriptions ───────────────────────────────────────────────────────────
// Read once when a page opens, like the art, so not held open.

bool sd_desc_read(const char* rom_filename, char* out, size_t out_sz) {
    char path[ART_PATH_MAX];

    if (!out || !out_sz) {
        return false;
    }
    out[0] = '\0';
    if (!stem_path(DESC_PATH, rom_filename, DESC_SUFFIX, path, sizeof(path))) {
        // A missing file is the ordinary case: the blurb stands in.
        return false;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[SD] desc open failed: %s\n", path);
        return false;
    }

    size_t want = f.size();
    if (want >= out_sz) {
        Serial.printf("[SD] desc size %u, max %u: %s\n", (unsigned)want,
                      (unsigned)(out_sz - 1), path);
        f.close();
        return false;
    }

    // The SD library may return a short read; loop until the text is in or a
    // read stops making progress.
    size_t got = 0;
    while (got < want) {
        int n = f.read((uint8_t*)out + got, want - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    f.close();

    if (got != want) {
        Serial.printf("[SD] desc short read %u of %u: %s\n", (unsigned)got,
                      (unsigned)want, path);
        out[0] = '\0';
        return false;
    }
    out[got] = '\0';
    return true;
}

// <prefix>/saves/<base><suffix>, where <base> is the ROM's filename less its
// extension. Every file kept beside the battery save follows this one rule,
// so a state can never end up under a different stem from its .sav.
static bool saves_path(const char* rp, const char* suffix, const char* prefix,
                       char* out, size_t mx) {
    const char* fn=strrchr(rp,'/'); if(!fn)fn=rp; else fn++;
    char base[ROM_STORE_NAME_MAX];
    strncpy(base, fn, ROM_STORE_NAME_MAX - 1);
    base[ROM_STORE_NAME_MAX - 1] = 0;
    char* dot=strrchr(base,'.'); if(dot)*dot=0;
    int n = snprintf(out, mx, "%s%s/%s%s", prefix, SAVE_PATH, base, suffix);
    return n >= 0 && (size_t)n < mx;
}

void sd_get_save_path(const char* rp, char* sp, int mx) {
    saves_path(rp, ".sav", "", sp, (size_t)mx);
}

bool sd_get_state_path(const char* rom_path, const char* suffix, bool vfs,
                       char* out, size_t out_sz) {
    if (!rom_path || !suffix || !out || !out_sz) {
        return false;
    }
    if (!saves_path(rom_path, suffix, vfs ? SD_VFS_ROOT : "", out, out_sz)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool sd_state_exists(const char* rom_path) {
    char p[STATE_PATH_MAX];
    return ready && sd_get_state_path(rom_path, STATE_SUFFIX, false, p,
                                      sizeof(p))
           && SD.exists(p);
}

// rename() over an existing name is not portable across the FAT layers this
// could sit on, so the destination is removed first rather than relied on to
// be replaced.
bool sd_commit_tmp(const char* path) {
    char tp[STATE_PATH_MAX + 4];
    int n = snprintf(tp, sizeof(tp), "%s%s", path, SAVE_TMP_SUFFIX);
    if (!ready || n < 0 || (size_t)n >= sizeof(tp)) {
        return false;
    }
    if (SD.exists(path)) {
        SD.remove(path);
    }
    if (!SD.rename(tp, path)) {
        Serial.printf("[SD] rename failed: %s -> %s\n", tp, path);
        return false;
    }
    return true;
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

    if (!sd_commit_tmp(sp)) {
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

bool sd_boot_rom_read(uint8_t* out, size_t size) {
    File f = SD.open(BOOT_ROM_PATH, FILE_READ);
    if (!f) {
        // The ordinary case on a card nobody copied one to.
        return false;
    }
    if (f.size() != size) {
        Serial.printf("[SD] boot ROM size %u, want %u\n", (unsigned)f.size(),
                      (unsigned)size);
        f.close();
        return false;
    }
    size_t got = f.read(out, size);
    f.close();
    return got == size;
}
