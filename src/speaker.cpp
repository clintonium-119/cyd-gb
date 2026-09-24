#include "speaker.h"
#include "hw_config.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/dac.h>
#include <esp_timer.h>

#include "audio/level.h"

// The expanded frame handed to the driver: one 16-bit word per channel, the
// sample in the high byte. Sized for SPEAKER_SAMPLES_MAX rather than the
// nominal frame so a core whose per-frame count varies can hand over
// everything it generated instead of having the surplus truncated away.
// Static rather than stack — the emulation task should not carry this — and
// the only buffer this module owns; the DMA chain itself is allocated once by
// i2s_driver_install().
static uint16_t frame[2 * SPEAKER_SAMPLES_MAX];

// Queue depth is estimated, not observed: built-in-DAC mode reports neither a
// fill level nor a starvation event.
static level_t lvl;

static uint32_t underflow_count = 0;
static uint32_t overflow_count = 0;
static uint32_t last_wait_us = 0;
static bool ready = false;

// One 16-bit word per channel, two channels.
#define SPEAKER_BYTES_PER_SAMPLE 4

// A write that took this long waited for a DMA buffer to come free, so the
// queue was full when it returned. Copying a frame into a free buffer takes
// tens of microseconds; a wait for one is up to a frame, 16.7 ms.
#define SPEAKER_BLOCKED_US 1000u

// Mid-scale in the DAC's own encoding: 128 in the high byte.
#define SPEAKER_SILENCE_WORD ((uint16_t)(128u << 8))

// Fills the whole frame buffer with mid-scale and pushes it, so no caller can
// hand the DMA chain a zero-filled buffer by accident.
static void write_silence_frames(unsigned n_frames) {
    /* Nominal frames, not the buffer's full size: this primes and drains the
     * DMA chain, and the chain's buffers are SPEAKER_SAMPLES_PER_FRAME long.
     * The extra headroom above exists for a caller's overshoot, not here. */
    const size_t bytes = SPEAKER_SAMPLES_PER_FRAME * SPEAKER_BYTES_PER_SAMPLE;

    for (size_t i = 0; i < 2 * SPEAKER_SAMPLES_PER_FRAME; i++) {
        frame[i] = SPEAKER_SILENCE_WORD;
    }
    for (unsigned f = 0; f < n_frames; f++) {
        size_t written = 0;
        i2s_write(I2S_NUM_0, frame, bytes, &written,
                  pdMS_TO_TICKS(SPEAKER_WRITE_TIMEOUT_MS));
    }
}

bool speaker_init() {
    gpio_num_t dac_gpio;

    // The I2S API selects a DAC channel, not a pin. Checking the mapping here
    // is what makes SPEAKER_DAC_PIN more than a comment.
    if (dac_pad_get_io_num(DAC_CHANNEL_2, &dac_gpio) != ESP_OK
        || (int)dac_gpio != SPEAKER_DAC_PIN) {
        Serial.printf("[SPK] DAC channel 2 is not IO%d\n", SPEAKER_DAC_PIN);
        return false;
    }

    // Mid-scale before the I2S path takes the pin, so the amplifier — which
    // is always live — never sees the swing from 0 V.
    dac_output_enable(DAC_CHANNEL_2);
    dac_output_voltage(DAC_CHANNEL_2, 128);

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
    cfg.sample_rate = SPEAKER_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = SPEAKER_DMA_FRAMES;
    cfg.dma_buf_len = SPEAKER_SAMPLES_PER_FRAME;
    cfg.use_apll = false;
    // A starved chain repeats its last buffer instead of zero-filling it.
    // Zero is 0 V on this DAC, not silence, so the repeat is the quieter
    // failure; speaker_silence() is what makes the repeated buffer mid-scale.
    cfg.tx_desc_auto_clear = false;

    // I2S0 is the only port wired to the built-in DAC.
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[SPK] i2s_driver_install failed (%d)\n", (int)err);
        return false;
    }

    // Left channel only: GPIO26. GPIO25 is not ours.
    err = i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
    if (err != ESP_OK) {
        Serial.printf("[SPK] i2s_set_dac_mode failed (%d)\n", (int)err);
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    level_init(&lvl, SPEAKER_SAMPLE_RATE,
               SPEAKER_DMA_FRAMES * SPEAKER_SAMPLES_PER_FRAME);
    ready = true;
    speaker_silence();

    Serial.printf("[SPK] I2S DAC on IO%d %u Hz depth %u\n", SPEAKER_DAC_PIN,
                  (unsigned)SPEAKER_SAMPLE_RATE, (unsigned)SPEAKER_DMA_FRAMES);
    return true;
}

void speaker_write_frame(const uint8_t* mono, size_t n_samples) {
    if (!ready || mono == nullptr || n_samples == 0) {
        return;
    }
    if (n_samples > SPEAKER_SAMPLES_MAX) {
        n_samples = SPEAKER_SAMPLES_MAX;
    }

    int64_t now_us = esp_timer_get_time();

    // The queue having run dry before this frame arrived is the emulator
    // falling behind, not a pause: a pause goes through speaker_silence(),
    // which stops the clock.
    if (level_running(&lvl) && level_queued(&lvl, now_us) == 0) {
        underflow_count++;
    }

    // The DAC takes the high byte as an unsigned level; both channels carry
    // the same sample because only the left one is enabled and the right
    // word still has to be clocked out.
    for (size_t i = 0; i < n_samples; i++) {
        uint16_t word = (uint16_t)mono[i] << 8;
        frame[i * 2 + 0] = word;
        frame[i * 2 + 1] = word;
    }

    size_t bytes = n_samples * SPEAKER_BYTES_PER_SAMPLE;
    size_t written = 0;
    i2s_write(I2S_NUM_0, frame, bytes, &written,
              pdMS_TO_TICKS(SPEAKER_WRITE_TIMEOUT_MS));

    last_wait_us = (uint32_t)(esp_timer_get_time() - now_us);
    if (written < bytes) {
        overflow_count++;
    }

    level_note_write(&lvl, (uint32_t)(written / SPEAKER_BYTES_PER_SAMPLE),
                     now_us);
    if (last_wait_us >= SPEAKER_BLOCKED_US) {
        level_note_full(&lvl, now_us + (int64_t)last_wait_us);
    }
}

void speaker_silence() {
    if (!ready) {
        return;
    }
    // One more write than there are buffers: the extra one guarantees that
    // every buffer in the chain has been overwritten, whatever the chain's
    // current position.
    write_silence_frames(SPEAKER_DMA_FRAMES + 1);
    level_reset(&lvl);
}

void speaker_get_stats(uint32_t* underflows, uint32_t* overflows,
                       uint32_t* last_wait) {
    if (underflows != nullptr) {
        *underflows = underflow_count;
    }
    if (overflows != nullptr) {
        *overflows = overflow_count;
    }
    if (last_wait != nullptr) {
        *last_wait = last_wait_us;
    }
}
