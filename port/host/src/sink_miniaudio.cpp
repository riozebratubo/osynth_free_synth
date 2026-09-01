/*
 * osynth host port — the audio device, over miniaudio: output and capture.
 *
 * Both directions live here rather than in a file each, and that is the same
 * arrangement the firmware has for the same reason. On an S3 the line input is
 * the RX half of the *output's* I2S port, so the two share BCLK and WS and a
 * captured block is sample-locked to the block being played. Here they are the
 * two halves of one duplex miniaudio device, which buys exactly that: one
 * device, one clock, one callback delivering a block in each direction.
 *
 * Opening a second device for capture would have been less code and would have
 * given the two independent clocks -- and a slow drift between them that the
 * looper and the vocoder would eventually show.
 *
 * One duplex device does mean the output's fate is tied to the input's, and
 * sink_start() unties it deliberately: a refused capture device is retried as
 * output-only. That is not defensiveness. The capture half is what a phone
 * gates behind a permission and what a desktop loses when a jack is pulled,
 * and neither is a reason for an instrument to stop making sound. See the two
 * long comments at the ma_device_init_ex() calls -- they are the record of the
 * Android silence that made this necessary, including why "the device opened"
 * was not the same question as "the device makes a sound".
 *
 * ---------------------------------------------------------------------------
 * The impedance mismatch this file exists to solve
 *
 * audio_sink.h's contract is a *blocking write*: "write() blocks until the
 * sink has accepted the whole block -- the sink's DMA is the real-time clock
 * that paces the audio task." On the ESP32 that is literal: i2s_channel_write()
 * blocks until DMA has room, so the DAC's own clock decides when the next block
 * is due, and the render task can never run ahead or fall behind unnoticed.
 *
 * Host audio APIs are the other way round. miniaudio (like WASAPI, CoreAudio
 * and AAudio underneath it) *calls you* on its own thread when it wants
 * frames. Nothing blocks; you are handed a deadline instead.
 *
 * So this file is the adaptor: a small ring buffer between the two. The device
 * callback drains it, write() fills it and blocks while it is full. The
 * blocking is not incidental -- it is the whole point. It is what reproduces
 * the pacing the render task is written against, so that audio_io's underrun
 * counter, its DSP-load percentages and its deadline behaviour all keep the
 * meaning they have on hardware.
 *
 * ---------------------------------------------------------------------------
 * Sizing
 *
 * The ring is depth measured in DEVICE periods, not in render blocks -- see
 * kDevicePeriods below for the measurement that forced that. Larger is more
 * tolerant of a scheduler that leaves the render thread waiting, and is more
 * output latency: the whole ring is latency, because a frame written now plays
 * after everything already queued ahead of it.
 *
 * Four device periods is ~40 ms on WASAPI shared mode, which rounds its period
 * up to about 10 ms whatever is asked of it. That is a deliberate trade for a
 * general-purpose OS: the firmware runs three blocks deep at 64 frames because
 * a pinned max-priority task on an RTOS really does get the CPU, and nothing
 * here can promise that. Getting below this means either raising the render
 * thread's priority or asking for an exclusive-mode device, both of which are
 * per-platform and neither of which belongs in a first-sound port.
 */
#include "audio_sink.h"

#include "esp_log.h"
#include "synth_config.h"

#include "miniaudio.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

const char* TAG = "sink_ma";

constexpr size_t kChannels = 2;

/* The ring is sized at start(), from the device's *actual* period rather than
 * from SYNTH_BLOCK_SIZE, and this is the correction that mattered.
 *
 * We ask WASAPI for 256-frame periods and it gives 480: shared mode has a
 * minimum around 10 ms and rounds up to it. A ring of four 256-frame blocks is
 * 1024 frames, which sounds generous and is in fact only 2.1 device periods --
 * so any scheduling delay longer than one period starved it, which on Windows
 * with an ordinary-priority thread happens a couple of times a run. That is
 * exactly what the residual starve count was.
 *
 * So the depth that matters is measured in device periods, and the floor is
 * only a floor.
 *
 * That floor used to be counted in render blocks. It cannot be: the render
 * block became the firmware's 64 (see port/host/include/sdkconfig.h), and four
 * of those is 256 frames -- 5.3 ms against the ~40 ms the measurement above
 * says a general-purpose scheduler needs, so the safety net
 * would have quietly dissolved along with the block size. How deep the ring has
 * to be is a property of the host's scheduler, not of how the renderer chops
 * its work up, so it is written in frames. */
