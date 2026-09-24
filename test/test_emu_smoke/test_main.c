#include <unity.h>

#include <stdio.h>
#include <stdlib.h>

#include "gnuboy_runner.h"

#define ROM_PATH "test/roms/dmg-acid2.gb"
#define ROM_MAX (1024 * 1024)

static uint8_t rom[ROM_MAX];
static size_t rom_len;

void setUp(void)
{
}

void tearDown(void)
{
}

static void load_rom(void)
{
    FILE* f = fopen(ROM_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not open " ROM_PATH
        " — is the test running from the project root?");
    rom_len = fread(rom, 1, sizeof(rom), f);
    fclose(f);
    TEST_ASSERT_GREATER_THAN(0, rom_len);
}

static void test_dmg_acid2_boots_and_runs_60_frames_clean(void)
{
    const uint8_t* frame;
    size_t i;
    unsigned distinct;
    unsigned seen[256] = { 0 };

    load_rom();
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(60));
    TEST_ASSERT_EQUAL_UINT(0, gnuboy_runner_error_count());

    frame = gnuboy_runner_frame();
    TEST_ASSERT_NOT_NULL(frame);
    distinct = 0;
    for (i = 0; i < (size_t)GNUBOY_RUNNER_W * GNUBOY_RUNNER_H; i++) {
        if (!seen[frame[i]]) {
            seen[frame[i]] = 1;
            distinct++;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, distinct);
}

/* gnuboy's sound unit writes one frame of output per emulated frame. Whether
 * dmg-acid2 makes any sound is not asserted — it is unknown, and not what this
 * test is for; what matters is that the stream exists and has the geometry the
 * mixer expects. The count follows the frame's real emulated length, so it is
 * 548 or 549 at the speaker's 32768 Hz; a wrong rate would be off by a factor. */
static void test_a_run_captures_a_frame_of_sound(void)
{
    load_rom();
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(60));

    TEST_ASSERT_EQUAL_UINT(0, gnuboy_runner_error_count());
    TEST_ASSERT_NOT_NULL(gnuboy_runner_audio());
    TEST_ASSERT_UINT_WITHIN(1, 548, gnuboy_runner_audio_samples());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dmg_acid2_boots_and_runs_60_frames_clean);
    RUN_TEST(test_a_run_captures_a_frame_of_sound);
    return UNITY_END();
}
