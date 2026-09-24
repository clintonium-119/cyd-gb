#include <unity.h>

#include <stdint.h>
#include <string.h>

void setUp(void)
{
}

void tearDown(void)
{
}

/* The golden suites hash frames as bytes, so they assume a little-endian
 * host, as the ESP32 is. */
static void test_host_is_little_endian(void)
{
    const uint16_t v = 0x0102u;
    uint8_t b[2];

    memcpy(b, &v, sizeof(b));
    TEST_ASSERT_EQUAL_HEX8(0x02u, b[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_host_is_little_endian);
    return UNITY_END();
}