constexpr size_t kRingMinFrames = 1024; /* ~21 ms at 48 kHz */
constexpr size_t kDevicePeriods = 4;    /* depth, in device periods */

/* What sink_start() asks the device for; see the note at that assignment for
 * why it is not SYNTH_BLOCK_SIZE. */
constexpr size_t kDevicePeriodFrames = 256; /* ~5.3 ms at 48 kHz */

/* How long write() waits for room before giving up on a block.
 *
 * It exists so that a device which has stopped consuming -- unplugged, or
 * failed -- cannot wedge the render thread forever. Returning an error instead
 * costs one dropped block, which audio_io counts in sink_errors and reports on
 * the heartbeat; blocking forever would take the whole synth down silently and
 * leave the app connected to something that never answers again.
 *
 * Generously longer than a block period (1.33 ms at 64 frames) and than a
 * device period with it, so a scheduling hiccup never trips it. */
constexpr int kWriteTimeoutMs = 250;

ma_device g_device;
bool g_device_up = false;   /* opened */
bool g_device_running = false; /* started -- see the note in sink_write() */

std::mutex g_mutex;
std::condition_variable g_cv_space;

std::vector<int16_t> g_ring; /* g_ring_frames * kChannels, sized at start() */
size_t g_ring_frames = 0;
size_t g_read = 0;  /* frame index of the oldest queued frame */
size_t g_count = 0; /* frames queued */

#if SYNTH_ENABLE_LINE_IN
/* The capture side: a ring, for the same reason the playback side has one.
 *
 * The two ends do not agree on a block size and cannot be made to. WASAPI
 * hands over 480-frame periods whatever is asked of it; the render chain
 * consumes SYNTH_BLOCK_SIZE (64) at a time. So each device callback delivers
 * 7.5 render blocks, and the leftover has to be kept somewhere. The ratio grew
 * when the block came down to the firmware's 64 -- it was 1.9 at 256 -- which
 * changes nothing here: a ring is what makes the ratio irrelevant, and it is
 * never a whole number either way.
 *
 * An earlier version kept a single block and truncated each callback to it.
 * That looked correct and metered correctly -- real audio arrived, peaks moved
 * -- while discarding 224 of every 480 frames and starving the reader on 44%
 * of blocks. Both counters below climbed together, which is the signature: the
 * reader finding nothing *and* the writer finding the previous block
 * uncollected means the two are exchanging at different sizes, not that either
 * is too slow.
 *
 * What stays different from the playback ring is the contract: the sink's
 * write() blocks to pace the render thread, while audio_source_i2s_read() must
 * never block and never pace -- it is called from the audio task with a zero
 * timeout and returns short rather than wait, and the caller zero-fills the
 * tail. audio_sink.h states that plainly, and it is the opposite of the
 * output's rule. So this ring drops on overflow rather than making the device
 * callback wait.
 */
std::mutex g_cap_mutex;
std::vector<int16_t> g_cap;   /* g_cap_frames * kChannels */
size_t g_cap_frames = 0;      /* capacity, in frames */
size_t g_cap_read = 0;        /* oldest queued frame */
size_t g_cap_count = 0;       /* frames queued */
bool g_cap_open = false;

/* Frames the device delivered that there was no room for. Expected to climb
 * whenever `in.route` is off -- nothing reads then, so the ring fills and
 * stays full by design -- which is why this is a diagnostic for "is the input
 * being consumed", not a fault counter. */
std::atomic<uint32_t> g_cap_dropped{0};
#endif

/* Blocks the device asked for and we could not fill. Distinct from audio_io's
 * own underrun counter, which counts the render chain missing its budget: this
 * counts the render chain not *delivering* in time, whatever the reason. On a
 * host the two come apart -- the DSP can be well inside budget and still be
 * descheduled past the deadline -- and only this one sees that. */
std::atomic<uint32_t> g_starves{0};

