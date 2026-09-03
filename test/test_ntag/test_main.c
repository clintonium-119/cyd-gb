#include <unity.h>

#include <string.h>

#include "cart/ntag.h"
#include "fake_ntag215.h"

/* This build's password and acknowledge. Arbitrary here — on the device they
 * come from hw_config.h, which this module deliberately does not include. */
static const uint8_t PWD[4] = { 0x43, 0x59, 0x44, 0x47 };
static const uint8_t PACK[2] = { 0x47, 0x42 };
static const uint8_t OTHER_PWD[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
static const uint8_t OTHER_PACK[2] = { 0x13, 0x37 };

/* A composed NDEF message for "MENU", 16 bytes = four pages. */
static const uint8_t MSG[16] = {
    0x03, 0x0B, 0xD1, 0x01, 0x07, 0x54, 0x02, 0x65, 0x6E,
    'M', 'E', 'N', 'U', 0xFE, 0x00, 0x00,
};

static fake_ntag215_t tag;
static ntag_dev_t dev;

void setUp(void)
{
    fake_ntag215_init(&tag);
    dev.ctx = &tag;
    dev.xcv = fake_ntag215_xcv;
}

void tearDown(void)
{
}

static size_t writes_to(int page)
{
    return fake_ntag215_count(&tag, NTAG_CMD_WRITE, page);
}

/* ---- the whitelist ----------------------------------------------------- */

static void test_whitelist_refuses_the_irreversible_pages(void)
{
    uint8_t data[4] = { 0x00, 0x00, 0x00, 0x00 };

    TEST_ASSERT_FALSE(ntag_page_writable(0x00, data));
    TEST_ASSERT_FALSE(ntag_page_writable(0x01, data));
    TEST_ASSERT_FALSE(ntag_page_writable(NTAG215_PAGE_STATIC_LOCK, data));
    TEST_ASSERT_FALSE(ntag_page_writable(NTAG215_PAGE_CC, data));
    TEST_ASSERT_FALSE(ntag_page_writable(NTAG215_PAGE_DYN_LOCK, data));
    TEST_ASSERT_FALSE(ntag_page_writable(NTAG215_PAGE_COUNT, data));
    TEST_ASSERT_FALSE(ntag_page_writable(0xFF, data));

    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_USER_FIRST, data));
    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_USER_LAST, data));
    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_CFG0, data));
    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_CFG1, data));
    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_PWD, data));
    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_PACK, data));
}

static void test_whitelist_refuses_cfglck(void)
{
    uint8_t clear[4] = { 0x00, 0x00, 0x00, 0x00 };
    uint8_t locked[4] = { NTAG215_ACCESS_CFGLCK, 0x00, 0x00, 0x00 };
    /* NFC_CNT_EN | NFC_CNT_PWD_PROT: other bits set, CFGLCK clear. */
    uint8_t other_bits[4] = { 0x18, 0x00, 0x00, 0x00 };

    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_CFG1, clear));
    TEST_ASSERT_FALSE(ntag_page_writable(NTAG215_PAGE_CFG1, locked));
    /* Only bit 6 decides: other ACCESS bits do not make a write refusable. */
    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_CFG1, other_bits));
    other_bits[0] |= NTAG215_ACCESS_CFGLCK;
    TEST_ASSERT_FALSE(ntag_page_writable(NTAG215_PAGE_CFG1, other_bits));
    /* PROT set but CFGLCK clear is still allowed — this module composes PROT
     * itself and never relies on the whitelist to police it. */
    other_bits[0] = NTAG215_ACCESS_PROT;
    TEST_ASSERT_TRUE(ntag_page_writable(NTAG215_PAGE_CFG1, other_bits));

    TEST_ASSERT_FALSE(ntag_page_writable(NTAG215_PAGE_CFG1, NULL));
}

