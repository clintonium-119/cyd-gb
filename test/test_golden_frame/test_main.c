#include <unity.h>

#include <stdio.h>

#include "gb_runner.h"

/*
 * Golden-frame regression pin.
 *
 * GOLDEN_FRAME_HASH is FNV-1a 64 over the raw 160x144 index buffer after
 * running dmg-acid2 for GOLDEN_FRAME_COUNT frames. It pins Peanut-GB's OWN
 * output under the firmware's emulator flags (ENABLE_LCD=1, ENABLE_SOUND=0,
 * PEANUT_GB_HIGH_LCD_ACCURACY=0, PEANUT_GB_USE_DOUBLE_WIDTH_PALETTE=0) — it
 * is NOT the official dmg-acid2 reference image, and it is only meaningful
 * while [env:native]'s flags mirror [env:cyd]'s.
 *
 * An intentional rendering change updates the constant in the same commit,
 * with the reason in the commit body. dmg-acid2's output stabilises at
 * frame 10 (measured 2026-08-31, constant through frame 180); 60 leaves a
 * wide margin.
 */
#define GOLDEN_FRAME_COUNT 60u
#define GOLDEN_FRAME_HASH 0xd6473e074b948ceaULL

#define ROM_PATH "test/roms/dmg-acid2.gb"
#define ROM_MAX (1024 * 1024)

static uint8_t rom[ROM_MAX];

void setUp(void)
{
}

void tearDown(void)
{
}

static uint64_t fnv1a64(const uint8_t* buf, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void test_golden_frame_hash_matches(void)
{
    FILE* f;
    size_t rom_len;

    f = fopen(ROM_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not open " ROM_PATH
        " — is the test running from the project root?");
    rom_len = fread(rom, 1, sizeof(rom), f);
    fclose(f);

    TEST_ASSERT_EQUAL_INT(GB_RUNNER_OK, gb_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GB_RUNNER_OK,
        gb_runner_run_frames(GOLDEN_FRAME_COUNT));
    TEST_ASSERT_EQUAL_UINT(0, gb_runner_error_count());

    TEST_ASSERT_EQUAL_HEX64_MESSAGE(GOLDEN_FRAME_HASH,
        fnv1a64(gb_runner_frame(), (size_t)GB_RUNNER_W * GB_RUNNER_H),
        "Peanut-GB's dmg-acid2 output changed. If the rendering change is "
        "intentional, update GOLDEN_FRAME_HASH in the same commit and say "
        "why in the commit body; otherwise this is a regression. Check "
        "first that [env:native]'s emulator flags still mirror [env:cyd]'s.");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_frame_hash_matches);
    return UNITY_END();
}