void data_callback(ma_device*, void* out, const void* in, ma_uint32 frames) {
#if SYNTH_ENABLE_LINE_IN
    /* Capture first, so that a block handed to the renderer this period is the
     * one recorded in the same period -- the sample-locked relationship the
     * I2S port gives for free and the whole reason this is one duplex device.
     *
     * Copied under the lock rather than published by pointer: `in` belongs to
     * miniaudio and is reused the moment this returns. */
    if (in != nullptr && frames > 0 && g_cap_frames > 0) {
        const auto* src = static_cast<const int16_t*>(in);
        std::lock_guard<std::mutex> lk(g_cap_mutex);
        size_t left = frames;
        size_t at = 0;
        while (left > 0) {
            if (g_cap_count == g_cap_frames) {
                /* Full: drop the oldest rather than the newest. An input is
                 * monitored live, so the freshest frames are the ones worth
                 * keeping -- discarding those would add latency that never
                 * comes back. */
                const size_t drop = left < g_cap_frames ? left : g_cap_frames;
                g_cap_read = (g_cap_read + drop) % g_cap_frames;
                g_cap_count -= drop;
                g_cap_dropped.fetch_add((uint32_t)drop,
                                        std::memory_order_relaxed);
            }
            const size_t w = (g_cap_read + g_cap_count) % g_cap_frames;
            size_t n = g_cap_frames - g_cap_count;
            if (n > left) n = left;
            if (n > g_cap_frames - w) n = g_cap_frames - w;
            std::memcpy(&g_cap[w * kChannels], src + at * kChannels,
                        n * kChannels * sizeof(int16_t));
            g_cap_count += n;
            at += n;
            left -= n;
        }
    }
#else
    (void)in;
#endif

    auto* dst = static_cast<int16_t*>(out);
    size_t got = 0;

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        /* Two runs at most: the queued frames are contiguous from g_read
         * until the ring wraps, then contiguous again from 0. Copying in runs
         * rather than per frame keeps this a pair of memcpys on the device
         * thread, where the deadline is hardest. */
        while (got < frames && g_count > 0) {
            const size_t to_end = g_ring_frames - g_read;
            size_t n = frames - got;
            if (n > g_count) n = g_count;
            if (n > to_end) n = to_end;

            std::memcpy(dst + got * kChannels,
                        g_ring.data() + g_read * kChannels,
                        n * kChannels * sizeof(int16_t));
            g_read = (g_read + n) % g_ring_frames;
            g_count -= n;
            got += n;
        }
    }

    if (got < frames) {
        /* Silence rather than stale audio: a repeated block is a buzz, and a
         * gap is a click. The click is the honest one, and the counter beside
         * it is what says which happened. */
        std::memset(dst + got * kChannels, 0,
                    (frames - got) * kChannels * sizeof(int16_t));
        g_starves.fetch_add(1, std::memory_order_relaxed);
    }

    g_cv_space.notify_one();
}

