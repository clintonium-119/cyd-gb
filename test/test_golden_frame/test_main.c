#include <unity.h>

#include <stdio.h>

#include "gnuboy_runner.h"

/*
 * Golden-frame regression pin.
 *
 * GOLDEN_FRAME_HASH is FNV-1a 64 over the raw 160x144 index buffer after
 * running dmg-acid2 for GOLDEN_FRAME_COUNT frames, followed by the three DMG
 * palette registers. gnuboy's pixel byte carries the tile's raw two bits and
 * applies BGP / OBP0 / OBP1 only when the colour table is built, so the
 * buffer alone does not say what the frame looks like; the registers do. It
 * pins gnuboy's OWN output — it is NOT the official dmg-acid2 reference
 * image.
 *
 * An intentional rendering change updates the constant in the same commit,
 * with the reason in the commit body. The picture is checked to have settled
 * by frame 60 in test_gnuboy_core.
 */
#define GOLDEN_FRAME_COUNT 60u
#define GOLDEN_FRAME_HASH 0x0fc5b5c4ee79097aULL

#define ROM_PATH "test/roms/dmg-acid2.gb"
#define ROM_MAX (1024 * 1024)

static uint8_t rom[ROM_MAX];

void setUp(void)
{
}

void tearDown(void)
{
}

static uint64_t fnv1a64(uint64_t h, const uint8_t* buf, size_t len)
{
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
    uint8_t regs[3];
    uint64_t h;

    f = fopen(ROM_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not open " ROM_PATH
        " — is the test running from the project root?");
    rom_len = fread(rom, 1, sizeof(rom), f);
    fclose(f);

    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK,
        gnuboy_runner_run_frames(GOLDEN_FRAME_COUNT));
    TEST_ASSERT_EQUAL_UINT(0, gnuboy_runner_error_count());

    regs[0] = gnuboy_runner_bgp();
    regs[1] = gnuboy_runner_obp0();
    regs[2] = gnuboy_runner_obp1();
    h = fnv1a64(0xcbf29ce484222325ULL, gnuboy_runner_frame(),
                (size_t)GNUBOY_RUNNER_W * GNUBOY_RUNNER_H);
    h = fnv1a64(h, regs, sizeof(regs));

    TEST_ASSERT_EQUAL_HEX64_MESSAGE(GOLDEN_FRAME_HASH, h,
        "gnuboy's dmg-acid2 output changed. If the rendering change is "
        "intentional, update GOLDEN_FRAME_HASH in the same commit and say "
        "why in the commit body; otherwise this is a regression.");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_frame_hash_matches);
    return UNITY_END();
}
