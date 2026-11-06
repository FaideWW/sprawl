#ifndef SPRAWL_TESTS_H_
#define SPRAWL_TESTS_H_

#include <stdint.h>
#include <time.h>

void test_page(void);
void test_mem_io(void);
void test_cache(uint64_t seed);
void test_btree(uint64_t seed);
void test_crc32c(uint64_t seed);
void test_freelist(uint64_t seed);
void test_store(uint64_t seed);

static double now_s(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define TIMED(call) do { double t0_ = now_s(); call; \
    printf("%-44s %8.1f ms\n", #call, (now_s() - t0_) * 1e3); } while (0)

#define RUN(cache, r, expr) \
    while (((r) = (expr)) == SprawlResult_Pending) { CHECK((cache)->in_flight > 0); cache_poll(cache); }

#endif // SPRAWL_TESTS_H_
