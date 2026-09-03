#include "catalog.h"

#include <string.h>

/*
 * Per-line callback. Returns 0 to keep going, 1 to stop early with success,
 * or a negative catalog_result_e to abort and propagate.
 */
typedef int (*catalog_line_fn)(void* user, uint32_t offset, const char* line,
                               size_t len);

static int reader_usable(const catalog_reader_t* rd)
{
    return rd != NULL && rd->read != NULL;
}

/*
 * Read the line starting at `off` into buf/cap, stripping the newline and a
 * CRLF's carriage return. *out_len is the line's length, *out_next the offset
 * to read from for the following line, and *out_eof is set when there was
 * nothing left to read.
 *
 * Every line is read from its own offset with a fresh chunk, so a line
 * straddling any particular read boundary is not a special case.
 */
static int read_line_at(const catalog_reader_t* rd, uint32_t off, char* buf,
                        size_t cap, size_t* out_len, uint32_t* out_next,
                        int* out_eof)
{
    size_t got = 0;
    size_t i;
    size_t len;

    *out_len = 0;
    *out_next = off;
    *out_eof = 0;

    if (rd->read(rd->ctx, off, buf, cap, &got) != 0) {
        return CATALOG_ERR_IO;
    }
    if (got == 0) {
        *out_eof = 1;
        return CATALOG_OK;
    }
    if (got > cap) {
        return CATALOG_ERR_IO; /* the reader overran the buffer it was given */
    }

    for (i = 0; i < got && buf[i] != '\n'; i++) {
    }
    if (i == got) {
        /* No newline in the chunk: either the file's last line, or a line
         * longer than the format allows. */
        if (got == cap) {
            return CATALOG_ERR_LINE;
        }
        len = got;
        *out_next = off + (uint32_t)got;
    } else {
        len = i;
        *out_next = off + (uint32_t)i + 1u;
    }
    if (len > 0 && buf[len - 1] == '\r') {
        len--;
    }
    *out_len = len;
    return CATALOG_OK;
}

/* Walk every non-empty line in the file. Empty lines — including the one a
 * trailing newline leaves at the end — are not entries. */
static int for_each_line(const catalog_reader_t* rd, catalog_line_fn fn,
                         void* user)
{
    char buf[CATALOG_LINE_MAX];
    uint32_t off = 0;
    uint32_t next = 0;
    size_t len = 0;
    int eof = 0;
    int rc;

    for (;;) {
        rc = read_line_at(rd, off, buf, sizeof(buf), &len, &next, &eof);
        if (rc != CATALOG_OK) {
            return rc;
        }
        if (eof) {
            return CATALOG_OK;
        }
        if (len > 0) {
            rc = fn(user, off, buf, len);
            if (rc < 0) {
                return rc;
            }
            if (rc > 0) {
                return CATALOG_OK;
            }
        }
        if (next <= off) {
            return CATALOG_OK; /* no progress; stop rather than spin */
        }
        off = next;
    }
}

/* Comma-separated tokens. Unknown ones are ignored so the generator can add
 * flags without a firmware change. */
static uint8_t parse_flags(const char* s, size_t len)
{
    static const char starter[] = "starter";
    uint8_t flags = 0;
    size_t begin = 0;
    size_t i;

    for (i = 0; i <= len; i++) {
        if (i == len || s[i] == ',') {
            size_t tok = i - begin;
            if (tok == sizeof(starter) - 1 &&
                memcmp(s + begin, starter, tok) == 0) {
                flags |= CATALOG_FLAG_STARTER;
            }
            begin = i + 1;
        }
    }
    return flags;
}

int catalog_parse_line(const char* line, size_t len, catalog_entry_t* out,
                       const char** desc, size_t* desc_len)
{
    size_t tab[3];
    size_t n_tabs = 0;
    size_t i;
    size_t fn_len;
    size_t ti_len;

    if (line == NULL || out == NULL) {
        return CATALOG_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));
    if (desc != NULL) {
        *desc = NULL;
    }
    if (desc_len != NULL) {
        *desc_len = 0;
    }

    /* Only the first three tabs are separators; the description is the rest
     * of the line, tabs and all. */
    for (i = 0; i < len && n_tabs < 3; i++) {
        if (line[i] == '\t') {
            tab[n_tabs] = i;
            n_tabs++;
        }
    }
    if (n_tabs < 3) {
        return CATALOG_ERR_LINE;
    }

    /* Fail closed on an over-long field rather than truncate: the generator's
     * CI validator enforces the same caps, so this can only be a corrupt or
     * hand-edited file, and a truncated name would match the wrong ROM. */
    fn_len = tab[0];
    ti_len = tab[1] - tab[0] - 1;
    if (fn_len == 0 || fn_len >= ROM_STORE_NAME_MAX) {
        return CATALOG_ERR_LINE;
    }
    if (ti_len >= CATALOG_TITLE_MAX) {
        return CATALOG_ERR_LINE;
    }

    memcpy(out->filename, line, fn_len);
    out->filename[fn_len] = '\0';
    memcpy(out->title, line + tab[0] + 1, ti_len);
    out->title[ti_len] = '\0';
    out->flags = parse_flags(line + tab[1] + 1, tab[2] - tab[1] - 1);
    if (desc != NULL) {
        *desc = line + tab[2] + 1;
    }
    if (desc_len != NULL) {
        *desc_len = len - tab[2] - 1;
    }
    return CATALOG_OK;
}