/* Guard test (d): a refused write never reaches the tag. */
static void test_refused_writes_never_reach_the_tag(void)
{
    uint8_t data[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t lock[4] = { NTAG215_ACCESS_CFGLCK, 0x00, 0x00, 0x00 };

    TEST_ASSERT_EQUAL_INT(NTAG_ERR_WHITELIST,
        ntag_write_page(&dev, NTAG215_PAGE_STATIC_LOCK, data));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_WHITELIST,
        ntag_write_page(&dev, NTAG215_PAGE_CC, data));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_WHITELIST,
        ntag_write_page(&dev, NTAG215_PAGE_DYN_LOCK, data));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_WHITELIST,
        ntag_write_page(&dev, NTAG215_PAGE_CFG1, lock));

    /* The fake would have stored every one of those. It saw none. */
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_count(&tag, NTAG_CMD_WRITE,
                                                   FAKE_NTAG215_ANY_PAGE));
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_lock_pages_touched(&tag));
    /* And the capability container is untouched. */
    TEST_ASSERT_EQUAL_HEX8(0xE1, tag.pages[NTAG215_PAGE_CC][0]);
}

/* ---- reads ------------------------------------------------------------- */

static void test_read_pages_uses_one_read_per_four_pages(void)
{
    uint8_t out[24 * NTAG_PAGE_SIZE];

    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_read_pages(&dev, NTAG215_PAGE_USER_FIRST, 24, out));
    TEST_ASSERT_EQUAL_size_t(6, fake_ntag215_count(&tag, NTAG_CMD_READ,
                                                   FAKE_NTAG215_ANY_PAGE));
    /* The factory tag's empty NDEF message is the first thing in it. */
    TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, out[2]);
}

static void test_read_past_the_last_page_is_out_of_range(void)
{
    uint8_t out[NTAG_READ_SIZE];

    TEST_ASSERT_EQUAL_INT(NTAG_ERR_RANGE,
        ntag_read_pages(&dev, NTAG215_PAGE_PACK, 2, out));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_RANGE,
        ntag_read_pages(&dev, NTAG215_PAGE_COUNT, 1, out));
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_count(&tag, NTAG_CMD_READ,
                                                   FAKE_NTAG215_ANY_PAGE));
}

static void test_get_version_returns_the_ntag215_signature(void)
{
    uint8_t v[NTAG_VERSION_SIZE];

    TEST_ASSERT_EQUAL_INT(NTAG_OK, ntag_get_version(&dev, v));
    TEST_ASSERT_EQUAL_HEX8(0x00, v[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04, v[1]); /* NXP */
    TEST_ASSERT_EQUAL_HEX8(0x04, v[2]); /* NTAG */
    TEST_ASSERT_EQUAL_HEX8(0x11, v[6]); /* 504 bytes: NTAG215, not 213 or 216 */
    TEST_ASSERT_EQUAL_HEX8(0x03, v[7]);
}

static void test_factory_tag_reads_as_unprotected(void)
{
    uint8_t auth0 = 0;

    TEST_ASSERT_EQUAL_INT(NTAG_OK, ntag_read_auth0(&dev, &auth0));
    TEST_ASSERT_EQUAL_HEX8(NTAG215_AUTH0_OPEN, auth0);
}

/* ---- protect ----------------------------------------------------------- */

static void test_protect_write_order_and_composed_access(void)
{
    uint8_t access;

    TEST_ASSERT_EQUAL_INT(NTAG_OK, ntag_protect(&dev, PWD, PACK));

    /* Exactly four writes, in the order PWD, PACK, CFG1, CFG0. */
    TEST_ASSERT_EQUAL_size_t(4, fake_ntag215_count(&tag, NTAG_CMD_WRITE,
                                                   FAKE_NTAG215_ANY_PAGE));
    {
        size_t i;
        uint8_t order[4];
        size_t n = 0;
        for (i = 0; i < tag.logged; i++) {
            if (tag.log[i].cmd == NTAG_CMD_WRITE) {
                order[n] = tag.log[i].page;
                n++;
            }
        }
        TEST_ASSERT_EQUAL_size_t(4, n);
        TEST_ASSERT_EQUAL_HEX8(NTAG215_PAGE_PWD, order[0]);
        TEST_ASSERT_EQUAL_HEX8(NTAG215_PAGE_PACK, order[1]);
        TEST_ASSERT_EQUAL_HEX8(NTAG215_PAGE_CFG1, order[2]);
        TEST_ASSERT_EQUAL_HEX8(NTAG215_PAGE_CFG0, order[3]);
    }

    access = fake_ntag215_access(&tag);
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_PROT);
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_CFGLCK);
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_AUTHLIM_MASK);
    TEST_ASSERT_EQUAL_HEX8(NTAG215_PAGE_USER_FIRST, fake_ntag215_auth0(&tag));
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_lock_pages_touched(&tag));
}

