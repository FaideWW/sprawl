#include <stdint.h>
#include <string.h>
#include "bytes.h"
#include "page.h"
#include "profile.h"
#include "result.h"
#include "tests.h"

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

static void test_crc32c(void) {
    CHECK(crc32c((const uint8_t *)"123456789", 9) == 0xE3069283);

    for (uint32_t n = 0; n < 256; n++) {
        uint8_t byte = (uint8_t)n;
        CHECK(crc32c(&byte, 1) == crc32c_bitwise(&byte, 1));
    }
}

static void page_make_valid(uint8_t *page, const sprawl_page_header *header) {
    memset(page, 0, PAGE_SIZE_BYTES);
    page_header_write(page, header);
    page_checksum_write(page);
}

static void test_page_validate(void) {
    static uint8_t page[PAGE_SIZE_BYTES];
    sprawl_page_header header = {
        .kind = PageKind_Leaf,
        .table = 1,
        .level = 0,
        .count = 3,
        .page_num = 42,
        .generation = 7,
    };

    page_make_valid(page, &header);
    
    sprawl_page_header readback;
    page_header_read(page, &readback);
    // check header_read reads back the same values
    CHECK(readback.kind == header.kind);
    CHECK(readback.table == header.table);
    CHECK(readback.level == header.level);
    CHECK(readback.count == header.count);
    CHECK(readback.page_num == header.page_num);
    CHECK(readback.generation == header.generation);

    // validates OK
    CHECK(page_validate(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_OK);
    CHECK(page_validate(page, 42, PageKind_Leaf, 1, 8) == SprawlResult_OK);

    // wrong expected values
    CHECK(page_validate(page, 43, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);
    CHECK(page_validate(page, 42, PageKind_Branch, 1, 7) == SprawlResult_Corrupt);
    CHECK(page_validate(page, 42, PageKind_Leaf, 2, 7) == SprawlResult_Corrupt);
    CHECK(page_validate(page, 42, PageKind_Leaf, 1, 6) == SprawlResult_Corrupt);

    // checksum invalid 
    page_make_valid(page, &header);
    page[100] ^= 0x01;
    CHECK(page_validate(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);
    

    // reserved bytes nonzero
    page_make_valid(page, &header);
    page[7] = 0x01;
    page_checksum_write(page);
    CHECK(page_validate(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);
    page_make_valid(page, &header);
    page[12] = 0x01;
    page_checksum_write(page);
    CHECK(page_validate(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);
    
    // all-zero page
    memset(page, 0, PAGE_SIZE_BYTES);
    CHECK(page_validate(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);

    // leaf nonzero level
    page_make_valid(page, &header);
    page[6] = 1;
    page_checksum_write(page);
    CHECK(page_validate(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);

    // leaf has no table
    page_make_valid(page, &header);
    page[5] = 0;
    page_checksum_write(page);
    CHECK(page_validate(page, 42, PageKind_Leaf, 0, 7) == SprawlResult_Corrupt);

}

void test_page(void) {
    test_crc32c();
    test_page_validate();
}
