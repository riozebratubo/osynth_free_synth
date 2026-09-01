/*
 * osynth host port — logging, error names, the cycle counter and the ROM CRC.
 *
 * The small leftovers: one file rather than four, because none of them is more
 * than a few dozen lines and they share nothing but being IDF surface.
 */
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "synth_file.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__ANDROID__)
/* Android closes stderr, so every line the log shim writes would otherwise be
 * formatted and thrown away -- which is exactly what happened to "no audio
 * device could be opened" while the standalone app sat silent on a phone and
 * said nothing about it anywhere. logcat is where a phone's diagnostics live. */
#include <android/log.h>
#endif

#if defined(_MSC_VER)
/* MoveFileExA, for the rename shim below. WIN32_LEAN_AND_MEAN keeps the rest
 * of the Windows headers -- and their `min`/`max` macros -- out of a
 * translation unit that has no use for them. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point boot_time() {
    static const Clock::time_point t0 = Clock::now();
    return t0;
}

struct BootTimeInit {
    BootTimeInit() { (void)boot_time(); }
} g_boot_time_init;

int64_t elapsed_us() {
    const auto d = Clock::now() - boot_time();
    return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(d)
        .count();
}

/* ---- log levels --------------------------------------------------------- */

/* A fixed table rather than a map: the callers are loop_store raising the four
 * sdmmc tags around a mount attempt and putting them back, so the working set
 * is single digits and a linear scan beats a hash. Overflow is not an error --
 * a tag that does not fit simply keeps the default level. */
constexpr int kMaxTagLevels = 16;

struct TagLevel {
    const char* tag;
    esp_log_level_t level;
};

std::mutex g_log_mutex;
TagLevel g_tag_levels[kMaxTagLevels];
int g_tag_level_count = 0;
esp_log_level_t g_default_level = ESP_LOG_INFO;

#if defined(__ANDROID__)
/* IDF's levels onto Android's. The same order, different numbers, and no
 * arithmetic relationship between the two worth relying on. */
int android_priority(esp_log_level_t l) {
    switch (l) {
        case ESP_LOG_ERROR: return ANDROID_LOG_ERROR;
        case ESP_LOG_WARN: return ANDROID_LOG_WARN;
        case ESP_LOG_INFO: return ANDROID_LOG_INFO;
        case ESP_LOG_DEBUG: return ANDROID_LOG_DEBUG;
        default: return ANDROID_LOG_VERBOSE;
    }
}
#endif

const char* level_letter(esp_log_level_t l) {
    switch (l) {
        case ESP_LOG_ERROR: return "E";
        case ESP_LOG_WARN: return "W";
        case ESP_LOG_INFO: return "I";
        case ESP_LOG_DEBUG: return "D";
        default: return "V";
    }
}

}  // namespace

void esp_log_level_set(const char* tag, esp_log_level_t level) {
    if (tag == nullptr) return;
    std::lock_guard<std::mutex> lk(g_log_mutex);

    /* IDF treats "*" as the global default. */
    if (std::strcmp(tag, "*") == 0) {
        g_default_level = level;
        return;
    }
    for (int i = 0; i < g_tag_level_count; ++i) {
        if (std::strcmp(g_tag_levels[i].tag, tag) == 0) {
            g_tag_levels[i].level = level;
            return;
        }
    }
    if (g_tag_level_count < kMaxTagLevels) {
        g_tag_levels[g_tag_level_count].tag = tag;
        g_tag_levels[g_tag_level_count].level = level;
        ++g_tag_level_count;
    }
}

esp_log_level_t esp_log_level_get(const char* tag) {
    if (tag == nullptr) return g_default_level;
    std::lock_guard<std::mutex> lk(g_log_mutex);
    for (int i = 0; i < g_tag_level_count; ++i) {
        if (std::strcmp(g_tag_levels[i].tag, tag) == 0) {
            return g_tag_levels[i].level;
        }
    }
    return g_default_level;
}

