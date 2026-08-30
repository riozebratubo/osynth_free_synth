/*
 * osynth host port — freertos/task.h
 *
 * Tasks become std::threads. Priority and core affinity are accepted and
 * ignored, which is the one place this shim is knowingly weaker than the thing
 * it replaces: the firmware's whole scheduling story (ARCHITECTURE.md's
 * "Tasks and cores" table, audio at max-2 on core 1) is a statement about a
 * two-core RTOS. A host OS decides for itself.
 *
 * What that costs is real but bounded, and the answer is NOT a larger render
 * block -- that is what this note used to say, and it cost the standalone
 * build its parity with the instrument (see OSYNTH_BLOCK_SIZE in sdkconfig.h
 * for what a 256-frame block did to the FX bus). The slack lives in
 * sink_miniaudio's ring instead, which is sized in device periods: the render
 * thread is paced by a blocking write against several periods' depth, so it
 * absorbs a scheduling delay whatever size it chops its work into. Raising the
 * render thread's priority is a per-platform call and belongs in the sink, not
 * here.
 *
 * Stack size is ignored too: these are host threads with host stacks, and the
 * firmware's figures (6144 for audio, 8192 for drum_ctl) describe a different
 * machine.
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

/* `core` is accepted and ignored; see the header comment. */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name,
                                   uint32_t stack, void* arg,
                                   UBaseType_t prio, TaskHandle_t* out,
                                   BaseType_t core);

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack,
                       void* arg, UBaseType_t prio, TaskHandle_t* out);

/* NULL means the calling task. A host task ends by returning from its
 * function, so this sets a flag the task loop is expected to notice; it does
 * not unwind the thread the way FreeRTOS does. Only usb_host_midi calls it,
 * and that component is not in the host build. */
void vTaskDelete(TaskHandle_t task);

void vTaskDelay(TickType_t ticks);

/* Milliseconds since the process started, by the 1 ms tick FreeRTOS.h sets. */
TickType_t xTaskGetTickCount(void);

TaskHandle_t xTaskGetCurrentTaskHandle(void);

/* The give/take pair the firmware uses as its task wakeup: a counting
 * notification, given by a producer and taken by the one task that waits on
 * it. `clear_on_exit` zeroes the count rather than decrementing it, which is
 * what every caller here passes (pdTRUE) -- they are waking on "there is work"
 * and then draining all of it. Returns the count that was pending, or 0 on
 * timeout. */
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks);
void xTaskNotifyGive(TaskHandle_t task);

#ifdef __cplusplus
}
#endif
