#include <unity.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

/*
 * Structural guards over the first-party sources.
 *
 * These replace an earlier test that asserted a single symbol's absence. They
 * exist because five properties of this firmware are load-bearing and none
 * of them are expressible in C:
 *
 *   (a) exactly one translation unit can write a cartridge tag,
 *   (b) the cartridge writer is opened from exactly one place,
 *   (c) the writer reaches for nothing below itself,
 *   (f) the in-game menu is not a route to the writer or to the tag,
 *   (g) no source under src/ carries the removed exit-to-selection path,
 *
 * plus (e) the bench ROM bypass cannot be configured into a default build.
 * Guard (d), the tag layer's page whitelist, is a behavioural property and
 * lives in test_ntag where it belongs.
 *
 * The mechanism is a plain substring scan, comments included, and that is
 * deliberate on both counts. Substring means there is no parser to disagree
 * with the compiler about what the source says. Including comments means the
 * rule is "these names do not appear here", which is simple enough to hold in
 * your head and cannot be satisfied by moving a call into a comment.
 *
 * Only src/ and include/ are scanned. lib/ defines these symbols and test/
 * exercises them; both are supposed to name them.
 */

/* The tag-write symbols. Referencing any of these is what "can write a
 * cartridge" means in practice. */
static const char* const WRITE_SYMS[] = {
    "ntag_write_",
    "ntag_protect",
    "ntag_provision",
    "ntag_pwd_auth",
};
#define N_WRITE_SYMS (sizeof(WRITE_SYMS) / sizeof(WRITE_SYMS[0]))

/* The one translation unit allowed to reference them. */
#define PROVISIONER "cart_provision.cpp"

/* Layers the writer must not reach into. */
static const char* const WRITER_FORBIDDEN[] = {
    "ntag_",
    "nfc_",
    "emu_",
    "rom_store_",
};
#define N_WRITER_FORBIDDEN \
    (sizeof(WRITER_FORBIDDEN) / sizeof(WRITER_FORBIDDEN[0]))

/* Layers the in-game menu must not reach into either. It is handed a snapshot
 * of the cartridge and shows it; it never reads a tag, resolves a path or
 * writes anything. "sd_rom_" catching sd_rom_path and sd_rom_find_legacy is
 * the intent, not a side effect. */
static const char* const MENU_FORBIDDEN[] = {
    "writer_open",
    "ntag_",
    "nfc_",
    "rom_store_",
    "sd_rom_",
    "provision_",
};
#define N_MENU_FORBIDDEN (sizeof(MENU_FORBIDDEN) / sizeof(MENU_FORBIDDEN[0]))

/* The two names the removed exit path went by. Literal and case-sensitive,
 * because the grep this replaces was. */
static const char* const GONE_SYMS[] = {
    "quit",
    "launcher_show",
};
#define N_GONE_SYMS (sizeof(GONE_SYMS) / sizeof(GONE_SYMS[0]))

void setUp(void)
{
}

void tearDown(void)
{
}

/* Whole file into a buffer. Returns 0 when the file cannot be opened. The
 * largest first-party source is a few tens of KB; 512 KB is generous and
 * keeps this a single read with no chunk-boundary case to get wrong. */
#define SLURP_MAX (512u * 1024u)
static char slurp_buf[SLURP_MAX];

static int slurp(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t n = fread(slurp_buf, 1, SLURP_MAX - 1, f);
    fclose(f);
    slurp_buf[n] = '\0';
    return 1;
}

static int file_contains(const char* path, const char* needle)
{
    if (!slurp(path)) {
        return 0;
    }
    return strstr(slurp_buf, needle) != NULL;
}

/* Occurrences of needle in the file at path. */
static int file_count(const char* path, const char* needle)
{
    if (!slurp(path)) {
        return -1;
    }
    int count = 0;
    size_t len = strlen(needle);
    const char* at = slurp_buf;
    while ((at = strstr(at, needle)) != NULL) {
        count++;
        at += len;
    }
    return count;
}

static int has_suffix(const char* name, const char* suffix)
{
    size_t n = strlen(name);
    size_t s = strlen(suffix);
    return n >= s && strcmp(name + n - s, suffix) == 0;
}

/* Calls fn for every entry in dir whose name ends in suffix. Returns the
 * number of entries visited, or -1 when the directory cannot be opened. */
