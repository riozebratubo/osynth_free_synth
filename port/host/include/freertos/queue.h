/*
 * osynth host port — freertos/queue.h
 *
 * One user in the host build: presets.cpp hands load/save requests to the
 * preset task through an 8-deep queue of fixed-size structs. So this is a
 * bounded copy-by-value queue and nothing more -- no peek, no overwrite, no
 * ISR variants (the tree uses none anywhere).
 *
 * xQueueSend with a 0 timeout must fail rather than block when full, because
 * that is how presets_request_* refuses a request instead of stalling the
 * caller, which can be the BLE command task.
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
void vQueueDelete(QueueHandle_t q);

BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t ticks);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);

#ifdef __cplusplus
}
#endif