static void test_protect_preserves_bits_it_does_not_own(void)
{
    /* NFC counter enabled, and the undocumented byte real parts ship with. */
    tag.pages[NTAG215_PAGE_CFG1][0] = 0x18; /* NFC_CNT_EN | NFC_CNT_PWD_PROT */
    TEST_ASSERT_EQUAL_HEX8(0x05, tag.pages[NTAG215_PAGE_CFG1][1]);

    TEST_ASSERT_EQUAL_INT(NTAG_OK, ntag_protect(&dev, PWD, PACK));

    TEST_ASSERT_EQUAL_HEX8(0x18, fake_ntag215_access(&tag));
    TEST_ASSERT_EQUAL_HEX8(0x05, tag.pages[NTAG215_PAGE_CFG1][1]);
    /* CFG0's MIRROR byte keeps STRG_MOD_EN; only AUTH0 changed. */
    TEST_ASSERT_EQUAL_HEX8(0x04, tag.pages[NTAG215_PAGE_CFG0][0]);
}

static void test_protect_clears_a_previously_set_authlim(void)
{
    tag.pages[NTAG215_PAGE_CFG1][0] = (uint8_t)(NTAG215_ACCESS_PROT | 0x03);

    TEST_ASSERT_EQUAL_INT(NTAG_OK, ntag_protect(&dev, PWD, PACK));
    TEST_ASSERT_EQUAL_HEX8(0x00, fake_ntag215_access(&tag));
}

/* ---- authentication ---------------------------------------------------- */

static void test_pwd_auth_outcomes(void)
{
    fake_ntag215_set_protected(&tag, PWD, PACK, NTAG215_PAGE_USER_FIRST);

    TEST_ASSERT_EQUAL_INT(NTAG_OK, ntag_pwd_auth(&dev, PWD, PACK));
    TEST_ASSERT_TRUE(tag.authed);

    fake_ntag215_init(&tag);
    fake_ntag215_set_protected(&tag, PWD, PACK, NTAG215_PAGE_USER_FIRST);
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_AUTH,
        ntag_pwd_auth(&dev, OTHER_PWD, PACK));
    TEST_ASSERT_FALSE(tag.authed);

    /* Right password, but we expected a different PACK — not our tag. */
    fake_ntag215_init(&tag);
    fake_ntag215_set_protected(&tag, PWD, PACK, NTAG215_PAGE_USER_FIRST);
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_AUTH,
        ntag_pwd_auth(&dev, PWD, OTHER_PACK));
}

/* ---- provisioning ------------------------------------------------------ */

static void assert_provisioned_state(void)
{
    uint8_t back[sizeof(MSG)];
    uint8_t access = fake_ntag215_access(&tag);

    TEST_ASSERT_EQUAL_HEX8(NTAG215_PAGE_USER_FIRST, fake_ntag215_auth0(&tag));
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_PROT);
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_CFGLCK);
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_AUTHLIM_MASK);

    /* PROT is 0, so the content still reads without a session. */
    tag.authed = false;
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_read_pages(&dev, NTAG215_PAGE_USER_FIRST, 4, back));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MSG, back, sizeof(MSG));

    /* But it does not write without one. */
    tag.authed = false;
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_NAK,
        ntag_write_page(&dev, NTAG215_PAGE_USER_FIRST, MSG));

    /* And it does with one. */
    TEST_ASSERT_EQUAL_INT(NTAG_OK, ntag_pwd_auth(&dev, PWD, PACK));
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_write_page(&dev, NTAG215_PAGE_USER_FIRST, MSG));

    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_lock_pages_touched(&tag));
}

