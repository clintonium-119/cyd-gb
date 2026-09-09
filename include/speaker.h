#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Audio output on IO26 (design §1.6, §4).
//
// The ESP32's built-in DAC is driven through the I2S peripheral rather than a
// timer interrupt: I2S0 in built-in-DAC mode takes a DMA chain of buffers and
// clocks them out on its own, so the DMA queue IS the sample buffer and this
// module owns no ring of its own. Only the left channel is enabled, which is
// DAC channel 2 on IO26; IO25 is not ours and stays untouched.
//
// The DAC reads the high byte of each 16-bit word as an unsigned level, so a
// frame of mono bytes is expanded here into stereo words with the sample in
// the high byte. 128 is mid-scale — silence — and 0 is not: a zero-filled
// buffer would swing the pin to 0 V, which is why the queue is primed with
// 128 rather than zeroed and why tx_desc_auto_clear stays false.
//
// speaker_write_frame() blocks only when the DMA queue is full, which happens
// only when the emulator is ahead of real time; that bounded block is the
// pacing rule, and it is the only throttle audio applies. It never slows a
// frame rate the pipeline could not already reach.
//
// There is no hardware mute on this board, so volume "off" is not this
// module's concern: the mixer hands it a frame of 128s and the pin sits at
// mid-scale.
//
// Threading: all four functions are called from the emulation task on core 1
// and none is reentrant. The DMA completion interrupt belongs to the I2S
// driver; nothing here runs in interrupt context.

// Parks the DAC at mid-scale, installs I2S0 and primes the queue with
// silence. False when the driver refuses — the caller logs and carries on,
// and every other call here becomes a no-op, so the unit plays silently
// rather than halting.
bool speaker_init();

// One frame of unsigned 8-bit mono, mid-scale at 128. n_samples must not
// exceed SPEAKER_SAMPLES_PER_FRAME. Blocks for at most
// SPEAKER_WRITE_TIMEOUT_MS waiting for room in the DMA queue.
void speaker_write_frame(const uint8_t* mono, size_t n_samples);

// Fills every DMA buffer with mid-scale and stops the level clock, so a
// deliberate pause — the menu, a save flush — leaves the pin quiet instead of
// repeating the last buffer, and is not counted as an underflow.
void speaker_silence();

// Counters since boot, plus the microseconds the last write spent blocked.
// Any pointer may be NULL, like emu_get_frame_times().
//
// underflows: the queue was estimated empty at the moment a frame arrived —
// the emulator fell behind. overflows: a write placed fewer bytes than it was
// given before its timeout expired.
void speaker_get_stats(uint32_t* underflows, uint32_t* overflows,
                       uint32_t* last_wait_us);