esp_err_t sink_start(void) {
    if (g_device_up) return ESP_OK;

    /* Duplex where there is an input to capture, playback-only otherwise. The
     * type is what decides whether miniaudio opens a capture device at all, so
     * a build with the input compiled out asks the OS for nothing it will not
     * use -- and never prompts for microphone permission. */
#if SYNTH_ENABLE_LINE_IN
    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.capture.format = ma_format_s16;
    cfg.capture.channels = (ma_uint32)kChannels;
    /* The default device, and shareMode left at its default (shared): an
     * exclusive capture claim would take the microphone away from everything
     * else on the machine. */
    cfg.capture.shareMode = ma_share_mode_shared;
#else
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
#endif
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = (ma_uint32)kChannels;
    cfg.sampleRate = SYNTH_SAMPLE_RATE;
    /* What we ask the device for, and deliberately NOT SYNTH_BLOCK_SIZE.
     *
     * It used to be the block size, back when that was 256 and the two numbers
     * happened to agree. They no longer do -- the render block is the
     * firmware's 64 now, for the parity reasons in port/host/include/
     * sdkconfig.h -- and passing 64 here would be asking a host audio device
     * to wake 750 times a second. WASAPI shared mode would round it away, but
     * CoreAudio, ALSA and AAudio can all honour a period that small, and
     * granting it would shrink the ring below with it: the ring's depth is
     * kDevicePeriods *of whatever the device settles on*, so a 64-frame period
     * would leave 5.3 ms of slack where the measurement that set kDevicePeriods
     * said 40 ms was the honest number for a general-purpose scheduler.
     *
     * So this is the device's number and SYNTH_BLOCK_SIZE is the renderer's,
     * and the ring is the thing between them that lets them differ -- which is
     * what it was built for. Still a hint: the backend may round it either
     * way, and nothing here depends on getting it. */
    cfg.periodSizeInFrames = (ma_uint32)kDevicePeriodFrames;
    cfg.dataCallback = data_callback;

    /* Every backend miniaudio was compiled with EXCEPT the null one, and that
     * exclusion is the entire reason this list is built by hand.
     *
     * ma_device_init() with a null context calls ma_device_init_ex(), which
     * walks the backends in priority order and keeps the first that opens the
     * device. ma_backend_null is the last entry in that walk, and it is a
     * device in every respect this code can test: it opens, it honours the
     * sample rate, it calls the data callback on a timer, and it throws every
     * frame away.
     *
     * That is what the standalone app got on Android. AAudio was refused the
     * capture stream (no RECORD_AUDIO), OpenSL|ES was refused it for the same
     * reason, and miniaudio returned MA_SUCCESS holding a silent device. Every
     * layer above reported success -- audio_io never reached its own null-sink
     * fallback, because nothing had failed -- and the synth rendered into
     * nothing for as long as the app was open.
     *
     * Named backends turn that into an error the retry below can act on. Where
     * there genuinely is no audio hardware, sink_start() now returns ESP_FAIL
     * and audio_io installs its OWN null sink: the same silence, chosen and
     * logged in the one place that knows to say so.
     *
     * Built rather than written out, because it has to track whatever this
     * miniaudio was compiled with; ma_backend_null is documented as the last
     * enumerator, so counting up to it is exactly "all the real ones". The
     * default *device* is still miniaudio's to pick -- which output to use is
     * a setting the app should own, not something to bake in here. */
    ma_backend backends[ma_backend_null];
    for (ma_uint32 i = 0; i < (ma_uint32)ma_backend_null; ++i) {
        backends[i] = (ma_backend)i;
    }

    ma_result mr = ma_device_init_ex(backends, (ma_uint32)ma_backend_null,
                                     nullptr, &cfg, &g_device);

#if SYNTH_ENABLE_LINE_IN
    /* Whether the device that opened has a capture half, and the whole reason
     * there are two attempts here rather than one.
     *
     * A refused input must never cost the synth its output. On Android an app
     * without RECORD_AUDIO is refused the capture stream, and miniaudio's
     * AAudio backend opens the capture half of a duplex device FIRST -- so the
     * playback stream is never even attempted and the whole device is refused.
     * A working DAC would go silent behind a permission the synth does not
     * need in order to make a sound. The same shape appears on macOS with the
     * microphone refused in Settings, and on any machine whose default input
     * was unplugged between boot and here.
     *
     * Note that this only became a *visible* refusal with the named backend
     * list above. Before it, the walk carried on past the two real backends
     * and handed back a null device, so nothing here ever ran.
     *
     * So: ask for both, and if that is refused ask for the output alone. What
     * is lost is the line input, the vocoder's modulator and the granular
     * capture -- audio_source_i2s_ready() answers false below and audio_io
     * leaves `in.route` and `in.gain` registered and inert, which is already
     * its behaviour for a device that was refused. Everything else plays. */
    bool capture_up = (mr == MA_SUCCESS);
    if (!capture_up) {
        ESP_LOGW(TAG,
                 "no capture device (%s); opening the output alone -- line "
                 "input, vocoder and granular capture are off",
                 ma_result_description(mr));
        /* Reusing cfg: the init reads deviceType out of it, and every other
         * field is the same request. The capture.* fields left set are ignored
         * for a playback device. */
        cfg.deviceType = ma_device_type_playback;
        mr = ma_device_init_ex(backends, (ma_uint32)ma_backend_null, nullptr,
                               &cfg, &g_device);
    }
#endif

    if (mr != MA_SUCCESS) {
        ESP_LOGE(TAG, "no audio device could be opened (%s)",
                 ma_result_description(mr));
        return ESP_FAIL;
    }

    /* Prime the ring with silence before the device runs.
     *
     * This is half of closing the startup window; sink_write() below is the
     * other half. The device must never be pulling while the ring is empty,
     * and at this point in the sequence the render task does not exist yet --
     * audio_io_start() creates it only after this function returns. Priming
     * alone left two starves per run, because the device drains a full ring in
     * about two of its 480-frame periods and the task took longer than that to
     * appear. A counter that reads non-zero on a healthy run cannot report a
     * fault, so the device is not started here at all.
     *
     * Filling completely rather than partially, because that is where the ring
     * sits in steady state anyway: write() blocks when full and the producer
     * is far faster than real time, so the level rides at the top. Priming to
     * the same level simply starts there instead of climbing to it, and the
     * latency is the ring's depth either way. */
    {
        const size_t by_period =
            (size_t)g_device.playback.internalPeriodSizeInFrames * kDevicePeriods;

        std::lock_guard<std::mutex> lk(g_mutex);
        g_ring_frames = kRingMinFrames > by_period ? kRingMinFrames : by_period;
        g_ring.assign(g_ring_frames * kChannels, 0);
        g_read = 0;
        g_count = g_ring_frames;

#if SYNTH_ENABLE_LINE_IN
        /* Sized from the same two numbers, and starting EMPTY -- the opposite
         * of the playback ring, which starts primed. Nothing has been captured
         * yet, and priming this with silence would hand the renderer a block of
         * nothing and call it input.
         *
         * Zero frames where the capture half was refused above, which is what
         * data_callback tests before it touches this at all. */
        std::lock_guard<std::mutex> cl(g_cap_mutex);
        g_cap_frames = capture_up ? g_ring_frames : 0;
        g_cap.assign(g_cap_frames * kChannels, 0);
        g_cap_read = 0;
        g_cap_count = 0;
#endif
    }

    g_device_up = true;
#if SYNTH_ENABLE_LINE_IN
    /* Only now: a reader that arrived between init and here would find the
     * buffer empty and count a starve for no reason. And never at all where
     * the device that opened has no capture half -- audio_io reads this to
     * decide whether the input exists. */
    g_cap_open = capture_up;
#endif

    /* The device's *actual* rate, which is not necessarily the one asked for:
     * miniaudio resamples between them, and a mismatch is worth seeing in the
     * log because it costs CPU and a little quality that nothing else reports.
     */
    ESP_LOGI(TAG, "%s via %s, %u Hz (asked %d), %u frame periods",
             g_device.playback.name,
             ma_get_backend_name(g_device.pContext->backend),
             (unsigned)g_device.sampleRate, SYNTH_SAMPLE_RATE,
             (unsigned)g_device.playback.internalPeriodSizeInFrames);
    ESP_LOGI(TAG, "ring %u frames (%.1f ms) = %u device periods, block %d",
             (unsigned)g_ring_frames,
             1000.0 * (double)g_ring_frames / SYNTH_SAMPLE_RATE,
             (unsigned)(g_ring_frames /
                        (g_device.playback.internalPeriodSizeInFrames
                             ? g_device.playback.internalPeriodSizeInFrames
                             : 1)),
             SYNTH_BLOCK_SIZE);
    return ESP_OK;
}