typedef void (*visit_fn)(const char* dir, const char* name);

static int visit_dir(const char* dir, const char* suffix, visit_fn fn)
{
    DIR* d = opendir(dir);
    if (!d) {
        return -1;
    }
    int seen = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!has_suffix(e->d_name, suffix)) {
            continue;
        }
        seen++;
        fn(dir, e->d_name);
    }
    closedir(d);
    return seen;
}

static char path_buf[1024];

static const char* join(const char* dir, const char* name)
{
    snprintf(path_buf, sizeof(path_buf), "%s/%s", dir, name);
    return path_buf;
}

/* ─── project root sanity ─────────────────────────────────────────────────── */
/* Without this, a wrong PROJECT_DIR would make every scan below pass by
 * finding nothing at all. */

static void test_project_dir_points_at_the_repository(void)
{
    TEST_ASSERT_TRUE_MESSAGE(slurp(PROJECT_DIR "/platformio.ini"),
                             "PROJECT_DIR does not resolve to the repository "
                             "root: " PROJECT_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(slurp_buf, "[env:native]"),
                                 "platformio.ini opened but does not look "
                                 "like this project's");
}

/* ─── (a) one write site ──────────────────────────────────────────────────── */

static int a_offenders;
static char a_first_offender[512];

static void check_no_write_syms(const char* dir, const char* name)
{
    if (strcmp(name, PROVISIONER) == 0) {
        return;
    }
    const char* path = join(dir, name);
    for (size_t i = 0; i < N_WRITE_SYMS; i++) {
        if (file_contains(path, WRITE_SYMS[i])) {
            if (a_offenders == 0) {
                snprintf(a_first_offender, sizeof(a_first_offender),
                         "%s references %s", path, WRITE_SYMS[i]);
            }
            a_offenders++;
        }
    }
}

static void test_no_write_symbol_outside_the_provisioner(void)
{
    a_offenders = 0;
    a_first_offender[0] = '\0';

    int src = visit_dir(PROJECT_DIR "/src", ".cpp", check_no_write_syms);
    int inc = visit_dir(PROJECT_DIR "/include", ".h", check_no_write_syms);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, src, "no .cpp files found under src/");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, inc, "no .h files found under include/");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, a_offenders, a_first_offender);
}

/* The same scan, proved non-vacuous: if the provisioner stopped referencing
 * the write symbols, the test above would pass over a firmware that cannot
 * write a tag at all. */
static void test_the_provisioner_does_reference_a_write_symbol(void)
{
    const char* path = PROJECT_DIR "/src/" PROVISIONER;
    int found = 0;
    for (size_t i = 0; i < N_WRITE_SYMS; i++) {
        if (file_contains(path, WRITE_SYMS[i])) {
            found++;
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(
        0, found,
        "src/" PROVISIONER " references no tag-write symbol, so guard (a) "
        "would pass vacuously");
}

/* ─── (b) one writer_open call site ───────────────────────────────────────── */

static int b_call_sites;
static char b_files[512];

static void count_writer_open(const char* dir, const char* name)
{
    /* The definition lives in cart_writer.cpp; it is not a call site. */
    if (strcmp(name, "cart_writer.cpp") == 0) {
        return;
    }
    const char* path = join(dir, name);
    int n = file_count(path, "writer_open(");
    if (n > 0) {
        b_call_sites += n;
        snprintf(b_files + strlen(b_files), sizeof(b_files) - strlen(b_files),
                 "%s(x%d) ", name, n);
    }
}

static void test_writer_open_has_exactly_one_call_site(void)
{
    b_call_sites = 0;
    b_files[0] = '\0';

    TEST_ASSERT_GREATER_THAN(
        0, visit_dir(PROJECT_DIR "/src", ".cpp", count_writer_open));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, b_call_sites, b_files);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, file_count(PROJECT_DIR "/src/main.cpp", "writer_open("),
        "the single writer_open call site is not in src/main.cpp");
}

static void test_writer_open_is_declared_once(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, file_count(PROJECT_DIR "/include/cart_writer.h", "writer_open("),
        "include/cart_writer.h does not declare writer_open exactly once");
}

/* ─── (c) the writer reaches for nothing below itself ─────────────────────── */

