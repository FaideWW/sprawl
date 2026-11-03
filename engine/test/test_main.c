#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "tests.h"

static void run_all(uint64_t seed) {
    TIMED(test_mem_io());
    TIMED(test_page());
    TIMED(test_cache(seed));
    TIMED(test_btree(seed));
    TIMED(test_crc32c(seed));
    TIMED(test_freelist(seed));
}


int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 10) : (uint64_t)time(NULL);
    printf("seed %" PRIu64 "\n" , seed);
    TIMED(run_all(seed));
    printf("ok\n");
    return 0;
}