esp_err_t sink_write(const int16_t* interleaved, size_t frames) {
    if (!g_device_up) return ESP_ERR_INVALID_STATE;

    /* The other half of closing the startup window: the device is started by
     * the render chain's first block, not by sink_start().
     *
     * At sink_start() the render task does not exist yet -- audio_io_start()
     * creates it only after the sink is up -- so a device started there pulls
     * against a ring nobody is filling, and every callback in that gap is a
     * real hole in the output. Priming alone did not close it: the device
     * drains a full ring in about two of its periods, and the task took longer
     * than that to appear.
     *
     * Here, by contrast, the producer is provably alive (it is calling us) and
     * the ring is already full of primed silence, so the device has the whole
     * ring as runway from its very first callback.
     *
     * At the *top* of the write and not the bottom, which was the first
     * attempt and deadlocked: the ring is primed full, so the write below
     * waits for space, and space only appears once the device is running.
     * Starting it after the write meant each block timed out instead. */
    if (!g_device_running) {
        if (ma_device_start(&g_device) != MA_SUCCESS) {
            ESP_LOGE(TAG, "device would not start");
            return ESP_FAIL;
        }
        g_device_running = true;
        /* Deliberately no log line here. This runs on the audio thread, and
         * esp_log.h in this port says plainly that nothing there is safe to
         * call from the render chain -- it takes a mutex and does a blocking
         * write to stderr. An earlier version logged here and the write itself
         * cost enough time to risk the very starve this code exists to
         * prevent. The fact that the device started is visible in the sink
         * line start() already printed. */
    }

    size_t done = 0;
    while (done < frames) {
        std::unique_lock<std::mutex> lk(g_mutex);
        if (!g_cv_space.wait_for(lk, std::chrono::milliseconds(kWriteTimeoutMs),
                                 [] { return g_count < g_ring_frames; })) {
            /* The device stopped taking frames. Drop the rest of the block and
             * let audio_io count it; see kWriteTimeoutMs. */
            return ESP_ERR_TIMEOUT;
        }

        const size_t write_pos = (g_read + g_count) % g_ring_frames;
        const size_t to_end = g_ring_frames - write_pos;
        size_t n = g_ring_frames - g_count;
        if (n > frames - done) n = frames - done;
        if (n > to_end) n = to_end;

        std::memcpy(g_ring.data() + write_pos * kChannels,
                    interleaved + done * kChannels,
                    n * kChannels * sizeof(int16_t));
        g_count += n;
        done += n;
    }

    return ESP_OK;
}

