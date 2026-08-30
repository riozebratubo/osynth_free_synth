/*
 * osynth host port — the monotonic clock and the periodic timer.
 *
 * One thread per timer. There is exactly one timer in the host build (the
 * sequencer clock), so a thread pool would be machinery for a single user; if
 * a second ever appears this is still the cheapest thing that is correct.
 */
#include "esp_timer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point boot_time() {
    static const Clock::time_point t0 = Clock::now();
    return t0;
}

struct BootTimeInit {
    BootTimeInit() { (void)boot_time(); }
} g_boot_time_init;

}  // namespace

struct esp_timer {
    esp_timer_cb_t callback = nullptr;
    void* arg = nullptr;
    const char* name = "";
    bool skip_unhandled = false;

    std::mutex m;
    std::condition_variable cv;
    std::thread thread;

    /* Guarded by m. `period_us` of 0 with running=true is a one-shot. */
    uint64_t period_us = 0;
    bool running = false;
    bool one_shot = false;
    bool quit = false;
    /* Bumped by start/restart/stop so a thread already waiting on an old
     * deadline re-reads the schedule instead of firing against it. */
    uint64_t generation = 0;
};

namespace {

void timer_thread(esp_timer* t) {
    std::unique_lock<std::mutex> lk(t->m);
    for (;;) {
        /* Idle: nothing scheduled. */
        while (!t->quit && !t->running) t->cv.wait(lk);
        if (t->quit) return;

        const uint64_t gen = t->generation;
        const uint64_t period = t->period_us;
        auto next = Clock::now() + std::chrono::microseconds(period);

        for (;;) {
            /* wait_until returns on timeout or on any notify; the generation
             * check is what tells the two apart, since a restart while we are
             * parked here must abandon this deadline. */
            if (t->cv.wait_until(lk, next) == std::cv_status::no_timeout) {
                if (t->quit || t->generation != gen || !t->running) break;
                continue; /* spurious wake: the deadline still stands */
            }
            if (t->quit || t->generation != gen || !t->running) break;

            const bool one_shot = t->one_shot;
            esp_timer_cb_t cb = t->callback;
            void* arg = t->arg;

            /* The callback runs unlocked: seqarp's tick notifies the clock
             * task, and holding the timer lock across that would put this
             * thread in the path of anything that task touches. */
            lk.unlock();
            if (cb != nullptr) cb(arg);
            lk.lock();

            if (t->quit || t->generation != gen) break;
            if (one_shot) {
                t->running = false;
                break;
            }

            const auto now = Clock::now();
            next += std::chrono::microseconds(period);
            if (next < now) {
                /* Late. skip_unhandled_events means "do not make up the
                 * missed ticks" -- seqarp asks for it so a stall costs one
                 * late tick and not an avalanche of catch-up ones. Without
                 * it, walk the deadline forward tick by tick as IDF does. */
                if (t->skip_unhandled) {
                    next = now + std::chrono::microseconds(period);
                } else {
                    while (next < now) {
                        next += std::chrono::microseconds(period);
                    }
                }
            }
        }
    }
}

}  // namespace

int64_t esp_timer_get_time(void) {
    const auto d = Clock::now() - boot_time();
    return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(d)
        .count();
}

esp_err_t esp_timer_create(const esp_timer_create_args_t* args,
                           esp_timer_handle_t* out) {
    if (args == nullptr || out == nullptr || args->callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* t = new (std::nothrow) esp_timer();
    if (t == nullptr) return ESP_ERR_NO_MEM;

    t->callback = args->callback;
    t->arg = args->arg;
    t->name = args->name != nullptr ? args->name : "";
    t->skip_unhandled = args->skip_unhandled_events;
    t->thread = std::thread(timer_thread, t);

    *out = t;
    return ESP_OK;
}

static esp_err_t timer_arm(esp_timer_handle_t t, uint64_t period_us,
                           bool one_shot) {
    if (t == nullptr || period_us == 0) return ESP_ERR_INVALID_ARG;
    {
        std::lock_guard<std::mutex> lk(t->m);
        if (t->running) return ESP_ERR_INVALID_STATE;
        t->period_us = period_us;
        t->one_shot = one_shot;
        t->running = true;
        ++t->generation;
    }
    t->cv.notify_all();
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer,
                                   uint64_t period_us) {
    return timer_arm(timer, period_us, false);
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
    return timer_arm(timer, timeout_us, true);
}

esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t period_us) {
    if (timer == nullptr || period_us == 0) return ESP_ERR_INVALID_ARG;
    {
        std::lock_guard<std::mutex> lk(timer->m);
        if (!timer->running) return ESP_ERR_INVALID_STATE;
        timer->period_us = period_us;
        ++timer->generation;
    }
    timer->cv.notify_all();
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    {
        std::lock_guard<std::mutex> lk(timer->m);
        if (!timer->running) return ESP_ERR_INVALID_STATE;
        timer->running = false;
        ++timer->generation;
    }
    timer->cv.notify_all();
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    {
        std::lock_guard<std::mutex> lk(timer->m);
        timer->quit = true;
        timer->running = false;
        ++timer->generation;
    }
    timer->cv.notify_all();
    if (timer->thread.joinable()) timer->thread.join();
    delete timer;
    return ESP_OK;
}
