#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "tests.h"
#include "test_posix_file.h"

int main (int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 10) : (uint64_t)time(NULL);
    printf("seed %" PRIu64 "\n", seed);
    TIMED(test_posix_file(seed));
    printf("ok\n");
    return 0;
}