typedef struct {
    catalog_index_t* idx;
    int full;
} build_ctx_t;

static int build_cb(void* user, uint32_t offset, const char* line, size_t len)
{
    build_ctx_t* c = (build_ctx_t*)user;
    catalog_entry_t e;
    int rc;

    rc = catalog_parse_line(line, len, &e, NULL, NULL);
    if (rc != CATALOG_OK) {
        return rc;
    }
    if (c->idx->count >= CATALOG_MAX) {
        c->full = 1;
        return 1;
    }
    e.offset = offset;
    c->idx->e[c->idx->count] = e;
    c->idx->count++;
    return 0;
}

int catalog_index_build(const catalog_reader_t* rd, catalog_index_t* out)
{
    build_ctx_t c;
    int rc;

    if (!reader_usable(rd) || out == NULL) {
        return CATALOG_ERR_ARGS;
    }
    out->count = 0;
    c.idx = out;
    c.full = 0;

    rc = for_each_line(rd, build_cb, &c);
    if (rc != CATALOG_OK) {
        return rc;
    }
    return c.full ? CATALOG_ERR_FULL : CATALOG_OK;
}

int catalog_read_desc(const catalog_reader_t* rd, uint32_t offset, char* out,
                      size_t out_sz)
{
    char buf[CATALOG_LINE_MAX];
    catalog_entry_t e;
    const char* desc = NULL;
    size_t desc_len = 0;
    size_t len = 0;
    uint32_t next = 0;
    int eof = 0;
    int rc;

    if (!reader_usable(rd) || out == NULL || out_sz == 0) {
        return CATALOG_ERR_ARGS;
    }
    out[0] = '\0';

    rc = read_line_at(rd, offset, buf, sizeof(buf), &len, &next, &eof);
    if (rc != CATALOG_OK) {
        return rc;
    }
    if (eof || len == 0) {
        return CATALOG_NOT_FOUND;
    }
    rc = catalog_parse_line(buf, len, &e, &desc, &desc_len);
    if (rc != CATALOG_OK) {
        return rc;
    }
    if (desc_len > out_sz - 1) {
        desc_len = out_sz - 1;
    }
    memcpy(out, desc, desc_len);
    out[desc_len] = '\0';
    return CATALOG_OK;
}

typedef struct {
    const char* filename;
    uint8_t flag;
    catalog_entry_t* out;
    int found;
} search_ctx_t;

static int find_cb(void* user, uint32_t offset, const char* line, size_t len)
{
    search_ctx_t* c = (search_ctx_t*)user;
    catalog_entry_t e;
    int rc;

    rc = catalog_parse_line(line, len, &e, NULL, NULL);
    if (rc != CATALOG_OK) {
        return rc;
    }
    /* Exact and case-sensitive: the tag carries the catalog's own filename. */
    if (strcmp(e.filename, c->filename) != 0) {
        return 0;
    }
    e.offset = offset;
    *c->out = e;
    c->found = 1;
    return 1;
}

static int flagged_cb(void* user, uint32_t offset, const char* line, size_t len)
{
    search_ctx_t* c = (search_ctx_t*)user;
    catalog_entry_t e;
    int rc;

    rc = catalog_parse_line(line, len, &e, NULL, NULL);
    if (rc != CATALOG_OK) {
        return rc;
    }
    if ((e.flags & c->flag) == 0) {
        return 0;
    }
    e.offset = offset;
    *c->out = e;
    c->found = 1;
    return 1;
}

int catalog_find(const catalog_reader_t* rd, const char* filename,
                 catalog_entry_t* out)
{
    search_ctx_t c;
    int rc;

    if (!reader_usable(rd) || filename == NULL || out == NULL) {
        return CATALOG_ERR_ARGS;
    }
    c.filename = filename;
    c.flag = 0;
    c.out = out;
    c.found = 0;

    rc = for_each_line(rd, find_cb, &c);
    if (rc != CATALOG_OK) {
        return rc;
    }
    return c.found ? CATALOG_OK : CATALOG_NOT_FOUND;
}

int catalog_first_flagged(const catalog_reader_t* rd, uint8_t flag,
                          catalog_entry_t* out)
{
    search_ctx_t c;
    int rc;

    if (!reader_usable(rd) || out == NULL) {
        return CATALOG_ERR_ARGS;
    }
    c.filename = NULL;
    c.flag = flag;
    c.out = out;
    c.found = 0;

    rc = for_each_line(rd, flagged_cb, &c);
    if (rc != CATALOG_OK) {
        return rc;
    }
    return c.found ? CATALOG_OK : CATALOG_NOT_FOUND;
}
