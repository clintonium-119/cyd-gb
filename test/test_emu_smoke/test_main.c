#include <unity.h>

#include <stdio.h>
#include <stdlib.h>

#include "gb_runner.h"

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
    TEST_ASSERT_EQUAL_INT(GB_RUNNER_OK, gb_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GB_RUNNER_OK, gb_runner_run_frames(60));
    TEST_ASSERT_EQUAL_UINT(0, gb_runner_error_count());

    frame = gb_runner_frame();
    TEST_ASSERT_NOT_NULL(frame);
    distinct = 0;
    for (i = 0; i < (size_t)GB_RUNNER_W * GB_RUNNER_H; i++) {
        if (!seen[frame[i]]) {
            seen[frame[i]] = 1;
            distinct++;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, distinct);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dmg_acid2_boots_and_runs_60_frames_clean);
    return UNITY_END();
}
