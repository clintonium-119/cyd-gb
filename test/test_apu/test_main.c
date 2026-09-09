#include <unity.h>

#include <string.h>

#include "minigb_apu.h"

/*
 * Vendored MiniGB APU integration.
 *
 * Pins the frame geometry the speaker's constants mirror (548 samples per
 * channel, 1096 interleaved), and proves the vendored code behaves the way
 * the mixer downstream assumes: silence from a fresh context, sound from a
 * triggered channel, and hard panning that the mono sum later rescues.
 * Nothing here calls the emulator — the APU is driven through its register
 * interface, because no sound test ROM with a permissive licence is
 * available.
 */

#define GUARD 8

/* Interleaved stereo: even index is left, odd is right. */
#define LEFT_CH 0u
#define RIGHT_CH 1u

/* The header derives AUDIO_SAMPLES from a double division, so it is not a
 * constant expression and cannot size a static array. These literals do,
 * and test_frame_geometry_is_548_samples_per_channel pins them against the
 * header's own values. */
#define FRAME_SAMPLES 548u
#define FRAME_SAMPLES_TOTAL 1096u

static struct {
    audio_sample_t pre[GUARD];
    audio_sample_t out[FRAME_SAMPLES_TOTAL];
    audio_sample_t post[GUARD];
} g;

static struct minigb_apu_ctx apu;

void setUp(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        g.pre[i] = g.post[i] = (audio_sample_t)0x5C5C;
    }
    memset(g.out, 0x7E, sizeof(g.out));
    memset(&apu, 0, sizeof(apu));
    minigb_apu_audio_init(&apu);
}

void tearDown(void)
{
}

static void assert_guards_intact(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX16((audio_sample_t)0x5C5C, g.pre[i]);
        TEST_ASSERT_EQUAL_HEX16((audio_sample_t)0x5C5C, g.post[i]);
    }
}

/* Power on, both sides at full master volume, square 1 at full envelope
 * volume, triggered without its length counter. `panning` goes to NR51. */
static void trigger_square_one(uint8_t panning)
{
    minigb_apu_audio_write(&apu, 0xFF26, 0x80); /* NR52: APU powered */
    minigb_apu_audio_write(&apu, 0xFF24, 0x77); /* NR50: master 7 / 7 */
    minigb_apu_audio_write(&apu, 0xFF25, panning); /* NR51 */
    minigb_apu_audio_write(&apu, 0xFF12, 0xF0); /* NR12: volume 15, no envelope */
    minigb_apu_audio_write(&apu, 0xFF13, 0x00); /* NR13: frequency low */
    minigb_apu_audio_write(&apu, 0xFF14, 0x87); /* NR14: trigger, length off */
}

static int count_nonzero(unsigned channel_offset)
{
    unsigned i;
    int n = 0;
    for (i = 0; i < FRAME_SAMPLES; i++) {
        if (g.out[i * 2 + channel_offset] != 0) {
            n++;
        }
    }
    return n;
}

/* The speaker's per-frame buffer sizes are derived from these two, so a
 * change in either upstream must fail here rather than silently resize the
 * DMA frame. */
static void test_frame_geometry_is_548_samples_per_channel(void)
{
    TEST_ASSERT_EQUAL_UINT(FRAME_SAMPLES, AUDIO_SAMPLES);
    TEST_ASSERT_EQUAL_UINT(FRAME_SAMPLES_TOTAL, AUDIO_SAMPLES_TOTAL);
    TEST_ASSERT_EQUAL_UINT(2u, AUDIO_CHANNELS);
}

static void test_fresh_context_yields_one_frame_of_silence(void)
{
    unsigned i;
    minigb_apu_audio_callback(&apu, g.out);
    for (i = 0; i < FRAME_SAMPLES_TOTAL; i++) {
        TEST_ASSERT_EQUAL_HEX16(0, g.out[i]);
    }
    assert_guards_intact();
}

static void test_triggered_square_sounds_on_both_sides(void)
{
    trigger_square_one(0x11); /* channel 1 to left and right */
    minigb_apu_audio_callback(&apu, g.out);

    TEST_ASSERT_GREATER_THAN_INT(0, count_nonzero(LEFT_CH));
    TEST_ASSERT_GREATER_THAN_INT(0, count_nonzero(RIGHT_CH));
    assert_guards_intact();
}

static void test_hard_left_panning_zeroes_the_right_channel(void)
{
    unsigned i;
    trigger_square_one(0x10); /* channel 1 to left only */
    minigb_apu_audio_callback(&apu, g.out);

    TEST_ASSERT_GREATER_THAN_INT(0, count_nonzero(LEFT_CH));
    for (i = 0; i < FRAME_SAMPLES; i++) {
        TEST_ASSERT_EQUAL_HEX16(0, g.out[i * 2 + RIGHT_CH]);
    }
    assert_guards_intact();
}

static void test_nr52_reports_power_and_unused_registers_read_as_ones(void)
{
    /* NR52 bit 7 is the power bit; bits 4-6 read back as 1. */
    TEST_ASSERT_BITS_HIGH(0x80, minigb_apu_audio_read(&apu, 0xFF26));
    /* 0xFF15 has no register behind it. */
    TEST_ASSERT_EQUAL_HEX8(0xFF, minigb_apu_audio_read(&apu, 0xFF15));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_geometry_is_548_samples_per_channel);
    RUN_TEST(test_fresh_context_yields_one_frame_of_silence);
    RUN_TEST(test_triggered_square_sounds_on_both_sides);
    RUN_TEST(test_hard_left_panning_zeroes_the_right_channel);
    RUN_TEST(test_nr52_reports_power_and_unused_registers_read_as_ones);
    return UNITY_END();
}
