#ifndef SPRAWL_PAGE_H_
#define SPRAWL_PAGE_H_

#if defined(__SSE4_2__)  && !defined(SPRAWL_CRC_SOFTWARE)
#define SPRAWL_CRC_HARDWARE
#endif

#include <stddef.h>
#include <stdint.h>
#include "bytes.h"
#include "result.h"

/**
*   PAGE HEADER FORMAT 
*   Offset   Size (bytes)   Field         Notes
*   0        4              checksum      CRC32C over bytes [4,PAGE_SIZE)
*   4        1              kind          superblock, freelist, branch, leaf
*   5        1              table         table id
*   6        1              level         0 = leaf 
*   7        1              reserved      
*   8        2              count         number of entries 
*   10       6              reserved 
*   16       8              page_num
*   24       8              generation    if generation == current_cycle, the page 
*/ 
#define PAGE_HEADER_SIZE_BYTES 32 

#define PAGE_LEVEL(n) (uint8_t)(*((n)+6))
#define PAGE_COUNT(n) (get_u16le((n) + 8))
#define PAGE_PAGENUM(n) (get_u64le((n) + 16))
#define PAGE_GENERATION(n) (get_u64le((n) + 24))

typedef enum {
    PageKind_Invalid = 0,
    PageKind_Superblock = 1,
    PageKind_Freelist = 2,
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

SprawlResult page_checksum_verify(const uint8_t *page);
SprawlResult page_header_verify(const uint8_t *page, uint64_t page_num, PageKind kind,
                                uint8_t table, uint64_t generation_max);

uint32_t crc32c_sw(const uint8_t *data, size_t size);
#ifdef SPRAWL_CRC_HARDWARE
uint32_t crc32c_hw(const uint8_t *data, size_t size);
#endif // SPRAWL_CRC_HARDWARE

uint32_t crc32c(const uint8_t *data, size_t size);

#endif // SPRAWL_PAGE_H_