static void test_the_writer_references_no_lower_layer(void)
{
    static const char* const paths[] = {
        PROJECT_DIR "/src/cart_writer.cpp",
        PROJECT_DIR "/include/cart_writer.h",
    };
    char msg[512];

    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        TEST_ASSERT_TRUE_MESSAGE(slurp(paths[p]), paths[p]);
        for (size_t i = 0; i < N_WRITER_FORBIDDEN; i++) {
            snprintf(msg, sizeof(msg), "%s references %s", paths[p],
                     WRITER_FORBIDDEN[i]);
            TEST_ASSERT_FALSE_MESSAGE(
                file_contains(paths[p], WRITER_FORBIDDEN[i]), msg);
        }
    }
}

/* ─── (e) the bypass is not configurable ──────────────────────────────────── */

static void test_platformio_ini_does_not_mention_the_rom_bypass(void)
{
    /* Assembled at run time rather than written as a literal, because this
     * file is not exempt from its own rule: a source that spells the token
     * out is a source someone can copy the wrong half of. */
    char needle[32];
    snprintf(needle, sizeof(needle), "%s_%s_%s", "DEV", "ROM", "PATH");

    TEST_ASSERT_FALSE_MESSAGE(
        file_contains(PROJECT_DIR "/platformio.ini", needle),
        "platformio.ini mentions the bench ROM bypass flag; it must stay an "
        "environment-variable-only build flag so no default build can "
        "acquire it");
}

/* ─── (f) the menu is not a route to the writer or the tag ────────────────── */

static void test_the_menu_references_no_writer_or_tag_symbol(void)
{
    static const char* const paths[] = {
        PROJECT_DIR "/src/menu.cpp",
        PROJECT_DIR "/include/menu.h",
    };
    char msg[512];

    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        TEST_ASSERT_TRUE_MESSAGE(slurp(paths[p]), paths[p]);
        for (size_t i = 0; i < N_MENU_FORBIDDEN; i++) {
            snprintf(msg, sizeof(msg), "%s references %s", paths[p],
                     MENU_FORBIDDEN[i]);
            TEST_ASSERT_FALSE_MESSAGE(
                file_contains(paths[p], MENU_FORBIDDEN[i]), msg);
        }
    }
}

/* The same scan, proved non-vacuous: a renamed or emptied menu would let the
 * test above pass by scanning nothing of consequence. */
static void test_the_menu_defines_menu_open(void)
{
    TEST_ASSERT_GREATER_THAN_MESSAGE(
        0, file_count(PROJECT_DIR "/src/menu.cpp", "menu_open("),
        "src/menu.cpp does not define menu_open, so guard (f) would pass "
        "vacuously");
}

/* ─── (g) no exit-to-selection path under src/ ────────────────────────────── */

static int g_offenders;
static char g_first_offender[512];

static void check_no_gone_syms(const char* dir, const char* name)
{
    const char* path = join(dir, name);
    for (size_t i = 0; i < N_GONE_SYMS; i++) {
        if (file_contains(path, GONE_SYMS[i])) {
            if (g_offenders == 0) {
                snprintf(g_first_offender, sizeof(g_first_offender),
                         "%s references %s", path, GONE_SYMS[i]);
            }
            g_offenders++;
        }
    }
}

static void test_no_exit_path_symbol_under_src(void)
{
    g_offenders = 0;
    g_first_offender[0] = '\0';

    /* The project-root sanity test above already proves this scan finds
     * files, so no separate vacuity twin is needed here. */
    int src = visit_dir(PROJECT_DIR "/src", ".cpp", check_no_gone_syms);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, src, "no .cpp files found under src/");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_offenders, g_first_offender);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_project_dir_points_at_the_repository);
    RUN_TEST(test_no_write_symbol_outside_the_provisioner);
    RUN_TEST(test_the_provisioner_does_reference_a_write_symbol);
    RUN_TEST(test_writer_open_has_exactly_one_call_site);
    RUN_TEST(test_writer_open_is_declared_once);
    RUN_TEST(test_the_writer_references_no_lower_layer);
    RUN_TEST(test_platformio_ini_does_not_mention_the_rom_bypass);
    RUN_TEST(test_the_menu_references_no_writer_or_tag_symbol);
    RUN_TEST(test_the_menu_defines_menu_open);
    RUN_TEST(test_no_exit_path_symbol_under_src);
    return UNITY_END();
}
