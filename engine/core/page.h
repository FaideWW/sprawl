#ifndef SPRAWL_PAGE_H_
#define SPRAWL_PAGE_H_

#include "result.h"
#include <stddef.h>
#include <stdint.h>

/**
*   PAGE HEADER FORMAT 
*   Offset   Size (bytes)   Field         Notes
*   0        4              checksum      CRC32C over bytes [4,PAGE_SIZE)
*   4        1              kind          superblock, bitmap, branch, leaf
*   5        1              table         table id
*   6        1              level         0 = leaf 
*   7        1              reserved      
*   8        2              count         number of entries 
*   10       6              reserved 
*   16       8              page_num
*   24       8              generation    if generation == current_cycle, the page 
*/ 
#define PAGE_HEADER_SIZE_BYTES 32 

typedef enum {
    PageKind_Invalid = 0,
    PageKind_Superblock = 1,
    PageKind_Bitmap = 2,
    PageKind_Branch = 3,
    PageKind_Leaf = 4,
} PageKind;

typedef struct {
    uint8_t kind;
    uint8_t table;
    uint8_t level;
    uint16_t count;
    uint64_t page_num;
    uint64_t generation;
} sprawl_page_header;

void page_header_read(const uint8_t *page, sprawl_page_header *header);
void page_header_write(uint8_t *page, const sprawl_page_header *header);

void page_checksum_write(uint8_t *page);

SprawlResult page_validate(const uint8_t *page, uint64_t page_num, PageKind kind,
                           uint8_t table, uint64_t generation_max);

uint32_t crc32c(const uint8_t *data, size_t size);
#endif // SPRAWL_PAGE_H_
