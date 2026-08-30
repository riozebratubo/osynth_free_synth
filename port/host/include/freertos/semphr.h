/*
 * osynth host port — freertos/semphr.h
 *
 * Two shapes, and the difference between them matters: xSemaphoreCreateMutex()
 * returns a semaphore that is already available, xSemaphoreCreateBinary() one
 * that is not. presets.cpp and drums/sampler.cpp both create a binary
 * semaphore and then block on it waiting for a worker to signal completion --
 * handing them an available one would have every first wait return
 * immediately with the work not done.
 *
 * Both are counting semaphores underneath with a ceiling of one, which gives
 * mutex semantics without ownership tracking. That is enough here: nothing in
 * the tree takes one of these recursively, and nothing relies on priority
 * inheritance (the sections that would need it are portMUX critical sections,
 * not semaphores).
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* SemaphoreHandle_t;

/* Created available. */
SemaphoreHandle_t xSemaphoreCreateMutex(void);
/* Created taken. */
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t sem);

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);

#ifdef __cplusplus
}
#endif
