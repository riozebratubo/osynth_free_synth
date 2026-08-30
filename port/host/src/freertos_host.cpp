/*
 * osynth host port — the FreeRTOS primitives the firmware uses.
 *
 * Scope is exactly what components/ calls and no more: tasks, the counting
 * task-notification, mutex/binary semaphores, a bounded queue, the tick, and
 * the portMUX critical section. There are no ISR variants because the tree
 * uses none.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h> /* _mm_pause / __yield, for cpu_relax() */
#endif

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point boot_time() {
    static const Clock::time_point t0 = Clock::now();
    return t0;
}

/* Forces the baseline to be taken at load rather than at whichever call
 * happens to be first, so two threads asking early cannot disagree. */
struct BootTimeInit {
    BootTimeInit() { (void)boot_time(); }
} g_boot_time_init;

/* ---- tasks ------------------------------------------------------------- */

struct HostTask {
    std::thread thread;
    std::mutex m;
    std::condition_variable cv;
    uint32_t notify = 0;
    std::atomic<bool> deleted{false};
    const char* name = "";
};

thread_local HostTask* t_current = nullptr;

BaseType_t task_create(TaskFunction_t fn, const char* name, void* arg,
                       TaskHandle_t* out) {
    auto* t = new (std::nothrow) HostTask();
    if (t == nullptr) return pdFAIL;
    t->name = name != nullptr ? name : "";

    /* Published before the thread starts, so a task that notifies itself --
     * or a producer that read the handle out of *out -- never sees a
     * half-built object. */
    if (out != nullptr) *out = t;

    t->thread = std::thread([t, fn, arg]() {
        t_current = t;
        fn(arg);
    });
    t->thread.detach();
    return pdPASS;
}

/* ---- semaphores -------------------------------------------------------- */

struct HostSem {
    std::mutex m;
    std::condition_variable cv;
    uint32_t count = 0;
    uint32_t max = 1;
};

/* ---- queues ------------------------------------------------------------ */

struct HostQueue {
    std::mutex m;
    std::condition_variable cv_item;  /* a consumer waiting for data */
    std::condition_variable cv_space; /* a producer waiting for room */
    std::vector<uint8_t> buf;
    size_t item_size = 0;
    size_t capacity = 0;
    size_t head = 0;
    size_t count = 0;
};

/* portMAX_DELAY means "no timeout"; anything else is a millisecond count by
 * the 1 ms tick. Kept in one place so the four wait sites below cannot drift
 * on how they read it. */
template <typename Pred>
bool wait_for_ticks(std::unique_lock<std::mutex>& lk,
                    std::condition_variable& cv, TickType_t ticks,
                    Pred ready) {
    if (ticks == portMAX_DELAY) {
        cv.wait(lk, ready);
        return true;
    }
    return cv.wait_for(lk, std::chrono::milliseconds(ticks), ready);
}

/* A pause rather than a yield: the sections these guard are a handful of
 * instructions, and a syscall here would cost more than the spin. Every
 * spelling below is a hint to the core that it is in a spin loop -- falling
 * through to a plain busy loop on an unrecognised architecture is correct,
 * only slower under contention. */
inline void cpu_relax() {
#if defined(_MSC_VER)
#if defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#elif defined(_M_ARM64) || defined(_M_ARM)
    __yield();
#endif
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

}  // namespace

/* ---- critical section --------------------------------------------------- */

/* Atomics on a plain uint32_t, through the compiler rather than through a
 * library type.
 *
 * std::atomic_ref would read better and was the first version, but it is not
 * available everywhere this has to build: the Android NDK's libc++ (r27) does
 * not have it, and the failure is a compile error deep in a header rather than
 * anything that names the real problem.
 *
 * The intrinsics are also the better fit for what portMUX_TYPE is. That struct
 * is included from C as well as C++ -- midi.c takes it -- so its field cannot
 * be a std::atomic, and reaching a plain uint32_t through the compiler's own
 * atomic operations is precisely the arrangement FreeRTOS itself has. */
