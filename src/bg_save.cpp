#include "bg_save.h"
#include "sd_manager.h"
#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_timer.h>

// Up to four 8 KB pieces: a 32 KB save in instruction RAM, which has no
// single 32 KB block free during play either.
#define PIECE_BYTES 8192u
#define PIECE_WORDS (PIECE_BYTES / 4u)
#define PIECES_MAX  4u

// Priority 0, so the writer only ever runs in time nobody else wants: the
// push task (priority 3) preempts it, and the SD driver's busy-waits share
// core 0 with the idle task by time slicing, which keeps the core-0 task
// watchdog fed even when a sick card stalls a write for seconds. Measured on
// the bench: 8 s of spinning here did not trip it, and the game held 60 fps.
#define WRITER_PRIORITY tskIDLE_PRIORITY
// 1,456 bytes of 4,096 were left unused at the deepest point of a write on
// the bench.
#define WRITER_STACK 4096

static uint32_t* pieces[PIECES_MAX];
static uint32_t save_bytes = 0;
static bool ready = false;
static TaskHandle_t writer = nullptr;
static char path[96];

static std::atomic<bool> busy(false);
static std::atomic<uint8_t> result(BG_SAVE_NONE);

static void fill_from_pieces(void*, uint32_t off, uint8_t* dst, uint32_t n) {
    // Word loads only: the pieces may be 32-bit-only memory. `dst` is
    // sd_save_stream()'s word-aligned block, and every offset and length
    // it asks for is a whole number of words.
    uint32_t* out = (uint32_t*)dst;
    uint32_t w = off / 4u;
    for (uint32_t i = 0; i < n / 4u; i++, w++) {
        out[i] = pieces[w / PIECE_WORDS][w % PIECE_WORDS];
    }
}

static void writer_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        bool ok = sd_save_stream(path, save_bytes, fill_from_pieces, nullptr);
        Serial.printf("[SAVE] bg: %u bytes in %u ms (%s)\n", (unsigned)save_bytes,
                      (unsigned)((esp_timer_get_time() - t0) / 1000),
                      ok ? "ok" : "fail");
        // The result before the flag, so whoever sees idle sees the outcome.
        result.store(ok ? BG_SAVE_OK : BG_SAVE_FAILED);
        busy.store(false);
    }
}

bool bg_save_init(uint32_t size) {
    if (size == 0 || size % 4u != 0 || size > PIECES_MAX * PIECE_BYTES) {
        Serial.printf("[SAVE] bg: off, size %u\n", (unsigned)size);
        return false;
    }
    uint32_t n = (size + PIECE_BYTES - 1) / PIECE_BYTES;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t bytes = (i + 1 < n) ? PIECE_BYTES : size - i * PIECE_BYTES;
        // Instruction RAM first: it is what is left during play. The
        // byte-addressable heap only if that is gone, so the menu keeps it.
        pieces[i] = (uint32_t*)heap_caps_malloc(bytes, MALLOC_CAP_EXEC);
        if (!pieces[i]) {
            pieces[i] = (uint32_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        if (!pieces[i]) {
            Serial.printf("[SAVE] bg: off, piece %u of %u not allocated\n",
                          (unsigned)(i + 1), (unsigned)n);
            for (uint32_t j = 0; j < i; j++) {
                heap_caps_free(pieces[j]);
                pieces[j] = nullptr;
            }
            return false;
        }
    }
    if (!writer
        && xTaskCreatePinnedToCore(writer_task, "bgsave", WRITER_STACK, nullptr,
                                   WRITER_PRIORITY, &writer, 0) != pdPASS) {
        Serial.println("[SAVE] bg: off, writer task not created");
        for (uint32_t i = 0; i < n; i++) {
            heap_caps_free(pieces[i]);
            pieces[i] = nullptr;
        }
        return false;
    }
    save_bytes = size;
    ready = true;
    Serial.printf("[SAVE] bg: on, %u bytes in %u pieces\n", (unsigned)size,
                  (unsigned)n);
    return true;
}

bool bg_save_ready() {
    return ready;
}

bool bg_save_idle() {
    return !busy.load();
}

void bg_save_take(const uint8_t* ram) {
    // Word stores only, for the same reason as the loads. Cartridge RAM
    // comes from calloc, so it is word-aligned. About 1 ms for 32 KB.
    const uint32_t* src = (const uint32_t*)ram;
    for (uint32_t w = 0; w < save_bytes / 4u; w++) {
        pieces[w / PIECE_WORDS][w % PIECE_WORDS] = src[w];
    }
}

void bg_save_start(const char* rom_path) {
    strncpy(path, rom_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    busy.store(true);
    xTaskNotifyGive(writer);
}

void bg_save_wait() {
    while (busy.load()) {
        vTaskDelay(1);
    }
}

uint8_t bg_save_result() {
    return result.exchange(BG_SAVE_NONE);
}
