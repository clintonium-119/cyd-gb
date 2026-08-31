#include <unity.h>

#include <string.h>

#include "cart/rom_store.h"

/*
 * ROM-store suite over a fake NOR backend. The fake is the point of the
 * suite: real NOR flash erases to 0xFF in whole sectors and a write can only
 * clear bits, so a backend that tolerates a write to a non-erased byte would
 * let a broken write protocol pass. Every violation trips `fault`, which each
 * test asserts is still clear at the end.
 *
 * Erase and write calls are counted, because the product behaviour this
 * module exists for — an unchanged ROM must not be rewritten on every boot —
 * is a statement about counts, not about returned values.
 */

#define FAKE_SZ (64u * 1024u)
#define FAKE_SECTOR 4096u
#define FAKE_ROOM (FAKE_SZ - FAKE_SECTOR) /* bytes a ROM may occupy */

static uint8_t fake[FAKE_SZ];
static unsigned n_erase;
static unsigned n_write;
static int fault;

static int fake_read(void* ctx, uint32_t off, void* dst, uint32_t len)
{
    (void)ctx;
    if (off > FAKE_SZ || len > FAKE_SZ - off) {
        fault = 1;
        return -1;
    }
    memcpy(dst, fake + off, len);
    return 0;
}

static int fake_write(void* ctx, uint32_t off, const void* src, uint32_t len)
{
    const uint8_t* p = (const uint8_t*)src;
    uint32_t i;

    (void)ctx;
    n_write++;
    if (off > FAKE_SZ || len > FAKE_SZ - off) {
        fault = 1;
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (fake[off + i] != 0xFF) {
            fault = 1; /* write without erase */
            return -1;
        }
    }
    memcpy(fake + off, p, len);
    return 0;
}

static int fake_erase(void* ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    n_erase++;
    if ((off % FAKE_SECTOR) != 0 || (len % FAKE_SECTOR) != 0 || len == 0) {
        fault = 1; /* sub-sector erase */
        return -1;
    }
    if (off > FAKE_SZ || len > FAKE_SZ - off) {
        fault = 1;
        return -1;
    }
    memset(fake + off, 0xFF, len);
    return 0;
}

static rom_store_flash_t make_flash(void)
{
    rom_store_flash_t f;
    f.ctx = NULL;
    f.read = fake_read;
    f.write = fake_write;
    f.erase = fake_erase;
    f.size = FAKE_SZ;
    f.erase_size = FAKE_SECTOR;
    return f;
}

/* Table-driven CRC32, deliberately a different implementation from the
 * module's bitwise one, so case 1 compares against an independent answer
 * rather than against the code under test. */