const audio_sink_t kHostSink = {
    "host",
    sink_start,
    sink_write,
};

}  // namespace

const audio_sink_t* audio_sink_host(void) { return &kHostSink; }

#if SYNTH_ENABLE_LINE_IN

/* The capture contract from audio_sink.h, and note it is the OPPOSITE of the
 * sink's: never block, never pace, return short rather than wait. The primary
 * sink alone decides when the next block is due. */
bool audio_source_i2s_ready(void) { return g_cap_open; }

esp_err_t audio_source_i2s_read(int16_t* interleaved, size_t frames,
                                size_t* frames_read) {
    if (frames_read != nullptr) *frames_read = 0;
    if (interleaved == nullptr || !g_cap_open) return ESP_ERR_INVALID_STATE;

    std::lock_guard<std::mutex> lk(g_cap_mutex);
    /* Short, or nothing at all, is a legal answer and the caller handles it:
     * it zero-fills the tail and counts a starve. That is what a device
     * running fractionally slower than the render thread produces now and
     * then. Handing back stale frames instead would be a repeat, which is
     * audible; a gap is not. */
    size_t want = frames < g_cap_count ? frames : g_cap_count;
    size_t done = 0;
    while (done < want) {
        size_t n = want - done;
        if (n > g_cap_frames - g_cap_read) n = g_cap_frames - g_cap_read;
        std::memcpy(interleaved + done * kChannels,
                    &g_cap[g_cap_read * kChannels],
                    n * kChannels * sizeof(int16_t));
        g_cap_read = (g_cap_read + n) % g_cap_frames;
        g_cap_count -= n;
        done += n;
    }
    if (frames_read != nullptr) *frames_read = done;
    return ESP_OK;
}

uint32_t audio_sink_host_capture_dropped(void) {
    return g_cap_dropped.load(std::memory_order_relaxed);
}

#endif /* SYNTH_ENABLE_LINE_IN */

uint32_t audio_sink_host_starves(void) {
    return g_starves.load(std::memory_order_relaxed);
}

void audio_sink_host_stop(void) {
    if (!g_device_up) return;
#if SYNTH_ENABLE_LINE_IN
    /* Before the device goes, so a read racing the teardown is refused rather
     * than served from a buffer nothing is filling any more. */
    g_cap_open = false;
#endif
    /* Handles both states: ma_device_uninit() stops a running device first,
     * and is a no-op on the stop for one that was opened and never started. */
    ma_device_uninit(&g_device);
    g_device_up = false;
    g_device_running = false;
}
