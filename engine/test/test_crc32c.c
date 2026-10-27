#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "bytes.h"
#include "tests.h"
#include "page.h"
#include "rng.h"

static uint32_t crc32c_bitwise(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];                     
        for (int bit = 0; bit < 8; bit++) { 
            if (crc & 1) crc = (crc >> 1) ^ 0x82F63B78;
            else         crc =  crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

static void test_crc32c_bitwise(uint64_t *rng) {
    CHECK(crc32c_sw((const uint8_t *)"123456789", 9) == 0xE3069283);

    uint8_t buf[600];
    for (size_t size = 0; size < sizeof(buf); size++) {

        for (uint32_t i = 0; i < size; i++) {
            buf[i] = (uint8_t)rng_next(rng);
        }

        uint32_t sw = crc32c_sw(buf, size);
        uint32_t bitwise = crc32c_bitwise(buf, size);

        CHECK(sw == bitwise);
    }
}

#ifdef SPRAWL_CRC_HARDWARE
static void test_crc32c_hw_sw_parity(uint64_t *rng) {
    CHECK(crc32c_sw((const uint8_t *)"123456789", 9) == 0xE3069283);
    CHECK(crc32c_hw((const uint8_t *)"123456789", 9) == 0xE3069283);
    
    uint8_t buf[600];

    for (size_t size = 0; size < sizeof(buf); size++) {
        memset(buf, 0, sizeof(buf));

        for (uint32_t i = 0; i < size; i++) {
            buf[i] = (uint8_t)rng_next(rng);
        }

        uint32_t hw = crc32c_hw(buf, size);
        uint32_t sw = crc32c_sw(buf, size);

        CHECK(hw == sw);
    }
}
#endif 

void test_crc32c(uint64_t seed) {
    TIMED(test_crc32c_bitwise(&seed));
#ifdef SPRAWL_CRC_HARDWARE
    TIMED(test_crc32c_hw_sw_parity(&seed));
#endif
}
