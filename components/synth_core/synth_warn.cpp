/*
 * osynth — deferred warnings from the render path. Contract in synth_warn.h.
 */
#include "synth_warn.h"

#include <atomic>

#include "esp_log.h"

#include "synth_config.h" /* SYNTH_RENDER_IRAM — see the note in the header */

namespace osynth::dsp {

namespace {

/* Four is chosen against the number of *distinct* render-path warnings that
 * exist, not against a rate: every caller latches, so the ring holds one entry
 * per fault rather than one per occurrence. It is drained on the heartbeat,
 * which is far faster than new kinds of fault appear. */
constexpr unsigned kSlots = 4;

struct Entry {
    const char* tag;
    const char* msg;
};

Entry s_ring[kSlots];
std::atomic<unsigned> s_head{0}; /* next write slot; audio task only */
std::atomic<unsigned> s_tail{0}; /* next read slot; drain task only */

} // namespace

bool SYNTH_RENDER_IRAM render_warn(const char* tag, const char* msg) {
    if (tag == nullptr || msg == nullptr) return false;
    const unsigned head = s_head.load(std::memory_order_relaxed);
    if (head - s_tail.load(std::memory_order_acquire) >= kSlots) return false;
    s_ring[head % kSlots] = {tag, msg};
    /* Release: the reader that sees this head sees the entry behind it. */
    s_head.store(head + 1, std::memory_order_release);
    return true;
}

void render_warn_drain(void) {
    unsigned tail = s_tail.load(std::memory_order_relaxed);
    const unsigned head = s_head.load(std::memory_order_acquire);
    while (tail != head) {
        const Entry& e = s_ring[tail % kSlots];
        /* The tag travels with the message so the line reads as though the
         * component printed it itself, which is what makes it findable. */
        ESP_LOGW(e.tag, "%s", e.msg);
        ++tail;
    }
    s_tail.store(tail, std::memory_order_release);
}

} // namespace osynth::dsp
