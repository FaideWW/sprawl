#include <stdint.h>
#include <string.h>
#include "bytes.h"
#include "page.h"
#include "profile.h"
#include "result.h"
#include "tests.h"

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
    CHECK(page_checksum_verify(page) == SprawlResult_OK);
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_OK);
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 1, 8) == SprawlResult_OK);

    // wrong expected values
    CHECK(page_header_verify(page, 43, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);
    CHECK(page_header_verify(page, 42, PageKind_Branch, 1, 7) == SprawlResult_Corrupt);
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 2, 7) == SprawlResult_Corrupt);
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 1, 6) == SprawlResult_Corrupt);

    // checksum invalid 
    page_make_valid(page, &header);
    page[100] ^= 0x01;
    CHECK(page_checksum_verify(page) == SprawlResult_Corrupt);
    

    // reserved bytes nonzero
    page_make_valid(page, &header);
    page[7] = 0x01;
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);
    page_make_valid(page, &header);
    page[12] = 0x01;
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);
    
    // all-zero page
    memset(page, 0, PAGE_SIZE_BYTES);
    CHECK(page_checksum_verify(page) == SprawlResult_Corrupt);
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);

    // leaf nonzero level
    page_make_valid(page, &header);
    page[6] = 1;
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 1, 7) == SprawlResult_Corrupt);

    // leaf has no table
    page_make_valid(page, &header);
    page[5] = 0;
    CHECK(page_header_verify(page, 42, PageKind_Leaf, 0, 7) == SprawlResult_Corrupt);
}

void test_page(void) {
    TIMED(test_page_validate());
}