void esp_log_write_host(esp_log_level_t level, const char* tag, const char* fmt,
                        ...) {
    if (level > esp_log_level_get(tag)) return;

    /* Format into a local buffer first so one log line reaches stderr as one
     * write. Interleaved fragments from the control tasks and the render
     * thread would make the boot log unreadable exactly when it is being read
     * to diagnose the boot. */
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    const long ms = (long)(elapsed_us() / 1000);
    std::lock_guard<std::mutex> lk(g_log_mutex);
#if defined(__ANDROID__)
    /* The tag becomes logcat's tag rather than part of the message, so
     * `adb logcat -s sink_ma audio_io osynth_host` filters these the way it
     * filters any other Android component. The level letter and the
     * boot-relative milliseconds stay in the text: that is the IDF shape, and
     * keeping it is what lets a phone log and a serial log be read side by
     * side. */
    __android_log_print(android_priority(level), tag != nullptr ? tag : "?",
                        "%s (%ld) %s", level_letter(level), ms, msg);
#else
    std::fprintf(stderr, "%s (%ld) %s: %s\n", level_letter(level), ms,
                 tag != nullptr ? tag : "?", msg);
    std::fflush(stderr);
#endif
}

/* ---- errors ------------------------------------------------------------- */

const char* esp_err_to_name(esp_err_t code) {
    switch (code) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
        case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        default: break;
    }
    /* Never NULL: every caller feeds this straight to a %s. The buffer is
     * per-thread so two tasks logging unknown codes cannot overwrite each
     * other mid-format. */
    static thread_local char unknown[32];
    std::snprintf(unknown, sizeof(unknown), "ERROR 0x%x", (unsigned)code);
    return unknown;
}

void esp_error_check_failed(esp_err_t rc, const char* file, int line,
                            const char* expr) {
    esp_log_write_host(ESP_LOG_ERROR, "ESP_ERROR_CHECK",
                       "failed: %s (0x%x) at %s:%d -- %s", esp_err_to_name(rc),
                       (unsigned)rc, file, line, expr);
    std::abort();
}

void esp_restart(void) {
    /* There is nothing to restart: the engine is a library inside the app.
     * The one caller is the protocol's REBOOT opcode, which exists so the app
     * can make the firmware re-read its boot-time USB role. Logging and
     * returning leaves the app running, which is the honest outcome -- and
     * ctrl_proto should refuse the opcode on a host build rather than report
     * a reboot that did not happen. */
    esp_log_write_host(ESP_LOG_WARN, "host",
                       "esp_restart(): ignored, no firmware to restart");
}

/* ---- cycle counter and ROM helpers -------------------------------------- */

uint32_t esp_cpu_get_cycle_count(void) {
    /* Nanoseconds scaled to the nominal rate. The multiply is done in 64-bit
     * and truncated to 32 like the hardware counter, so the subtractions the
     * render chain does across a wrap still come out right. */
    const auto d = Clock::now() - boot_time();
    const int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    return (uint32_t)((uint64_t)ns * (OSYNTH_HOST_NOMINAL_HZ / 1000000u) /
                      1000u);
}

uint32_t esp_rom_get_cpu_ticks_per_us(void) {
    return OSYNTH_HOST_NOMINAL_HZ / 1000000u;
}

void esp_rom_delay_us(uint32_t us) {
    /* Busy-wait: the callers time register writes in single-digit
     * microseconds, where sleep_for overshoots by orders of magnitude. */
    const int64_t deadline = elapsed_us() + (int64_t)us;
    while (elapsed_us() < deadline) {
        std::this_thread::yield();
    }
}

#if defined(_MSC_VER)
/* See host_compat.h for why this exists. MOVEFILE_REPLACE_EXISTING is the
 * atomic replace; the return convention is C rename()'s (0 on success), since
 * that is what every caller tests. */
int synth_replace_file(const char* tmp, const char* path) {
    if (tmp == nullptr || path == nullptr) return -1;
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
#endif

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* buf, uint32_t len) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
    });

    /* Inverted in and out, exactly as the ROM routine is, so that a 0 seed
     * yields plain zlib.crc32 -- see the header. */
    crc = ~crc;
    for (uint32_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ table[(crc ^ buf[i]) & 0xFFu];
    }
    return ~crc;
}
