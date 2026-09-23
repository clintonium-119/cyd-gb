#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "gnuboy.h"
#include "gnuboy_runner.h"

/*
 * Save state round trip on the vendored gnuboy core: save, run, load, run the
 * same frames again, and the machine must repeat itself byte for byte.
 *
 * The fixture is dmg-acid2, the only ROM in the tree. It has no battery, but
 * that does not weaken the cart RAM assertion: gnuboy gives every cartridge at
 * least one 8 KB bank and a state carries all of them, battery or not. The
 * test plants a pattern in that RAM before the save and scribbles over it
 * afterwards, which is the case a battery game exercises by writing its save.
 *
 * dmg-acid2 draws one still picture from the first run on, so identical frames
 * alone would prove little. The machine underneath does move — CPU, timers,
 * work RAM, the sound unit's counters — so each pass also saves a second
 * state at its end, and the two must be the same file byte for byte. The
 * audio is silent in dmg-acid2, so the sample comparison mostly checks the
 * per-frame sample count, which alternates and so does carry the timing.
 */

#define ROM_PATH "test/roms/dmg-acid2.gb"
#define ROM_MAX (1024 * 1024)
#define STATE_PATH "test_savestate.state"
#define END_PATH "test_savestate_end.state"
#define STATE_MAX (64u * 1024u)

/*
 * Where the boot ROM register, FF50, sits in a state: the I/O page is stored
 * at 0xD00. gnuboy's load sets it to 1 unconditionally, to keep old states
 * that predate the register from re-entering the boot ROM. The runner loads
 * no boot ROM, so the register reads 0 before a load and 1 after, and with no
 * boot ROM mapped it changes nothing. The firmware runs its boot ROM to the
 * end before a game is playable, and the boot ROM's last act is to write 1.
 */
#define STATE_BOOT_REG (0xD00u + 0x50u)

#define SAVE_AT 2u
#define RUN_ON 10u

#define FW GNUBOY_RUNNER_W
#define FH GNUBOY_RUNNER_H
#define AUDIO_MAX 2048u

static uint8_t rom[ROM_MAX];
static size_t rom_len;

/* What the machine produced in the frames after the save, recorded on the
 * first pass and compared against on the second. */
static uint8_t frames_after[RUN_ON][FW * FH];
static int16_t audio_after[RUN_ON][AUDIO_MAX];
static unsigned samples_after[RUN_ON];
static uint8_t state_saved[STATE_MAX];
static uint8_t state_end[STATE_MAX];

void setUp(void)
{
}

void tearDown(void)
{
    remove(STATE_PATH);
    remove(END_PATH);
}

static void load_rom(void)
{
    FILE* f = fopen(ROM_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not open " ROM_PATH
        " — is the test running from the project root?");
    rom_len = fread(rom, 1, sizeof(rom), f);
    fclose(f);
    TEST_ASSERT_GREATER_THAN_UINT32(0x150u, (uint32_t)rom_len);
}

static size_t read_file(const char* path, uint8_t* out)
{
    FILE* f = fopen(path, "rb");
    size_t n;

    TEST_ASSERT_NOT_NULL(f);
    n = fread(out, 1, STATE_MAX, f);
    fclose(f);
    TEST_ASSERT_GREATER_THAN_UINT32(0, (uint32_t)n);
    TEST_ASSERT_LESS_THAN_UINT32(STATE_MAX, (uint32_t)n);
    return n;
}

static void boot_and_save(void)
{
    load_rom();
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(SAVE_AT));
    TEST_ASSERT_EQUAL_INT(0, gnuboy_save_state(STATE_PATH));
}

static void record_frame(unsigned i)
{
    unsigned n = gnuboy_runner_audio_samples() * 2u;

    TEST_ASSERT_LESS_OR_EQUAL_UINT32(AUDIO_MAX, n);
    memcpy(frames_after[i], gnuboy_runner_frame(), FW * FH);
    memcpy(audio_after[i], gnuboy_runner_audio(), n * sizeof(int16_t));
    samples_after[i] = n;
}

