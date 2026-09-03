#include <unity.h>

#include <string.h>

#include "cart/ndef.h"

#define CANARY 0xC5
#define GUARD 4
#define OUT_SZ 64

/* The design's byte layout for a tag reading "Tetris.gb", language "en":
 * message length 0x10 = header 4 + payload 12; payload length 0x0C =
 * status 1 + language 2 + text 9. */
static const uint8_t rec_en[] = {
    0x03, 0x10, 0xD1, 0x01, 0x0C, 0x54, 0x02, 0x65, 0x6E,
    'T', 'e', 't', 'r', 'i', 's', '.', 'g', 'b', 0xFE,
};

/* The same record written from a French-locale phone: language "fr". */
static const uint8_t rec_fr[] = {
    0x03, 0x10, 0xD1, 0x01, 0x0C, 0x54, 0x02, 0x66, 0x72,
    'T', 'e', 't', 'r', 'i', 's', '.', 'g', 'b', 0xFE,
};

/* Language "fr-CA": status 0x05, so the text starts five bytes further in.
 * Assuming a 2-byte code here would yield "CATetris.gb". */
static const uint8_t rec_fr_ca[] = {
    0x03, 0x13, 0xD1, 0x01, 0x0F, 0x54, 0x05, 0x66, 0x72, 0x2D, 0x43, 0x41,
    'T', 'e', 't', 'r', 'i', 's', '.', 'g', 'b', 0xFE,
};

/* rec_en with the terminator TLV cut off. */
static const uint8_t rec_no_terminator[] = {
    0x03, 0x10, 0xD1, 0x01, 0x0C, 0x54, 0x02, 0x65, 0x6E,
    'T', 'e', 't', 'r', 'i', 's', '.', 'g', 'b',
};

/* rec_en with a payload length that runs past the message. */
static const uint8_t rec_payload_overruns[] = {
    0x03, 0x10, 0xD1, 0x01, 0xFF, 0x54, 0x02, 0x65, 0x6E,
    'T', 'e', 't', 'r', 'i', 's', '.', 'g', 'b', 0xFE,
};

/* rec_en with type 'U' (URI) instead of 'T'. */
static const uint8_t rec_uri_type[] = {
    0x03, 0x10, 0xD1, 0x01, 0x0C, 0x55, 0x02, 0x65, 0x6E,
    'T', 'e', 't', 'r', 'i', 's', '.', 'g', 'b', 0xFE,
};

/* rec_en with the status byte's UTF-16 bit set. */
static const uint8_t rec_utf16[] = {
    0x03, 0x10, 0xD1, 0x01, 0x0C, 0x54, 0x82, 0x65, 0x6E,
    'T', 'e', 't', 'r', 'i', 's', '.', 'g', 'b', 0xFE,
};

/* rec_en behind two NULL TLVs, as a formatter may leave them. */
static const uint8_t rec_null_tlv_prefix[] = {
    0x00, 0x00,
    0x03, 0x10, 0xD1, 0x01, 0x0C, 0x54, 0x02, 0x65, 0x6E,
    'T', 'e', 't', 'r', 'i', 's', '.', 'g', 'b', 0xFE,
};

static const uint8_t blank_empty_message[] = { 0x03, 0x00, 0xFE };

/* Not a TLV structure at all: no 0x03 message TLV anywhere. */
static const uint8_t not_ndef[] = { 0x01, 0x02, 0x03, 0x04 };

static const uint8_t menu_expected[] = {
    0x03, 0x0B, 0xD1, 0x01, 0x07, 0x54, 0x02, 0x65, 0x6E,
    'M', 'E', 'N', 'U', 0xFE, 0x00, 0x00,
};

static struct {
    unsigned char pre[GUARD];
    char out[OUT_SZ];
    unsigned char mid[GUARD];
    uint8_t buf[NDEF_BUF_MAX];
    unsigned char post[GUARD];
} g;