static void test_provision_a_factory_tag(void)
{
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));
    assert_provisioned_state();
}

static void test_provision_an_all_zero_blank_tag(void)
{
    fake_ntag215_set_all_zero_user(&tag);
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));
    assert_provisioned_state();
}

static void test_provision_does_not_authenticate_a_factory_tag_first(void)
{
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));

    /* Exactly one PWD_AUTH, and it comes after the last WRITE — the closing
     * proof that protection took, never an unnecessary session up front. */
    TEST_ASSERT_EQUAL_size_t(1, fake_ntag215_count(&tag, NTAG_CMD_PWD_AUTH,
                                                   FAKE_NTAG215_ANY_PAGE));
    {
        size_t i;
        size_t last_write = 0;
        size_t auth_at = 0;
        for (i = 0; i < tag.logged; i++) {
            if (tag.log[i].cmd == NTAG_CMD_WRITE) {
                last_write = i;
            }
            if (tag.log[i].cmd == NTAG_CMD_PWD_AUTH) {
                auth_at = i;
            }
        }
        TEST_ASSERT_TRUE(auth_at > last_write);
    }
}

static void test_provision_rewrites_a_tag_that_is_already_ours(void)
{
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));

    fake_ntag215_reset_log(&tag);
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));

    /* This time it must authenticate before it can write anything. */
    {
        size_t i;
        size_t first_write = tag.logged;
        size_t first_auth = tag.logged;
        for (i = 0; i < tag.logged; i++) {
            if (tag.log[i].cmd == NTAG_CMD_WRITE && first_write == tag.logged) {
                first_write = i;
            }
            if (tag.log[i].cmd == NTAG_CMD_PWD_AUTH &&
                first_auth == tag.logged) {
                first_auth = i;
            }
        }
        TEST_ASSERT_TRUE(first_auth < first_write);
    }
    assert_provisioned_state();
}

static void test_provision_of_a_foreign_tag_writes_nothing(void)
{
    fake_ntag215_set_protected(&tag, OTHER_PWD, OTHER_PACK,
                               NTAG215_PAGE_USER_FIRST);

    TEST_ASSERT_EQUAL_INT(NTAG_ERR_AUTH,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_count(&tag, NTAG_CMD_WRITE,
                                                   FAKE_NTAG215_ANY_PAGE));
    /* The other build's password is still in place. */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(OTHER_PWD, tag.pages[NTAG215_PAGE_PWD], 4);
}

static void test_verify_detects_a_flipped_byte(void)
{
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_write_bytes(&dev, NTAG215_PAGE_USER_FIRST, MSG, sizeof(MSG)));
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_verify_bytes(&dev, NTAG215_PAGE_USER_FIRST, MSG, sizeof(MSG)));

    tag.pages[NTAG215_PAGE_USER_FIRST + 2][1] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_VERIFY,
        ntag_verify_bytes(&dev, NTAG215_PAGE_USER_FIRST, MSG, sizeof(MSG)));
}

static void test_a_mid_sequence_nak_leaves_auth0_open(void)
{
    /* The tag stops accepting the very last write of the protect sequence. */
    tag.nak_one_write = true;
    tag.nak_write_page = NTAG215_PAGE_CFG0;

    TEST_ASSERT_EQUAL_INT(NTAG_ERR_NAK,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));

    /* AUTH0 is written last precisely so this case is recoverable: the tag is
     * still open, so the boot flow's heal path can provision it again. */
    TEST_ASSERT_EQUAL_HEX8(NTAG215_AUTH0_OPEN, fake_ntag215_auth0(&tag));
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_write_page(&dev, NTAG215_PAGE_USER_FIRST, MSG));

    /* And a second run with the tag healthy again completes. */
    tag.nak_one_write = false;
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));
    assert_provisioned_state();
}

