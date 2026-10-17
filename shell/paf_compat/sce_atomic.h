/* sce_atomic.h - stub for Sony PSP2 atomic operations */
#ifndef _SCE_ATOMIC_H
#define _SCE_ATOMIC_H
#include <stdint.h>

static inline int32_t sceAtomicDecrement32(volatile int32_t *ptr) {
    return __sync_sub_and_fetch(ptr, 1);
}

static inline int32_t sceAtomicIncrement32(volatile int32_t *ptr) {
    return __sync_add_and_fetch(ptr, 1);
}

static inline int32_t sceAtomicDecrement32AcqRel(volatile int32_t *ptr) {
    return __sync_sub_and_fetch(ptr, 1);
}

static inline int32_t sceAtomicIncrement32AcqRel(volatile int32_t *ptr) {
    return __sync_add_and_fetch(ptr, 1);
}

static inline int32_t sceAtomicCompareAndSwap32(volatile int32_t *ptr,
    int32_t compare, int32_t newval) {
    return __sync_val_compare_and_swap(ptr, compare, newval);
}

#endif /* _SCE_ATOMIC_H */