static uint32_t ref_crc32(const void* data, size_t len)
{
    static uint32_t table[256];
    static int built = 0;
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c;
    size_t i;
    int bit;

    if (!built) {
        uint32_t n;
        for (n = 0; n < 256; n++) {
            c = n;
            for (bit = 0; bit < 8; bit++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        built = 1;
    }
    c = 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* Source bytes for every write. The default length is deliberately not a
 * multiple of the sector size, so the erase rounding and the trailing partial
 * write are exercised by the ordinary cases rather than by a special one; the
 * buffer itself is large enough for the fits-exactly case. */
#define ODD_ROM_SZ 5000u
static uint8_t rom[FAKE_ROOM];

static void fill_rom(uint8_t seed)
{
    uint32_t i;
    uint8_t v = seed;
    for (i = 0; i < sizeof(rom); i++) {
        v = (uint8_t)(v * 31u + 17u);
        rom[i] = v;
    }
}

/* Write `len` bytes of `rom` through the protocol in `chunk`-sized pieces. */
static int write_rom(const rom_store_flash_t* f, const char* name, uint32_t len,
                     uint32_t chunk)
{
    rom_store_writer_t w;
    uint32_t done = 0;
    int rc = rom_store_write_begin(f, &w, name, len);

    if (rc != ROM_STORE_OK) {
        return rc;
    }
    while (done < len) {
        uint32_t n = len - done;
        if (n > chunk) {
            n = chunk;
        }
        rc = rom_store_write_chunk(&w, rom + done, n);
        if (rc != ROM_STORE_OK) {
            return rc;
        }
        done += n;
    }
    return rom_store_write_end(&w);
}

void setUp(void)
{
    memset(fake, 0xFF, sizeof(fake));
    n_erase = 0;
    n_write = 0;
    fault = 0;
    fill_rom(1);
}

void tearDown(void)
{
}

static void test_fresh_write_round_trips_header_and_data(void)
{
    rom_store_flash_t f = make_flash();
    rom_store_hdr_t hdr;

    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        write_rom(&f, "Tetris (World).gb", ODD_ROM_SZ, 1024));
    /* One erase for the header sector plus the two data sectors 5000 bytes
     * round up to — not one per chunk. */
    TEST_ASSERT_EQUAL_UINT(1, n_erase);

    TEST_ASSERT_TRUE(rom_store_read_header(&f, &hdr));
    TEST_ASSERT_EQUAL_HEX32(ROM_STORE_MAGIC, hdr.magic);
    TEST_ASSERT_EQUAL_UINT32(ODD_ROM_SZ, hdr.size);
    TEST_ASSERT_EQUAL_STRING("Tetris (World).gb", hdr.filename);
    TEST_ASSERT_EQUAL_HEX32(ref_crc32(rom, ODD_ROM_SZ), hdr.crc32);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(rom, fake + ROM_STORE_DATA_OFFSET(&f),
                                  ODD_ROM_SZ);
    /* Nothing written past the declared size. */
    TEST_ASSERT_EQUAL_HEX8(0xFF, fake[ROM_STORE_DATA_OFFSET(&f) + ODD_ROM_SZ]);
    TEST_ASSERT_FALSE(fault);
}

static void test_unchanged_rom_short_circuits_with_no_erase_or_write(void)
{
    rom_store_flash_t f = make_flash();
    rom_store_hdr_t hdr;

    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        write_rom(&f, "Tetris (World).gb", ODD_ROM_SZ, 1024));

    /* From here on stands in for the next boot: read the header, ask whether
     * it already describes this ROM, touch nothing. */
    n_erase = 0;
    n_write = 0;
    TEST_ASSERT_TRUE(rom_store_read_header(&f, &hdr));
    TEST_ASSERT_TRUE(rom_store_matches(&hdr, "Tetris (World).gb", ODD_ROM_SZ));
    TEST_ASSERT_EQUAL_UINT(0, n_erase);
    TEST_ASSERT_EQUAL_UINT(0, n_write);
    TEST_ASSERT_FALSE(fault);
}

static void test_changed_name_or_size_does_not_match(void)
{
    rom_store_flash_t f = make_flash();
    rom_store_hdr_t hdr;

    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        write_rom(&f, "Tetris (World).gb", ODD_ROM_SZ, 1024));
    TEST_ASSERT_TRUE(rom_store_read_header(&f, &hdr));

    TEST_ASSERT_FALSE(rom_store_matches(&hdr, "Zelda (USA).gb", ODD_ROM_SZ));
    TEST_ASSERT_FALSE(rom_store_matches(&hdr, "Tetris (World).gb",
                                        ODD_ROM_SZ + 1));
    /* Case-sensitive, and a prefix is not a match. */
    TEST_ASSERT_FALSE(rom_store_matches(&hdr, "tetris (world).gb", ODD_ROM_SZ));
    TEST_ASSERT_FALSE(rom_store_matches(&hdr, "Tetris", ODD_ROM_SZ));
    TEST_ASSERT_FALSE(fault);
}

static void test_torn_write_leaves_no_valid_header(void)
{
    rom_store_flash_t f = make_flash();
    rom_store_writer_t w;
    rom_store_hdr_t hdr;

    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        write_rom(&f, "Tetris (World).gb", ODD_ROM_SZ, 1024));
    TEST_ASSERT_TRUE(rom_store_read_header(&f, &hdr));

    /* Now start replacing it and stop mid-stream, as a reset or a pulled USB
     * cable would. The old header must not survive to describe the new,
     * half-written bytes. */
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        rom_store_write_begin(&f, &w, "Zelda (USA).gb", ODD_ROM_SZ));
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK, rom_store_write_chunk(&w, rom, 1024));
    TEST_ASSERT_FALSE(rom_store_read_header(&f, &hdr));
    TEST_ASSERT_FALSE(fault);
}