static void test_transport_failures_never_become_success(void)
{
    uint8_t buf[NTAG_READ_SIZE];
    uint8_t auth0 = 0;

    tag.io_fail = true;

    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO, ntag_get_version(&dev, buf));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO,
        ntag_read_pages(&dev, NTAG215_PAGE_USER_FIRST, 1, buf));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO, ntag_read_auth0(&dev, &auth0));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO,
        ntag_write_page(&dev, NTAG215_PAGE_USER_FIRST, MSG));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO,
        ntag_write_bytes(&dev, NTAG215_PAGE_USER_FIRST, MSG, sizeof(MSG)));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO,
        ntag_verify_bytes(&dev, NTAG215_PAGE_USER_FIRST, MSG, sizeof(MSG)));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO, ntag_pwd_auth(&dev, PWD, PACK));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO, ntag_protect(&dev, PWD, PACK));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_IO,
        ntag_provision(&dev, MSG, sizeof(MSG), PWD, PACK));
}

static void test_null_arguments_are_rejected(void)
{
    uint8_t buf[NTAG_READ_SIZE];
    ntag_dev_t broken;

    broken.ctx = &tag;
    broken.xcv = NULL;

    TEST_ASSERT_EQUAL_INT(NTAG_ERR_ARGS, ntag_get_version(NULL, buf));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_ARGS, ntag_get_version(&broken, buf));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_ARGS, ntag_get_version(&dev, NULL));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_ARGS,
        ntag_read_pages(&dev, NTAG215_PAGE_USER_FIRST, 0, buf));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_ARGS,
        ntag_write_page(&dev, NTAG215_PAGE_USER_FIRST, NULL));
    /* A length that is not a whole number of pages cannot be written. */
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_ARGS,
        ntag_write_bytes(&dev, NTAG215_PAGE_USER_FIRST, MSG, 6));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_ARGS,
        ntag_provision(&dev, MSG, 6, PWD, PACK));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_ARGS, ntag_pwd_auth(&dev, PWD, NULL));
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_count(&tag, NTAG_CMD_WRITE,
                                                   FAKE_NTAG215_ANY_PAGE));
}

static void test_write_bytes_will_not_run_past_user_memory(void)
{
    static uint8_t big[8];

    memset(big, 0, sizeof(big));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_RANGE,
        ntag_write_bytes(&dev, NTAG215_PAGE_USER_LAST, big, sizeof(big)));
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_RANGE,
        ntag_write_bytes(&dev, NTAG215_PAGE_CFG0, big, sizeof(big)));
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_count(&tag, NTAG_CMD_WRITE,
                                                   FAKE_NTAG215_ANY_PAGE));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_whitelist_refuses_the_irreversible_pages);
    RUN_TEST(test_whitelist_refuses_cfglck);
    RUN_TEST(test_refused_writes_never_reach_the_tag);
    RUN_TEST(test_read_pages_uses_one_read_per_four_pages);
    RUN_TEST(test_read_past_the_last_page_is_out_of_range);
    RUN_TEST(test_get_version_returns_the_ntag215_signature);
    RUN_TEST(test_factory_tag_reads_as_unprotected);
    RUN_TEST(test_protect_write_order_and_composed_access);
    RUN_TEST(test_protect_preserves_bits_it_does_not_own);
    RUN_TEST(test_protect_clears_a_previously_set_authlim);
    RUN_TEST(test_pwd_auth_outcomes);
    RUN_TEST(test_provision_a_factory_tag);
    RUN_TEST(test_provision_an_all_zero_blank_tag);
    RUN_TEST(test_provision_does_not_authenticate_a_factory_tag_first);
    RUN_TEST(test_provision_rewrites_a_tag_that_is_already_ours);
    RUN_TEST(test_provision_of_a_foreign_tag_writes_nothing);
    RUN_TEST(test_verify_detects_a_flipped_byte);
    RUN_TEST(test_a_mid_sequence_nak_leaves_auth0_open);
    RUN_TEST(test_transport_failures_never_become_success);
    RUN_TEST(test_null_arguments_are_rejected);
    RUN_TEST(test_write_bytes_will_not_run_past_user_memory);
    return UNITY_END();
}
