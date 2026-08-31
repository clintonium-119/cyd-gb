#include "rom_store.h"
#include <Arduino.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>

// ─── Partition lookup ───────────────────────────────────────────────────────
// These three must match the romdata row of partitions.csv character for
// character — a mismatch is invisible at build time and shows up on the bench
// as "partition missing", which is exactly how this goes wrong in other
// projects. The row, verbatim:
//
//   romdata,  data, 0x40,    0xB0000,  0x350000,
//
#define ROMDATA_TYPE ESP_PARTITION_TYPE_DATA
#define ROMDATA_SUBTYPE 0x40
#define ROMDATA_LABEL "romdata"

// Bytes streamed per write_chunk call. Also the progress-report unit below.
#define IO_CHUNK 4096

static const esp_partition_t* part = nullptr;
static rom_store_flash_t flash;
static const uint8_t* mapped = nullptr;
static uint32_t mapped_len = 0;
static spi_flash_mmap_handle_t map_handle = 0;

// ─── Flash ops ──────────────────────────────────────────────────────────────
// The gbcore store speaks partition-relative offsets and 0/non-zero results;
// these three adapt esp_partition_* to that seam and hold no logic.
static int part_read(void* ctx, uint32_t off, void* dst, uint32_t len)
{
    (void)ctx;
    return (esp_partition_read(part, off, dst, len) == ESP_OK) ? 0 : -1;
}

static int part_write(void* ctx, uint32_t off, const void* src, uint32_t len)
{
    (void)ctx;
    return (esp_partition_write(part, off, src, len) == ESP_OK) ? 0 : -1;
}

static int part_erase(void* ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    return (esp_partition_erase_range(part, off, len) == ESP_OK) ? 0 : -1;
}

// ─── API ────────────────────────────────────────────────────────────────────
bool rom_store_init()
{
    if (part) {
        return true;
    }
    part = esp_partition_find_first(ROMDATA_TYPE,
                                    (esp_partition_subtype_t)ROMDATA_SUBTYPE,
                                    ROMDATA_LABEL);
    if (!part) {
        Serial.println("[ROM] partition missing");
        return false;
    }
    flash.ctx = nullptr;
    flash.read = part_read;
    flash.write = part_write;
    flash.erase = part_erase;
    flash.size = part->size;
    flash.erase_size = SPI_FLASH_SEC_SIZE;
    Serial.printf("[ROM] partition %uKB @0x%06X\n", part->size / 1024,
                  part->address);
    return true;
}

bool rom_store_stored(rom_store_hdr_t* out)
{
    if (!part) {
        return false;
    }
    return rom_store_read_header(&flash, out);
}

bool rom_store_write(fs::File& f, const char* filename)
{
    rom_store_hdr_t hdr;
    rom_store_writer_t writer;
    uint8_t* buf;
    uint32_t size;
    uint32_t done = 0;
    uint32_t next_report = 65536;
    int rc;

    if (!part || !f) {
        return false;
    }
    size = (uint32_t)f.size();
    if (size == 0) {
        Serial.println("[ROM] source file is empty");
        return false;
    }

    // The whole point of the header: a ROM already in flash is not written
    // again, so boot costs one read instead of a full erase/write cycle and
    // the partition's erase budget is spent once per cartridge, not once per
    // power-up.
    if (rom_store_read_header(&flash, &hdr) &&
        rom_store_matches(&hdr, filename, size)) {
        Serial.printf("[ROM] unchanged, skipping write (%uKB)\n", size / 1024);
        return true;
    }

    // Boot-path buffer, released before emulation starts: holding 4 KB of
    // DRAM for the rest of the session would give back part of what mapping
    // the ROM just reclaimed.
    buf = (uint8_t*)malloc(IO_CHUNK);
    if (!buf) {
        Serial.println("[ROM] no memory for the copy buffer");
        return false;
    }

    rc = rom_store_write_begin(&flash, &writer, filename, size);
    if (rc != ROM_STORE_OK) {
        Serial.printf("[ROM] write refused (%d), %uKB into %uKB\n", rc,
                      size / 1024, part->size / 1024);
        free(buf);
        return false;
    }
    Serial.printf("[ROM] writing %uKB...\n", size / 1024);

    while (done < size) {
        uint32_t want = size - done;
        size_t got;

        if (want > IO_CHUNK) {
            want = IO_CHUNK;
        }
        got = f.read(buf, want);
        if (got == 0) {
            Serial.printf("[ROM] source ended early at %u bytes\n", done);
            free(buf);
            return false;
        }
        rc = rom_store_write_chunk(&writer, buf, (uint32_t)got);
        if (rc != ROM_STORE_OK) {
            Serial.printf("[ROM] write failed (%d) at %u bytes\n", rc, done);
            free(buf);
            return false;
        }
        done += (uint32_t)got;
        if (done >= next_report) {
            Serial.printf("[ROM] wrote %uKB\n", done / 1024);
            next_report += 65536;
        }
    }
    free(buf);

    rc = rom_store_write_end(&writer);
    if (rc != ROM_STORE_OK) {
        Serial.printf("[ROM] commit failed (%d)\n", rc);
        return false;
    }
    Serial.printf("[ROM] done %u bytes\n", done);
    return true;
}

const uint8_t* rom_store_mmap(uint32_t* out_len)
{
    rom_store_hdr_t hdr;
    const void* ptr = nullptr;
    esp_err_t err;

    if (mapped) {
        if (out_len) {
            *out_len = mapped_len;
        }
        return mapped;
    }
    if (!part || !rom_store_read_header(&flash, &hdr)) {
        return nullptr;
    }

    // Map the stored ROM only, never the whole partition: the data map shares
    // a 4 MB address space with the app's flash-resident constants, and MMU
    // pages spent here are pages the rest of the image cannot have. The
    // offset need not be 64 KB aligned — the returned pointer is adjusted to
    // the byte asked for.
    err = esp_partition_mmap(part, ROM_STORE_DATA_OFFSET(&flash), hdr.size,
                             SPI_FLASH_MMAP_DATA, &ptr, &map_handle);
    if (err != ESP_OK || !ptr) {
        Serial.printf("[ROM] mmap failed (%d)\n", (int)err);
        return nullptr;
    }
    mapped = (const uint8_t*)ptr;
    mapped_len = hdr.size;
    Serial.printf("[ROM] mapped '%s' %uKB\n", hdr.filename, hdr.size / 1024);
    if (out_len) {
        *out_len = mapped_len;
    }
    return mapped;
}
