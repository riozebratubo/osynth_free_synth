/*
 * osynth host port — esp_heap_caps.h
 *
 * A host has one heap, so the capability bits could all collapse onto malloc.
 * They must not, and the reason is looper_init():
 *
 *     s_base_cap_s = base_cap_for_pool(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
 *
 * (looper.cpp:2231) sizes the loop pool from whatever PSRAM is left *after*
 * fx, drums and engines have taken theirs, and the answer is registered as
 * loop.maxlen's PARAM_INFO default — the number the app draws as "max n s".
 * Handed the free size of a desktop's RAM, that becomes a looper offering
 * hours per track and then trying to reserve them.
 *
 * So MALLOC_CAP_SPIRAM is a *budgeted* pool here, not an alias for malloc:
 * OSYNTH_HOST_SPIRAM_BUDGET_BYTES (sdkconfig.h) is its size, allocations draw
 * it down, frees give it back, and a request past the budget returns NULL
 * exactly as a full PSRAM heap would. Every caller in the tree already treats
 * NULL as "this effect is disabled" rather than as fatal, so the failure path
 * is the one the firmware was written against.
 *
 * MALLOC_CAP_INTERNAL is budgeted separately for the same reason in miniature:
 * graph_render.cpp's buffer pool and synth_line.h's fallback both ask for it,
 * and on hardware it is the scarce one.
 *
 * Every block carries a 32-byte header holding its size and pool, which is why
 * a heap_caps pointer must be freed with heap_caps_free. Nothing in the tree
 * mixes the two families — checked across all 14 components — and the header
 * keeps the SIMD alignment synth_simd.h's vector_size(16) kernels want.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_DEFAULT  (1 << 12)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_32BIT    (1 << 1)

void*  heap_caps_malloc(size_t size, uint32_t caps);
void*  heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void*  heap_caps_realloc(void* ptr, size_t size, uint32_t caps);
void   heap_caps_free(void* ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);

/* Host-only: set the two budgets before any component initialises. Called by
 * host_main.cpp from the values in sdkconfig.h, and exposed so a mobile build
 * can lower them at runtime once it knows the device. */
void heap_caps_host_set_budgets(size_t spiram_bytes, size_t internal_bytes);

#ifdef __cplusplus
}
#endif