void setUp(void)
{
    memset(g.pre, CANARY, sizeof(g.pre));
    memset(g.mid, CANARY, sizeof(g.mid));
    memset(g.post, CANARY, sizeof(g.post));
    memset(g.out, 0x7F, sizeof(g.out));
    memset(g.buf, 0xAB, sizeof(g.buf));
}

void tearDown(void)
{
}

static void assert_canaries_intact(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX8(CANARY, g.pre[i]);
        TEST_ASSERT_EQUAL_HEX8(CANARY, g.mid[i]);
        TEST_ASSERT_EQUAL_HEX8(CANARY, g.post[i]);
    }
}

/* Compose text, then parse the result back, asserting the round trip and the
 * page-aligned bound the writer depends on. */
static void assert_round_trip(const char* text)
{
    /* Sized for the longest payload compose accepts, not for OUT_SZ: the
     * boot flow's longest is "WILD:" plus a 63-character file name. */
    char back[NDEF_TEXT_MAX + 1];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_compose_text(text, g.buf, sizeof(g.buf), &len));
    TEST_ASSERT_EQUAL_size_t(0, len % 4);
    TEST_ASSERT_TRUE(len <= NDEF_BUF_MAX);
    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_parse_text(g.buf, len, back, sizeof(back)));
    TEST_ASSERT_EQUAL_STRING(text, back);
    assert_canaries_intact();
}

