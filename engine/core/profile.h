#ifndef SPRAWL_PROFILE_H_
#define SPRAWL_PROFILE_H_

#define KB(x) ((x) * 1024)

#if defined(SPRAWL_PROFILE_TEST)
#   define PAGE_SIZE_BYTES 512
#   define DOC_CACHE_SLOTS 32
#elif defined(SPRAWL_PROFILE_BROWSER)
#   define PAGE_SIZE_BYTES KB(16)
#   define DOC_CACHE_SLOTS 4096         // 64MB
#else 
#   error "SPRAWL_PROFILE_ not defined"
#endif

_Static_assert((PAGE_SIZE_BYTES & (PAGE_SIZE_BYTES - 1)) == 0, "PAGE_SIZE_BYTES must be a power of two");

#endif