static void check_frame(unsigned i)
{
    unsigned n = gnuboy_runner_audio_samples() * 2u;
    char msg[48];

    snprintf(msg, sizeof(msg), "frame %u after the load", i);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(frames_after[i], gnuboy_runner_frame(),
                                     FW * FH, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(samples_after[i], n, msg);
    if (n > 0) {
        TEST_ASSERT_EQUAL_INT16_ARRAY_MESSAGE(audio_after[i],
                                              gnuboy_runner_audio(), n, msg);
    }
}

static void test_a_load_repeats_the_frames_and_audio_after_the_save(void)
{
    static uint8_t replayed[STATE_MAX];
    size_t saved_len;
    size_t end_len;
    unsigned i;

    boot_and_save();
    saved_len = read_file(STATE_PATH, state_saved);
    for (i = 0; i < RUN_ON; i++) {
        TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(1));
        record_frame(i);
    }
    TEST_ASSERT_EQUAL_INT(0, gnuboy_save_state(END_PATH));
    end_len = read_file(END_PATH, state_end);
    /* Otherwise the comparison below proves nothing. */
    TEST_ASSERT_EQUAL_UINT32(saved_len, end_len);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(state_saved, state_end, end_len) != 0,
        "the machine did not move after the save");

    TEST_ASSERT_EQUAL_INT(0, gnuboy_load_state(STATE_PATH));
    for (i = 0; i < RUN_ON; i++) {
        TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(1));
        check_frame(i);
    }
    TEST_ASSERT_EQUAL_INT(0, gnuboy_save_state(END_PATH));
    TEST_ASSERT_EQUAL_UINT32(end_len, read_file(END_PATH, replayed));
    TEST_ASSERT_EQUAL_HEX8(1, replayed[STATE_BOOT_REG]);
    replayed[STATE_BOOT_REG] = state_end[STATE_BOOT_REG];
    TEST_ASSERT_EQUAL_MEMORY(state_end, replayed, end_len);
}

static void test_a_load_restores_the_cart_ram_at_save_time(void)
{
    static uint8_t planted[8192];
    size_t len = 0;
    uint8_t* ram;
    size_t i;

    load_rom();
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(SAVE_AT));
    ram = (uint8_t*)gnuboy_runner_cart_ram(&len);
    TEST_ASSERT_NOT_NULL(ram);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(sizeof(planted), (uint32_t)len);

    for (i = 0; i < sizeof(planted); i++) {
        planted[i] = (uint8_t)(i * 7u + 3u);
    }
    memcpy(ram, planted, sizeof(planted));
    TEST_ASSERT_EQUAL_INT(0, gnuboy_save_state(STATE_PATH));

    memset(ram, 0xA5, len);
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(RUN_ON));
    TEST_ASSERT_EQUAL_INT(0, gnuboy_load_state(STATE_PATH));
    TEST_ASSERT_EQUAL_MEMORY(planted, ram, sizeof(planted));
}

static void test_loading_a_missing_file_fails_and_the_machine_runs_on(void)
{
    static uint8_t before[FW * FH];

    load_rom();
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(SAVE_AT));
    memcpy(before, gnuboy_runner_frame(), sizeof(before));

    remove(STATE_PATH);
    TEST_ASSERT_NOT_EQUAL_INT(0, gnuboy_load_state(STATE_PATH));
    /* Nothing was touched: the fopen fails before any block is read. */
    TEST_ASSERT_EQUAL_MEMORY(before, gnuboy_runner_frame(), sizeof(before));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(RUN_ON));
    TEST_ASSERT_EQUAL_UINT(0, gnuboy_runner_error_count());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_load_repeats_the_frames_and_audio_after_the_save);
    RUN_TEST(test_a_load_restores_the_cart_ram_at_save_time);
    RUN_TEST(test_loading_a_missing_file_fails_and_the_machine_runs_on);
    return UNITY_END();
}
