/*
 * osynth host port — esp_attr.h
 *
 * IRAM_ATTR exists to keep the render path out of reach of a flash-cache miss
 * (synth_config.h:319 explains the hazard, and tools/iram_budget.py exists
 * because the P4 link runs out of the region). A host has no such region and
 * no such miss, so every one of these expands to nothing.
 *
 * They stay spelled out rather than being dropped from the sources: the
 * attributes are load-bearing on three real targets, and SYNTH_RENDER_IRAM is
 * how the firmware marks "this is the render path" for a reader.
 */
#pragma once

#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR

/* NOINLINE_ATTR is the one attribute here that still has to *do* something.
 * fx.cpp marks vocoder_process() and limiter_process() with it so they keep
 * their own stack frames rather than being folded into fx_process(), which on
 * the ESP32 is a stack-depth measure and here keeps the two out of an already
 * large function. Unlike the placement attributes above it has a real MSVC
 * spelling, so it gets one. */
#if defined(_MSC_VER)
#define NOINLINE_ATTR __declspec(noinline)
#else
#define NOINLINE_ATTR __attribute__((noinline))
#endif