static void test_parses_en_text_record(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_parse_text(rec_en, sizeof(rec_en), g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", g.out);
    assert_canaries_intact();
}

static void test_parses_fr_text_record(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_parse_text(rec_fr, sizeof(rec_fr), g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", g.out);
    assert_canaries_intact();
}

static void test_language_length_comes_from_the_status_byte(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_parse_text(rec_fr_ca, sizeof(rec_fr_ca), g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", g.out);
    assert_canaries_intact();
}

static void test_missing_terminator_is_truncated(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_TRUNCATED,
        ndef_parse_text(rec_no_terminator, sizeof(rec_no_terminator),
                        g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_payload_length_past_end_is_truncated(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_TRUNCATED,
        ndef_parse_text(rec_payload_overruns, sizeof(rec_payload_overruns),
                        g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    /* Nothing past the NUL was touched. */
    TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)g.out[1]);
    assert_canaries_intact();
}

static void test_uri_record_is_not_text(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_NOT_TEXT,
        ndef_parse_text(rec_uri_type, sizeof(rec_uri_type),
                        g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_utf16_text_is_refused(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_NOT_TEXT,
        ndef_parse_text(rec_utf16, sizeof(rec_utf16), g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_leading_null_tlvs_are_skipped(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_parse_text(rec_null_tlv_prefix, sizeof(rec_null_tlv_prefix),
                        g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", g.out);
    assert_canaries_intact();
}

static void test_empty_message_is_blank(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_BLANK,
        ndef_parse_text(blank_empty_message, sizeof(blank_empty_message),
                        g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_all_zero_region_is_blank(void)
{
    static uint8_t zeros[NDEF_BUF_MAX];
    memset(zeros, 0x00, sizeof(zeros));
    TEST_ASSERT_EQUAL_INT(NDEF_BLANK,
        ndef_parse_text(zeros, sizeof(zeros), g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_no_message_tlv_is_not_ndef(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_NOT_NDEF,
        ndef_parse_text(not_ndef, sizeof(not_ndef), g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_text_longer_than_output_is_too_long(void)
{
    /* "Tetris.gb" is nine characters; five bytes cannot hold it plus a NUL. */
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_TOO_LONG,
        ndef_parse_text(rec_en, sizeof(rec_en), g.out, 5));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)g.out[1]);
    assert_canaries_intact();
}

static void test_null_and_zero_sized_arguments_are_rejected(void)
{
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(NDEF_ERR_ARGS,
        ndef_parse_text(NULL, 0, g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_ARGS,
        ndef_parse_text(rec_en, sizeof(rec_en), NULL, sizeof(g.out)));
    /* A zero-sized output is never written, not even the NUL. */
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_ARGS,
        ndef_parse_text(rec_en, sizeof(rec_en), g.out, 0));
    TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)g.out[0]);

    TEST_ASSERT_EQUAL_INT(NDEF_ERR_ARGS,
        ndef_compose_text(NULL, g.buf, sizeof(g.buf), &len));
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_ARGS,
        ndef_compose_text("MENU", NULL, sizeof(g.buf), &len));
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_ARGS,
        ndef_compose_text("MENU", g.buf, 0, &len));
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_ARGS,
        ndef_compose_text("MENU", g.buf, sizeof(g.buf), NULL));
    assert_canaries_intact();
}

static void test_compose_menu_is_byte_exact(void)
{
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_compose_text("MENU", g.buf, sizeof(g.buf), &len));
    TEST_ASSERT_EQUAL_size_t(sizeof(menu_expected), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(menu_expected, g.buf, sizeof(menu_expected));
    assert_canaries_intact();
}

static void test_compose_parse_round_trips(void)
{
    char long_name[64];
    char wild_long[69];

    assert_round_trip("MENU");
    assert_round_trip("Tetris.gb");
    assert_round_trip("WILD:Tetris.gb");

    /* The longest file name a tag can carry: 63 characters, matching
     * ROM_STORE_NAME_MAX less its NUL. */
    memset(long_name, 'a', 60);
    memcpy(long_name + 60, ".gb", 4);
    TEST_ASSERT_EQUAL_size_t(63, strlen(long_name));
    assert_round_trip(long_name);

    /* And that same name as a wildcard target — the longest payload the
     * device ever composes, at 68 bytes. */
    memcpy(wild_long, "WILD:", 5);
    memcpy(wild_long + 5, long_name, 64);
    TEST_ASSERT_EQUAL_size_t(68, strlen(wild_long));
    assert_round_trip(wild_long);
}

static void test_compose_into_a_short_buffer_leaves_it_untouched(void)
{
    size_t len = (size_t)-1;

    /* "MENU" composes to 16 bytes; eight cannot hold it. */
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_TOO_LONG,
        ndef_compose_text("MENU", g.buf, 8, &len));
    TEST_ASSERT_EQUAL_HEX8(0xAB, g.buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAB, g.buf[7]);
    TEST_ASSERT_EQUAL_size_t((size_t)-1, len);
    assert_canaries_intact();
}

static void test_compose_refuses_text_past_the_cap(void)
{
    char too_long[NDEF_TEXT_MAX + 2];
    size_t len = (size_t)-1;

    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_TOO_LONG,
        ndef_compose_text(too_long, g.buf, sizeof(g.buf), &len));
    TEST_ASSERT_EQUAL_HEX8(0xAB, g.buf[0]);
    TEST_ASSERT_EQUAL_size_t((size_t)-1, len);
    assert_canaries_intact();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parses_en_text_record);
    RUN_TEST(test_parses_fr_text_record);
    RUN_TEST(test_language_length_comes_from_the_status_byte);
    RUN_TEST(test_missing_terminator_is_truncated);
    RUN_TEST(test_payload_length_past_end_is_truncated);
    RUN_TEST(test_uri_record_is_not_text);
    RUN_TEST(test_utf16_text_is_refused);
    RUN_TEST(test_leading_null_tlvs_are_skipped);
    RUN_TEST(test_empty_message_is_blank);
    RUN_TEST(test_all_zero_region_is_blank);
    RUN_TEST(test_no_message_tlv_is_not_ndef);
    RUN_TEST(test_text_longer_than_output_is_too_long);
    RUN_TEST(test_null_and_zero_sized_arguments_are_rejected);
    RUN_TEST(test_compose_menu_is_byte_exact);
    RUN_TEST(test_compose_parse_round_trips);
    RUN_TEST(test_compose_into_a_short_buffer_leaves_it_untouched);
    RUN_TEST(test_compose_refuses_text_past_the_cap);
    return UNITY_END();
}
