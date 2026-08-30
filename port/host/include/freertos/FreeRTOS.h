/*
 * osynth host port — freertos/FreeRTOS.h
 *
 * The base types and the critical section. Everything else the firmware uses
 * is in task.h, semphr.h and queue.h beside this.
 *
 * ---------------------------------------------------------------------------
 * Why portMUX is a spinlock here and not a std::mutex
 *
 * portENTER_CRITICAL on an ESP32 takes a spinlock and disables interrupts on
 * the calling core, and every one of the eleven muxes in this tree guards a
 * few instructions: a ParamStore descriptor swap, a voice-table update, a drum
 * slot's state, the sequencer's step read. Two of those (synth_params.cpp,
 * synth_voice.cpp) are taken from the audio thread.
 *
 * A std::mutex there would be wrong in the way that matters: on contention it
 * enters the kernel, and the audio thread would be descheduled inside a
 * section written on the assumption that it cannot be. A spin matches both the
 * duration and the intent, and costs nothing when uncontended.
 *
 * The trade is that a host spinlock has no priority inheritance and no
 * interrupt masking, so it is only correct while the sections stay short. They
 * are short today; a mux held across anything blocking would be a bug on the
 * ESP32 first.
 * ---------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int          BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t     TickType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

/* One tick is one millisecond here. The firmware only ever uses ticks through
 * pdMS_TO_TICKS and portTICK_PERIOD_MS, so the rate is ours to choose, and a
 * 1 ms tick makes xTaskGetTickCount() differences read directly as
 * milliseconds -- which is what every deadline in the tree computes with. */
#define configTICK_RATE_HZ  1000
#define portTICK_PERIOD_MS  1U
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFU)

/* Not a std::atomic_flag: portMUX_INITIALIZER_UNLOCKED has to work as a
 * static initialiser for a file-scope object in C++ and in C, and this stays a
 * POD either way. */
typedef struct {
    /* A plain uint32_t, reached through the compiler's atomic intrinsics in
     * freertos_host.cpp. Not a std::atomic: this header is included from C as
     * well as C++ (midi.c takes it), and not volatile either -- volatile
     * orders nothing against other threads, which is the entire job here. */
    uint32_t owned;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {0}

void port_mux_enter_host(portMUX_TYPE* mux);
void port_mux_exit_host(portMUX_TYPE* mux);

#define portENTER_CRITICAL(mux)      port_mux_enter_host(mux)
#define portEXIT_CRITICAL(mux)       port_mux_exit_host(mux)
#define portENTER_CRITICAL_SAFE(mux) port_mux_enter_host(mux)
#define portEXIT_CRITICAL_SAFE(mux)  port_mux_exit_host(mux)

/* The task-level spelling, and the one the tree actually prefers: 106 uses
 * against 40 of the port* form. In ESP-IDF these are the same operation -- its
 * taskENTER_CRITICAL takes a mux, unlike vanilla FreeRTOS where it takes
 * nothing -- so they map to the same pair here. */
#define taskENTER_CRITICAL(mux)      port_mux_enter_host(mux)
#define taskEXIT_CRITICAL(mux)       port_mux_exit_host(mux)
#define taskENTER_CRITICAL_ISR(mux)  port_mux_enter_host(mux)
#define taskEXIT_CRITICAL_ISR(mux)   port_mux_exit_host(mux)

/* IDF's default ceiling. The value matters only as arithmetic: the three call
 * sites ask for `configMAX_PRIORITIES - 2` and the shim ignores priorities
 * (see task.h), but keeping IDF's number means a reader comparing the two
 * builds sees the same expression mean the same thing. */
#define configMAX_PRIORITIES 25

/* Single conceptual core. SYNTH_ENABLE_SPLIT_RENDER is 0 on the host -- the
 * P4 two-core pipeline exists to dodge that chip's interrupt placement and
 * costs a block of latency -- so the one caller of this, in_slot() in
 * audio_io.cpp, is asking a question with one answer. */
BaseType_t xPortGetCoreID(void);

#ifdef __cplusplus
}
#endif