static void test_oversize_rom_is_rejected_without_erasing(void)
{
    rom_store_flash_t f = make_flash();
    rom_store_writer_t w;

    TEST_ASSERT_EQUAL_INT(ROM_STORE_ERR_RANGE,
        rom_store_write_begin(&f, &w, "Huge.gb", FAKE_ROOM + 1));
    TEST_ASSERT_EQUAL_UINT(0, n_erase);
    TEST_ASSERT_EQUAL_UINT(0, n_write);

    /* The boundary itself is accepted: a ROM filling every data sector fits. */
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        write_rom(&f, "Exact.gb", FAKE_ROOM, FAKE_SECTOR));
    TEST_ASSERT_FALSE(fault);
}

static void test_write_end_before_declared_size_is_an_order_error(void)
{
    rom_store_flash_t f = make_flash();
    rom_store_writer_t w;
    rom_store_hdr_t hdr;

    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        rom_store_write_begin(&f, &w, "Short.gb", 1000));
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK, rom_store_write_chunk(&w, rom, 500));
    TEST_ASSERT_EQUAL_INT(ROM_STORE_ERR_ORDER, rom_store_write_end(&w));
    TEST_ASSERT_FALSE(rom_store_read_header(&f, &hdr));
    /* The failed end closed the writer, so a second attempt cannot sneak the
     * header in behind it. */
    TEST_ASSERT_EQUAL_INT(ROM_STORE_ERR_ORDER, rom_store_write_end(&w));

    /* The other half of chunk accounting: a stream that would run past the
     * declared size is refused rather than truncated. */
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        rom_store_write_begin(&f, &w, "Short.gb", 1000));
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK, rom_store_write_chunk(&w, rom, 900));
    TEST_ASSERT_EQUAL_INT(ROM_STORE_ERR_RANGE,
        rom_store_write_chunk(&w, rom, 101));
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK, rom_store_write_chunk(&w, rom, 100));
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK, rom_store_write_end(&w));
    TEST_ASSERT_FALSE(fault);
}

static void test_verify_accepts_intact_data_and_rejects_a_flipped_byte(void)
{
    rom_store_flash_t f = make_flash();
    rom_store_hdr_t hdr;
    uint8_t scratch[256];

    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        write_rom(&f, "Tetris (World).gb", ODD_ROM_SZ, 1024));
    TEST_ASSERT_TRUE(rom_store_read_header(&f, &hdr));
    TEST_ASSERT_EQUAL_INT(ROM_STORE_OK,
        rom_store_verify(&f, &hdr, scratch, sizeof(scratch)));

    /* Corrupt one stored byte behind the backend's back, the way a bad
     * sector would. */
    fake[ROM_STORE_DATA_OFFSET(&f) + 1234] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(ROM_STORE_ERR_CRC,
        rom_store_verify(&f, &hdr, scratch, sizeof(scratch)));
    TEST_ASSERT_FALSE(fault);
}

static void test_crc32_matches_the_standard_check_value(void)
{
    /* The IEEE/zlib check value: crc32 of "123456789". */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
        rom_store_crc32(0, "123456789", 9));
    /* Seeding with the previous result must equal the one-shot answer, which
     * is what lets the writer fold the CRC chunk by chunk. */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
        rom_store_crc32(rom_store_crc32(0, "12345", 5), "6789", 4));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fresh_write_round_trips_header_and_data);
    RUN_TEST(test_unchanged_rom_short_circuits_with_no_erase_or_write);
    RUN_TEST(test_changed_name_or_size_does_not_match);
    RUN_TEST(test_torn_write_leaves_no_valid_header);
    RUN_TEST(test_oversize_rom_is_rejected_without_erasing);
    RUN_TEST(test_write_end_before_declared_size_is_an_order_error);
    RUN_TEST(test_verify_accepts_intact_data_and_rejects_a_flipped_byte);
    RUN_TEST(test_crc32_matches_the_standard_check_value);
    return UNITY_END();
}
