/*
 * osynth host port — the budgeted heap behind esp_heap_caps.h.
 *
 * See that header for why MALLOC_CAP_SPIRAM is a budget here and not an alias
 * for malloc. This file is the bookkeeping: a 32-byte header per block holding
 * the size and which pool it was drawn from, two atomic running totals, and an
 * aligned allocator underneath so the header does not cost synth_simd.h its
 * 16-byte alignment.
 */
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

/* 32 rather than 16: the header has to be a multiple of the alignment it is
 * preserving, and 16 would leave the payload correctly aligned but the header
 * sharing a cache line with it for no gain. */
constexpr size_t kAlign = 32;
constexpr uint32_t kMagic = 0x4F53484Bu; /* "OSHK" */

enum Pool : uint32_t { kPoolInternal = 0, kPoolSpiram = 1 };

struct BlockHdr {
    uint64_t size; /* payload bytes, as the caller asked for them */
    uint32_t pool;
    uint32_t magic;
};
static_assert(sizeof(BlockHdr) <= kAlign, "header must fit the alignment gap");

std::atomic<size_t> g_budget_spiram{0};
std::atomic<size_t> g_budget_internal{0};
std::atomic<size_t> g_used_spiram{0};
std::atomic<size_t> g_used_internal{0};

/* MALLOC_CAP_SPIRAM wins when both are named: that is IDF's behaviour for the
 * `SPIRAM | 8BIT` pair every caller in the tree writes, and line_alloc() then
 * falls back to an explicit INTERNAL request when this pool says no. */
Pool pool_for(uint32_t caps) {
    return (caps & MALLOC_CAP_SPIRAM) ? kPoolSpiram : kPoolInternal;
}

std::atomic<size_t>& used_of(Pool p) {
    return p == kPoolSpiram ? g_used_spiram : g_used_internal;
}

std::atomic<size_t>& budget_of(Pool p) {
    return p == kPoolSpiram ? g_budget_spiram : g_budget_internal;
}

/* Reserve `n` bytes from a pool, or fail if that would overrun the budget.
 * Compare-exchange rather than fetch_add so a refused request leaves the
 * counter untouched -- two threads racing at the ceiling must not both see
 * room and both take it. */
bool budget_take(Pool p, size_t n) {
    std::atomic<size_t>& used = used_of(p);
    const size_t cap = budget_of(p).load(std::memory_order_relaxed);
    size_t cur = used.load(std::memory_order_relaxed);
    for (;;) {
        if (cur + n > cap) return false;
        if (used.compare_exchange_weak(cur, cur + n, std::memory_order_relaxed))
            return true;
    }
}

void budget_give(Pool p, size_t n) {
    used_of(p).fetch_sub(n, std::memory_order_relaxed);
}

void* aligned_take(size_t bytes) {
#if defined(_MSC_VER)
    return _aligned_malloc(bytes, kAlign);
#else
    /* aligned_alloc requires a size that is a multiple of the alignment. */
    return std::aligned_alloc(kAlign, (bytes + kAlign - 1) / kAlign * kAlign);
#endif
}

void aligned_drop(void* p) {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

BlockHdr* hdr_of(void* payload) {
    return reinterpret_cast<BlockHdr*>(static_cast<uint8_t*>(payload) - kAlign);
}

}  // namespace

void heap_caps_host_set_budgets(size_t spiram_bytes, size_t internal_bytes) {
    g_budget_spiram.store(spiram_bytes, std::memory_order_relaxed);
    g_budget_internal.store(internal_bytes, std::memory_order_relaxed);
}

void* heap_caps_malloc(size_t size, uint32_t caps) {
    if (size == 0) return nullptr;
    const Pool p = pool_for(caps);
    if (!budget_take(p, size)) return nullptr;

    void* raw = aligned_take(size + kAlign);
    if (raw == nullptr) {
        budget_give(p, size);
        return nullptr;
    }
    auto* h = static_cast<BlockHdr*>(raw);
    h->size = size;
    h->pool = p;
    h->magic = kMagic;
    return static_cast<uint8_t*>(raw) + kAlign;
}

void* heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    /* Overflow check before the multiply: line_alloc() passes a length the
     * caller computed from a parameter, and a wrapped product would allocate
     * a small block and then be written as a large one. */
    if (n != 0 && size > (size_t)-1 / n) return nullptr;
    const size_t bytes = n * size;
    void* p = heap_caps_malloc(bytes, caps);
    if (p != nullptr) std::memset(p, 0, bytes);
    return p;
}

void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps) {
    if (ptr == nullptr) return heap_caps_malloc(size, caps);
    if (size == 0) {
        heap_caps_free(ptr);
        return nullptr;
    }
    BlockHdr* h = hdr_of(ptr);
    if (h->magic != kMagic) return nullptr;
    const size_t old = (size_t)h->size;

    void* fresh = heap_caps_malloc(size, caps);
    if (fresh == nullptr) return nullptr; /* the original stays valid */
    std::memcpy(fresh, ptr, old < size ? old : size);
    heap_caps_free(ptr);
    return fresh;
}

void heap_caps_free(void* ptr) {
    if (ptr == nullptr) return;
    BlockHdr* h = hdr_of(ptr);
    if (h->magic != kMagic) return; /* not ours; refuse rather than corrupt */
    budget_give((Pool)h->pool, (size_t)h->size);
    h->magic = 0;
    aligned_drop(h);
}

size_t heap_caps_get_free_size(uint32_t caps) {
    const Pool p = pool_for(caps);
    const size_t cap = budget_of(p).load(std::memory_order_relaxed);
    const size_t used = used_of(p).load(std::memory_order_relaxed);
    return used >= cap ? 0 : cap - used;
}

/* No fragmentation to report: the host allocator underneath compacts nothing
 * we can see, and the one caller (looper.cpp:2369) prints this beside the free
 * size as a sanity check rather than sizing anything from it. */
size_t heap_caps_get_largest_free_block(uint32_t caps) {
    return heap_caps_get_free_size(caps);
}

bool esp_ptr_external_ram(const void* p) {
    if (p == nullptr) return false;
    const BlockHdr* h = hdr_of(const_cast<void*>(p));
    return h->magic == kMagic && h->pool == kPoolSpiram;
}

bool esp_ptr_internal(const void* p) {
    if (p == nullptr) return false;
    const BlockHdr* h = hdr_of(const_cast<void*>(p));
    return h->magic == kMagic && h->pool == kPoolInternal;
}