#if defined(_MSC_VER)
inline bool mux_try_take(uint32_t* p) {
    return _InterlockedCompareExchange(
               reinterpret_cast<volatile long*>(p), 1, 0) == 0;
}
inline void mux_release(uint32_t* p) {
    _InterlockedExchange(reinterpret_cast<volatile long*>(p), 0);
}
#else
inline bool mux_try_take(uint32_t* p) {
    uint32_t expected = 0;
    /* Acquire on success so the section's reads cannot be hoisted above the
     * lock; relaxed on failure, which only leads back into the spin. */
    return __atomic_compare_exchange_n(p, &expected, 1u, /*weak=*/true,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
inline void mux_release(uint32_t* p) {
    /* Release, so every write inside the section is visible to whoever takes
     * the lock next. */
    __atomic_store_n(p, 0u, __ATOMIC_RELEASE);
}
#endif

void port_mux_enter_host(portMUX_TYPE* mux) {
    while (!mux_try_take(&mux->owned)) {
        cpu_relax();
    }
}

void port_mux_exit_host(portMUX_TYPE* mux) { mux_release(&mux->owned); }

BaseType_t xPortGetCoreID(void) { return 0; }

/* ---- tasks -------------------------------------------------------------- */

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name,
                                   uint32_t stack, void* arg, UBaseType_t prio,
                                   TaskHandle_t* out, BaseType_t core) {
    (void)stack;
    (void)prio;
    (void)core;
    return task_create(fn, name, arg, out);
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack,
                       void* arg, UBaseType_t prio, TaskHandle_t* out) {
    (void)stack;
    (void)prio;
    return task_create(fn, name, arg, out);
}

void vTaskDelete(TaskHandle_t task) {
    auto* t = task != nullptr ? static_cast<HostTask*>(task) : t_current;
    if (t != nullptr) t->deleted.store(true, std::memory_order_relaxed);
}

void vTaskDelay(TickType_t ticks) {
    if (ticks == 0) {
        std::this_thread::yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

TickType_t xTaskGetTickCount(void) {
    const auto d = Clock::now() - boot_time();
    return (TickType_t)std::chrono::duration_cast<std::chrono::milliseconds>(d)
        .count();
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return t_current; }

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks) {
    HostTask* t = t_current;
    if (t == nullptr) return 0; /* not a task this shim created */

    std::unique_lock<std::mutex> lk(t->m);
    wait_for_ticks(lk, t->cv, ticks, [t] { return t->notify > 0; });

    const uint32_t got = t->notify;
    if (got > 0) t->notify = clear_on_exit ? 0 : got - 1;
    return got;
}

void xTaskNotifyGive(TaskHandle_t task) {
    if (task == nullptr) return;
    auto* t = static_cast<HostTask*>(task);
    {
        std::lock_guard<std::mutex> lk(t->m);
        ++t->notify;
    }
    t->cv.notify_one();
}

/* ---- semaphores --------------------------------------------------------- */

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    auto* s = new (std::nothrow) HostSem();
    if (s != nullptr) s->count = 1; /* available */
    return s;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    auto* s = new (std::nothrow) HostSem();
    if (s != nullptr) s->count = 0; /* taken */
    return s;
}

void vSemaphoreDelete(SemaphoreHandle_t sem) {
    delete static_cast<HostSem*>(sem);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    if (sem == nullptr) return pdFALSE;
    auto* s = static_cast<HostSem*>(sem);
    std::unique_lock<std::mutex> lk(s->m);
    if (!wait_for_ticks(lk, s->cv, ticks, [s] { return s->count > 0; })) {
        return pdFALSE;
    }
    if (s->count == 0) return pdFALSE;
    --s->count;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    if (sem == nullptr) return pdFALSE;
    auto* s = static_cast<HostSem*>(sem);
    {
        std::lock_guard<std::mutex> lk(s->m);
        /* Giving an already-available semaphore fails in FreeRTOS rather than
         * counting past the ceiling; matching that keeps a double-give a bug
         * here too instead of silently banking a permit. */
        if (s->count >= s->max) return pdFALSE;
        ++s->count;
    }
    s->cv.notify_one();
    return pdTRUE;
}

/* ---- queues ------------------------------------------------------------- */

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    if (length == 0 || item_size == 0) return nullptr;
    auto* q = new (std::nothrow) HostQueue();
    if (q == nullptr) return nullptr;
    q->item_size = item_size;
    q->capacity = length;
    q->buf.resize((size_t)length * item_size);
    return q;
}

void vQueueDelete(QueueHandle_t queue) {
    delete static_cast<HostQueue*>(queue);
}

BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t ticks) {
    if (queue == nullptr || item == nullptr) return pdFALSE;
    auto* q = static_cast<HostQueue*>(queue);
    std::unique_lock<std::mutex> lk(q->m);
    if (!wait_for_ticks(lk, q->cv_space, ticks,
                        [q] { return q->count < q->capacity; })) {
        return pdFALSE;
    }
    if (q->count >= q->capacity) return pdFALSE;

    const size_t tail = (q->head + q->count) % q->capacity;
    std::memcpy(&q->buf[tail * q->item_size], item, q->item_size);
    ++q->count;
    lk.unlock();
    q->cv_item.notify_one();
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void* out, TickType_t ticks) {
    if (queue == nullptr || out == nullptr) return pdFALSE;
    auto* q = static_cast<HostQueue*>(queue);
    std::unique_lock<std::mutex> lk(q->m);
    if (!wait_for_ticks(lk, q->cv_item, ticks, [q] { return q->count > 0; })) {
        return pdFALSE;
    }
    if (q->count == 0) return pdFALSE;

    std::memcpy(out, &q->buf[q->head * q->item_size], q->item_size);
    q->head = (q->head + 1) % q->capacity;
    --q->count;
    lk.unlock();
    q->cv_space.notify_one();
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
    if (queue == nullptr) return 0;
    auto* q = static_cast<HostQueue*>(queue);
    std::lock_guard<std::mutex> lk(q->m);
    return (UBaseType_t)q->count;
}
