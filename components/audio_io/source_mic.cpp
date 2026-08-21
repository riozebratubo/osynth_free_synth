/*
 * osynth — the microphone port (S37): a digital MEMS mic on the *second* I2S
 * controller.
 *
 * The line input (sink_i2s.cpp) is the RX half of the DAC's own port, which is
 * what makes it sample-locked to playback by construction. That is also why
 * there is a second file here rather than a second pin in that one: a port has
 * one RX channel and one DIN pin, and the line input has them. A microphone on
 * other copper needs a controller of its own — the S3 has two and the P4
 * three, so the second one is free on both targets.
 *
 * Two ways to run it, OSYNTH_MIC_SHARE_CLOCKS:
 *
 *   shared  the mic's controller is an I2S *slave* reading the same BCLK and
 *           WS pins the output port already drives. One pin, and the capture
 *           is sample-locked exactly the way the line input is. The GPIO
 *           matrix allows one output driver and any number of input taps on a
 *           pad, and the driver only enables the input on a pin it takes as an
 *           input (i2s_gpio_check_and_set, esp_driver_i2s), so the slave
 *           reading those pins does not disturb the master driving them.
 *   own     the mic's controller masters a BCLK/WS pair of its own. Three
 *           pins, and a fixed but arbitrary phase offset against the output —
 *           not drift, since both dividers hang off the same root clock, but
 *           an offset nothing here measures.
 *
 * A MEMS mic (INMP441, ICS-43434, SPH0645) is mono: it drives the slot its
 * L/R strap selects and leaves the other alone. It is also 24-bit, MSB-first,
 * inside a 32-bit slot, with no mode that says otherwise — the same shape the
 * PCM1808 forces on the main port. Both facts are resolved here, at the read,
 * so everything downstream sees the plain stereo int16 block it already
 * expects from the line input.
 */
#include "audio_sink.h"

#if SYNTH_ENABLE_MIC_IN

#include <atomic>
#include <stdint.h>
#include <stdio.h> /* gpio_dump_io_configuration() takes a FILE* */
#include <string.h>

#include "driver/gpio.h"     /* probe_din(): pull the pad before I2S takes it */
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_rom_sys.h" /* esp_rom_delay_us() */
#include "soc/gpio_num.h"
#include "soc/soc_caps.h"

#include "synth_config.h"

static const char* TAG = "src_mic";

/* ---- bring-up A/B switch ---------------------------------------------------
 *
 * Skip the first N entries of kClockAttempts (below).
 *
 * The chain stops at the first source the *driver* accepts, and driver
 * acceptance is not quite the same question as whether the silicon then
 * produces a usable clock. Every entry left in that table is one the HAL will
 * take without aborting (see OSYNTH_MIC_CLK_HAS_PLL160), and one whose
 * arithmetic the driver checks — so a wrong choice should now show up as
 * `in starve` climbing on every block rather than as anything worse.
 *
 * If that is what the heartbeat shows, raise this by one and try again. It is
 * a source-level switch and not Kconfig on purpose, for the same reason the
 * switches at the top of sink_i2s.cpp are: it is a bring-up instrument for one
 * unresolved question, not a setting anyone should be choosing between. */
#define OSYNTH_MIC_CLK_SKIP 0

/* ---- loopback bring-up test -----------------------------------------------
 *
 * Points this port's DIN somewhere other than the microphone, so that what it
 * captures is known in advance.
 *
 * It exists because "the port delivers full blocks of zeros" has two entirely
 * different causes and no meter distinguishes them: nothing is driving the
 * data pin, or the peripheral is not latching what is on it. A starve counter
 * that stays put proves the DMA is filling at the right rate and says nothing
 * about whether the bits in it came from anywhere; a codec register dump that
 * matches the vendor byte for byte says nothing about it either.
 *   0  off — read the microphone's real data pin.
 *   1  read the *DAC's* data pin instead. Meaningful when this port shares the
 *      output port's clocks, where it decodes the very frames this chip is
 *      emitting and `mic pk` should track `out pk` exactly.
 *   2  read this port's *own WS pin*. The bluntest instrument here and the one
 *      to reach for when 1 is ambiguous: WS is a square wave at the sample
 *      rate that this port is generating itself, so sampling it as data must
 *      produce a large, obvious reading — one slot near full scale and the
 *      other near zero. It depends on no other peripheral, no clock domain
 *      agreement and no external part.
 *
 *      **A zero here does not prove what it looks like it proves**, and that
 *      was found the hard way. `ws == din` is not a case the I2S driver
 *      special-cases: it handles `dout == din` on one port explicitly
 *      (i2s_gpio_loopback_set(), which wires the output and the input on one
 *      pad on purpose), and has no equivalent for a *clock* pin. The generic
 *      path claims WS as an output and DIN as an input independently, and
 *      nothing guarantees the input mux survives the output reservation on the
 *      same pad. So mode 2 reading zero is ambiguous between "the receiver is
 *      not sampling" and "this test does not work" — prefer mode 1, where a
 *      different peripheral drives the pin and no pad is asked to do two jobs
 *      for one channel.
 */
#define OSYNTH_MIC_LOOPBACK_TEST 0

/* ---- pad dump --------------------------------------------------------------
 *
 * Print what the GPIO matrix actually did with this port's four pads, once, as
 * soon as the port is up.
 *
 * It answers a question every other instrument on this port assumes the answer
 * to: that the driver claimed the pins it was given, as outputs where it
 * should and with the input enabled on DIN. i2s_channel_init_std_mode()
 * returning ESP_OK does not say that — a pad another peripheral already holds
 * is a warning in the driver's log and a working channel that reaches nothing,
 * which is a fault this board has produced before (PINMAP.md, GPIO45/46/47).
 *
 * Cheap enough to leave on while the on-board microphone is unresolved: four
 * pads, four lines, once per boot. Turn it off once this port is trusted. */
#define OSYNTH_MIC_DUMP_GPIO 1

/* One more pad for the sweep in audio_source_mic_probe_pads(), or -1.
 *
 * On the Guition JC-ESP32P4-M3-DEV those are GPIO9, the ES8311's DSDIN — an
 * input to the codec, so it should read as floating — and GPIO11, the speaker
 * amplifier's enable, which should read as held LOW because a PA enable has a
 * pull-down keeping the amplifier off.
 *
 * They are swept because those two readings are the evidence for S37h. GPIO11
 * was this port's DIN for five rounds, on the strength of an `i2s_audio:` block
 * attributed to the wrong board, and it produced a hard zero that no codec
 * register could ever have changed. Keeping both pins in the log means the next
 * person sees *why* the data pin is GPIO48 rather than being told. */
#if SYNTH_ENABLE_CODEC_ES8311
#define OSYNTH_MIC_PROBE_EXTRA_GPIO  9  /* ES8311 DSDIN: an input, should float */
#define OSYNTH_MIC_PROBE_EXTRA2_GPIO 11 /* the speaker amp's enable, held low */
#else
#define OSYNTH_MIC_PROBE_EXTRA_GPIO  -1
#define OSYNTH_MIC_PROBE_EXTRA2_GPIO -1
#endif

#if OSYNTH_MIC_LOOPBACK_TEST == 1
#define OSYNTH_MIC_DIN ((gpio_num_t)CONFIG_OSYNTH_I2S_DOUT_GPIO)
#elif OSYNTH_MIC_LOOPBACK_TEST == 2
/* The WS pin this port drives, read back as data. Defined below alongside the
 * other clock pins, so this indirection is resolved after them. */
#define OSYNTH_MIC_DIN OSYNTH_MIC_WS
#else
#define OSYNTH_MIC_DIN ((gpio_num_t)CONFIG_OSYNTH_MIC_DIN_GPIO)
#endif
#if SYNTH_MIC_SHARE_CLOCKS
/* The output port's own pins, read as inputs. Deliberately the same Kconfig
 * symbols sink_i2s.cpp uses rather than a copy the user could set differently:
 * a slave decoding a *different* pin than the master drives is a
 * configuration that looks like a dead microphone, and there is no reason to
 * make it expressible. */
#define OSYNTH_MIC_BCLK ((gpio_num_t)CONFIG_OSYNTH_I2S_BCLK_GPIO)
#define OSYNTH_MIC_WS   ((gpio_num_t)CONFIG_OSYNTH_I2S_WS_GPIO)
#define OSYNTH_MIC_ROLE I2S_ROLE_SLAVE
#else
#define OSYNTH_MIC_BCLK ((gpio_num_t)CONFIG_OSYNTH_MIC_BCLK_GPIO)
#define OSYNTH_MIC_WS   ((gpio_num_t)CONFIG_OSYNTH_MIC_WS_GPIO)
#define OSYNTH_MIC_ROLE I2S_ROLE_MASTER
#endif

/* System clock for this port, and normally there is not one: a bare MEMS mic
 * derives everything from BCLK. It exists for the case where the thing on the
 * other end is a codec (an ES8311 or ES7210 driving a board's on-board mic),
 * which cannot convert without one — and whose symptom for missing it is the
 * same full-blocks-of-zeros every other fault on this port produces.
 *
 * Master mode only. A slave reads the output port's pins, and that port drives
 * its own MCLK for the converter already on it. */
#if defined(CONFIG_OSYNTH_MIC_MCLK_GPIO) && CONFIG_OSYNTH_MIC_MCLK_GPIO >= 0
#define OSYNTH_MIC_MCLK ((gpio_num_t)CONFIG_OSYNTH_MIC_MCLK_GPIO)
#else
#define OSYNTH_MIC_MCLK I2S_GPIO_UNUSED
#endif

/* Kconfig lets the slot width be anything from 16 to 32 because that is the
 * range the hardware accepts, but only the two ends of it have a code path
 * here — anything between would silently take the 16-bit branch and read the
 * wrong half of every word. A build-time stop, since the runtime symptom would
 * be "the mic sounds like noise", which is a long way from the cause. */
static_assert(SYNTH_MIC_SLOT_BITS == 16 || SYNTH_MIC_SLOT_BITS == 32,
              "OSYNTH_MIC_SLOT_BITS must be 16 or 32");

namespace {

/* NULL until the port comes up, and left NULL if it never does:
 * audio_source_mic_ready() then says no and the capture in audio_io.cpp skips
 * the device entirely. An input is an accessory — losing one must never cost
 * the output. */
i2s_chan_handle_t s_rx = nullptr;

/* What one slot looks like in memory. data_bit_width is pinned equal to
 * slot_bit_width rather than left to narrow on the way in, because the
 * hardware's alignment rule for the narrowing case is exactly the kind of
 * thing that reads as a broken microphone when it turns out to be the other
 * end of the word. One width, no inference. */
#if SYNTH_MIC_SLOT_BITS == 32
using mic_word_t = int32_t;
constexpr i2s_data_bit_width_t kDataBits = I2S_DATA_BIT_WIDTH_32BIT;
constexpr i2s_slot_bit_width_t kSlotBits = I2S_SLOT_BIT_WIDTH_32BIT;
#else
using mic_word_t = int16_t;
constexpr i2s_data_bit_width_t kDataBits = I2S_DATA_BIT_WIDTH_16BIT;
constexpr i2s_slot_bit_width_t kSlotBits = I2S_SLOT_BIT_WIDTH_16BIT;
#endif

/* Both slots of one block, as the port delivers them. 512 bytes at the
 * default block size and 32-bit slots. Not DMA memory — i2s_channel_read()
 * copies out of the driver's descriptors into this. */
mic_word_t s_raw[SYNTH_BLOCK_SIZE * 2];

/* Every bit that has appeared on either slot since the last read (S37d).
 *
 * This exists because nothing else on this port can distinguish a data pin
 * nothing drives from one carrying a signal too quiet to survive the pipeline
 * in front of the meter. Two stages crush it: narrow() keeps the top 16 bits of
 * a 24-bit word, and the heartbeat prints the result as %.2f — so everything
 * below about -46 dBFS reads as exactly `mic 0.00/0.00`, which is also what a
 * disconnected pin reads. Five rounds of ES8311 register work were spent inside
 * that ambiguity.
 *
 * An OR of the *magnitudes* resolves it and carries the level with it: the
 * highest set bit is the loudest sample the window saw, and 0x00000000 across a
 * whole window means the pin never left zero — which no register change can
 * fix, and which is the point at which the fault is in copper rather than code.
 *
 * Relaxed atomics, folded once per block rather than once per sample: this runs
 * in the render path, and the reader is a heartbeat that cares about the value
 * and not about which block it landed in. */
std::atomic<uint32_t> s_raw_or[2];

/* Which slot carries the microphone, and what to call it in the boot log.
 * Hoisted to a constant rather than selected inside the ESP_LOGI() call: a
 * preprocessor directive among a macro's arguments is undefined behaviour,
 * however reliably a given compiler happens to accept it. */
#if defined(CONFIG_OSYNTH_MIC_STEREO)
constexpr int kSlot = 0; /* unused; both slots are kept */
constexpr const char* kChannelName = "stereo, both slots";
#elif defined(CONFIG_OSYNTH_MIC_SLOT_RIGHT)
constexpr int kSlot = 1;
constexpr const char* kChannelName = "mono, right slot (L/R strapped high)";
#else
constexpr int kSlot = 0;
constexpr const char* kChannelName = "mono, left slot (L/R strapped low)";
#endif

constexpr int kShift = CONFIG_OSYNTH_MIC_SHIFT;

/* |w| as a bit mask, without the sign extension that would make every negative
 * sample read 0xFFFFFFFF and throw the magnitude away. */
inline uint32_t SYNTH_RENDER_IRAM mag_bits(mic_word_t w) {
    const int32_t v = (int32_t)w;
    return (uint32_t)(v ^ (v >> 31));
}

/* One slot to one int16.
 *
 * With 32-bit slots the mic's 24 bits arrive MSB-first at the top of the
 * word, so the word is the sample scaled by 256 and the wanted int16 is its
 * top half — a shift, not a conversion. kShift moves where that top half is
 * taken from, which is why it is free: each bit of it is 6 dB bought out of
 * precision the int16 pipeline was going to discard anyway.
 *
 * Saturating rather than wrapping. An over-shifted loud passage clipping flat
 * is a thing a player hears and corrects; the same passage wrapping is a
 * full-scale square edge every time the signal crosses, which sounds like the
 * hardware has failed. */
inline int16_t SYNTH_RENDER_IRAM narrow(mic_word_t w) {
#if SYNTH_MIC_SLOT_BITS == 32
    const int32_t v = (int32_t)w >> (16 - kShift);
#else
    const int32_t v = (int32_t)w << kShift;
#endif
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

/* Opens the port with `src` as the clock source and `rate_div` dividing the
 * *declared* sample rate. Split out because it is called once per entry in
 * kClockAttempts below. Leaves the channel registered but uninitialised on
 * failure, which is the state i2s_channel_init_std_mode() may be called from
 * again.
 *
 * On `rate_div`, which is a workaround and should look like one: for a SLAVE
 * the declared rate never reaches a pin. BCLK and WS come from the master, and
 * the driver uses the rate only to size the port's *internal* clock —
 * i2s_std_calculate_clock() computes mclk = rate x total_slot x slot_bits x
 * bclk_div and then refuses the configuration unless the source clock exceeds
 * 1.99 x that. Dividing the declared rate lowers that demand without touching
 * a single edge on the wire; the internal clock stays several times the real
 * BCLK, which is all a slave's serial engine needs. It is nonsense on a
 * MASTER, where the declared rate *is* the output rate, which is why those
 * entries are compiled out below when the mic runs its own clocks. */
esp_err_t mic_init_std(i2s_clock_src_t src, uint32_t rate_div) {
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SYNTH_SAMPLE_RATE / rate_div),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(kDataBits,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = OSYNTH_MIC_MCLK, /* unused unless a codec needs one */
#if SYNTH_MIC_CODEC_MASTER_TEST
            /* The codec drives these; this chip must not. -1 is skipped by
             * i2s_gpio_check_and_set(), so the pads are left alone entirely
             * rather than claimed and driven. See SYNTH_MIC_CODEC_MASTER_TEST
             * in synth_config.h. */
            .bclk = I2S_GPIO_UNUSED,
            .ws = I2S_GPIO_UNUSED,
#else
            .bclk = OSYNTH_MIC_BCLK,
            .ws = OSYNTH_MIC_WS,
#endif
            .dout = I2S_GPIO_UNUSED,
            .din = OSYNTH_MIC_DIN,
            .invert_flags = {},
        },
    };
    std_cfg.clk_cfg.clk_src = src;
    /* ws_width is the WS *high time in BCLK cycles* on this hardware's TDM
     * engine, so it moves with the slot width or the frame is not I2S at all.
     * Same rule, and the same trap, as the main port — see sink_i2s.cpp. */
    std_cfg.slot_cfg.slot_bit_width = kSlotBits;
    std_cfg.slot_cfg.ws_width = SYNTH_MIC_SLOT_BITS;
    return i2s_channel_init_std_mode(s_rx, &std_cfg);
}

struct ClockAttempt {
    i2s_clock_src_t src;
    uint32_t rate_div;
    const char* name;
};

/* Whether I2S_CLK_SRC_PLL_160M may even be *named* on this build.
 *
 * Not a question of whether it would work — a question of whether asking
 * reboots the box. i2s_ll_get_clk_src() (esp32p4/i2s_ll.h) puts PLL_160M
 * behind `#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) >= 300` and lands everything
 * else on `HAL_ASSERT(false && "unsupported clock source")`. That is an abort,
 * not an error return: the driver has already accepted the configuration and
 * computed the dividers by the time the HAL refuses it, so a P4 pinned to
 * rev <3 bootloops in i2s_channel_init_std_mode() rather than falling through
 * to the next entry in the table below. Confirmed the expensive way.
 *
 * So the condition here is the LL's own condition, spelled with the public
 * symbol it resolves to (HAL_CONFIG_CHIP_SUPPORT_MIN_REV is
 * CONFIG_ESP_REV_MIN_FULL, hal/config.h). Other targets keep the source: the
 * S3's LL maps PLL_160M to clk_sel 2 with no revision gate at all. */
#if !defined(CONFIG_IDF_TARGET_ESP32P4) || CONFIG_ESP_REV_MIN_FULL >= 300
#define OSYNTH_MIC_CLK_HAS_PLL160 1
#else
#define OSYNTH_MIC_CLK_HAS_PLL160 0
#endif

/* Clock sources to try, in order, first success wins.
 *
 * Three constraints shaped this list, and each one cost a boot to find.
 *
 * **APLL where this port masters its own pins, and only there** (S37d). The
 * rule here used to be "never APLL", on the grounds that a second channel
 * asking for a different frequency would retune the clock the DAC runs on.
 * Half of that is wrong and the other half does not apply to a master:
 *
 *   - It cannot retune an occupied APLL. i2s_set_get_apll_freq() goes through
 *     esp_clk_tree_src_set_freq_hz(), which returns ESP_ERR_INVALID_STATE when
 *     the PLL is already claimed, logs "APLL is occupied already ... trying to
 *     work at N Hz", and hands back the frequency it is *actually* running at
 *     for the divider arithmetic. The DAC's clock is not moved by asking.
 *   - There is nothing different to ask for. A master's clk_cfg names
 *     mclk = 256 x fs, the identical figure sink_i2s.cpp asks for on the
 *     output port — so both requests compute the same APLL frequency, and the
 *     second one is a no-op by construction.
 *
 * The reason to want it is the reason the output port is on APLL at all: on a
 * 40 MHz XTAL, 12.288 MHz needs a fractional divider (3 + 49/192), so the MCLK
 * this port generates dithers between 75 ns and 100 ns periods — 30% of a
 * period, peak to peak. On the output that produced audible distortion from an
 * ES8388 and cost a bring-up (see sink_i2s.cpp). On *this* port the same pin is
 * the only clock an ES8311 has: its clock manager divides MCLK for the ADC's
 * modulator and decimator and relates the result to the incoming LRCK, and
 * every reference design feeds it a clean 256 x fs.
 *
 * A second consequence, free: with MCLK, BCLK and WS all off the APLL the sink
 * uses, a mic mastering its own pins is sample-locked to the output instead of
 * sitting in a second clock domain that drifts. That was listed as the price of
 * not sharing clocks, and it is no longer one.
 *
 * **Still never APLL for a slave.** There the declared sample rate is divided
 * to make the internal clock legal (see mic_init_std), so the request genuinely
 * would differ from the DAC's — and it buys nothing, because a slave's timing
 * comes from the master's pins and this clock never leaves the peripheral.
 *
 * **Never DEFAULT.** On the P4 the enum reads `I2S_CLK_SRC_DEFAULT = 0` with a
 * comment promising auto-selection, and nothing implements it:
 * I2S_LL_DEFAULT_CLK_SRC, which i2s_get_source_clk_freq() would use to
 * translate that 0, is not defined for any target in IDF 6.0.2. The 0 reaches
 * esp_clk_tree_src_get_freq_hz() as an invalid module clock, the source
 * frequency comes back zero, and every configuration is refused as "sample
 * rate is too large". (The LL layer would have accepted it — on rev <3 it
 * quietly means XTAL — so the two layers disagree about the same constant.)
 * On the S3 it is #defined to PLL_F160M and works by luck of the enum.
 *
 * **PLL_160M only where the HAL will take it**, per the macro above.
 *
 * What that leaves on a rev <3 P4 is XTAL and nothing else, at 40 MHz, against
 * the ~48.9 MHz a slave needs to decode the 32-bit slots an ES8388 or PCM1808
 * front end forces. Which is why the divided-rate entries are not a last
 * resort on that target but *the* mechanism: at /2 the internal clock drops to
 * 12.288 MHz, needs only 24.5 MHz to be legal, and still runs four times the
 * real 3.072 MHz BCLK. See mic_init_std() for why dividing a slave's declared
 * rate changes nothing on the wire.
 *
 * The fractional mclk divider that results (40 / 12.288) is fine here in a way
 * it would not be on the output: this clock never leaves the peripheral. The
 * mic's sample timing comes from the master's BCLK and WS, so jitter on the
 * internal clock has nothing to modulate — the opposite of the DAC's case,
 * where exactly that jitter is what put the main port on APLL.
 *
 * What is NOT here, because it was checked and does not work: mono slot mode.
 * It looks like it should halve the internal clock demand, and it does not —
 * i2s_std_set_slot() hardcodes `handle->total_slot = 2` and only moves
 * `active_slot`, so the bclk term is identical either way. Written down so the
 * next person does not spend the afternoon on it. */
const ClockAttempt kClockAttempts[] = {
#if !SYNTH_MIC_SHARE_CLOCKS && SOC_I2S_SUPPORTS_APLL
    {I2S_CLK_SRC_APLL, 1, "apll"},
#endif
#if OSYNTH_MIC_CLK_HAS_PLL160
    {I2S_CLK_SRC_PLL_160M, 1, "pll160"},
#endif
    {I2S_CLK_SRC_XTAL, 1, "xtal"},
#if SYNTH_MIC_SHARE_CLOCKS
    {I2S_CLK_SRC_XTAL, 2, "xtal, declared rate/2"},
    {I2S_CLK_SRC_XTAL, 4, "xtal, declared rate/4"},
#endif
};

/* 1 << pin, and 0 for I2S_GPIO_UNUSED (-1). A function rather than the obvious
 * expression because shifting by a negative count is undefined even in the arm
 * of a conditional the compiler can fold away. */
inline uint64_t pin_mask(gpio_num_t pin) {
    return ((int)pin >= 0) ? (1ULL << (int)pin) : 0ULL;
}

} // namespace

/* Is anything driving the data pin at all?
 *
 * Runs before the I2S port claims the pad, as a plain GPIO: pull it up, read
 * it, pull it down, read it. A pin nothing drives follows both pulls. A pin
 * something drives ignores at least one of them — the internal pulls are ~45k
 * against any real driver.
 *
 * This is the variable every other test on this port left untouched. The
 * loopback proves the *receiver* samples, but it proves it about the pin the
 * loopback borrows, not about DIN. A register dump proves the codec holds its
 * configuration, not that its output reaches copper. And this board has form:
 * GPIO45/46/47 were assumed usable from a header listing and turned out to be
 * held by something on the carrier, which is a week documented in PINMAP.md.
 *
 * Reading "floating" here means the codec's SDOUT is silent or is not wired to
 * this pad, and no firmware change will ever produce a sample. Reading "driven"
 * means the signal is present and the fault is in how it is being decoded. */
void probe_din(void) {
    const gpio_num_t pin = OSYNTH_MIC_DIN;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << (int)pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) return;
    esp_rom_delay_us(2000);
    const int hi = gpio_get_level(pin);

    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    if (gpio_config(&cfg) != ESP_OK) return;
    esp_rom_delay_us(2000);
    const int lo = gpio_get_level(pin);

    /* Release both pulls before the I2S driver takes the pad. */
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    (void)gpio_config(&cfg);

    if (hi == 1 && lo == 0) {
        ESP_LOGW(TAG,
                 "din probe: GPIO%d FLOATS (follows both pulls) — nothing is "
                 "driving it. The codec's data output is silent or not wired "
                 "here; no I2S or register change can help.",
                 (int)pin);
    } else {
        ESP_LOGI(TAG,
                 "din probe: GPIO%d is driven (pull-up read %d, pull-down read "
                 "%d) — the signal is present, so the fault is in decoding it.",
                 (int)pin, hi, lo);
    }
}

/* Do this port's pads actually toggle? (S37e, retimed in S37f)
 *
 * The one question every other instrument here has quietly assumed the answer
 * to. A *master* fills its DMA ring from the peripheral's internal clock, so a
 * starve counter that stays at zero says the engine is running and says nothing
 * at all about whether one pad is connected to anything. The GPIO dump above is
 * the same kind of evidence one layer out: it proves the matrix routed the
 * signals, not that the pins carry them.
 *
 * And on this board that gap is not theoretical. GPIO45/46/47 read a static
 * 0.6 V as outputs with nothing attached, held by something on the carrier that
 * no header listing mentioned, and it cost a week of looking at converters.
 *
 * So: enable the pad's *input* next to the peripheral's output —
 * gpio_input_enable() sets the IE bit and touches neither the output enable nor
 * the matrix — and read the pin a few thousand times. MCLK at 12.288 MHz, BCLK
 * at 3.072 MHz and WS at 48 kHz are all fast enough relative to this loop that
 * a live pad returns both levels within a few hundred samples. One level for
 * the whole window means the pad is not switching, and no register in any codec
 * can help that.
 *
 * DIN is in the list too, as a second opinion on the `raw` field in the
 * heartbeat: same reading, arrived at without the I2S receiver in the path.
 *
 * Each pad is read twice, under a pull-up and under a pull-down (S37g), because
 * "not switching" is two findings and on a data pin they point opposite ways.
 * A converter that is clocked, unmuted and converting silence holds ASDOUT low
 * on purpose; a converter whose output stage is off — or a pad that reaches no
 * converter at all — floats. The internal pulls are ~45k, so any real driver
 * wins both readings and a floating pin follows both. Same instrument as
 * probe_din() above, applied to every pad and, crucially, applied again once
 * the codec has been configured. */
void audio_source_mic_probe_pads(const char* when) {
    struct Probe {
        gpio_num_t pin;
        const char* name;
    };
    const Probe kProbes[] = {
        {OSYNTH_MIC_BCLK, "bclk"},
        {OSYNTH_MIC_WS, "ws"},
        {OSYNTH_MIC_MCLK, "mclk"},
        {OSYNTH_MIC_DIN, "din"},
        {(gpio_num_t)OSYNTH_MIC_PROBE_EXTRA_GPIO, "extra"},
        {(gpio_num_t)OSYNTH_MIC_PROBE_EXTRA2_GPIO, "extra2"},
    };
    const int kSamples = 4096;
    for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
        const gpio_num_t pin = kProbes[i].pin;
        if ((int)pin < 0) continue;
        if (gpio_input_enable(pin) != ESP_OK) continue;

        int up = 0, dn = 0;
        (void)gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
        for (int n = 0; n < kSamples; ++n) up += gpio_get_level(pin);
        (void)gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        for (int n = 0; n < kSamples; ++n) dn += gpio_get_level(pin);
        (void)gpio_set_pull_mode(pin, GPIO_FLOATING);

        const bool moved_up = (up > 0 && up < kSamples);
        const bool moved_dn = (dn > 0 && dn < kSamples);
        if (moved_up || moved_dn) {
            /* Report the pull-down pass: on a real signal the two agree, and
             * where they do not the pulled-down figure is the conservative one. */
            ESP_LOGI(TAG, "pad probe (%s): %s GPIO%d toggles (high %d%% of %d)",
                     when, kProbes[i].name, (int)pin,
                     dn * 100 / kSamples, kSamples);
        } else if (up == kSamples && dn == 0) {
            ESP_LOGW(TAG,
                     "pad probe (%s): %s GPIO%d FLOATS — it follows both pulls, "
                     "so nothing is driving this pad",
                     when, kProbes[i].name, (int)pin);
        } else {
            ESP_LOGW(TAG,
                     "pad probe (%s): %s GPIO%d is held %s — something drives "
                     "it, but statically",
                     when, kProbes[i].name, (int)pin, dn ? "HIGH" : "LOW");
        }
    }
}

esp_err_t audio_source_mic_start(void) {
    probe_din();
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, OSYNTH_MIC_ROLE);
    chan_cfg.dma_desc_num = 4;                 /* as the main port, ~5 ms */
    chan_cfg.dma_frame_num = SYNTH_BLOCK_SIZE; /* one render block per buffer */

    /* RX only: nothing on this port ever transmits. I2S_NUM_AUTO takes the
     * next free controller, which is the one the output port did not. */
    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &s_rx);
    if (err != ESP_OK) {
        s_rx = nullptr;
        ESP_LOGW(TAG, "no free I2S controller for the mic (%s)",
                 esp_err_to_name(err));
        return err;
    }

    /* Walk kClockAttempts until one is accepted; see the table for why the
     * order is what it is and why APLL is not in it. A refused attempt leaves
     * the channel registered-but-uninitialised, which is exactly the state the
     * next i2s_channel_init_std_mode() may be called from, so this needs no
     * teardown between tries.
     *
     * If every attempt is refused the mic simply does not come up, which is
     * the correct outcome: nothing in this file is load-bearing for the
     * output. That is also what lets this port be on by default on the P4
     * while the silicon question is open — the failure is a log line. */
    const size_t n_attempts = sizeof(kClockAttempts) / sizeof(kClockAttempts[0]);
    const char* src_name = "none";
    err = ESP_ERR_INVALID_ARG;

    /* i2s_std logs three lines at ERROR for every configuration it refuses,
     * and on a rev <3 P4 the first attempt is *expected* to be refused — XTAL
     * cannot reach 32-bit slots at the undivided rate, which is the whole
     * reason the entries after it exist. Three red lines per boot on a path
     * that is working as designed is how people learn to skim red lines, so
     * the driver is quietened for any attempt there is still a candidate
     * after, and left alone for the last one. A genuinely unrecoverable
     * failure therefore still explains itself in the driver's own words; only
     * the recoverable ones go quiet, and each still gets our warning below
     * naming the source that was turned down. */
    const esp_log_level_t prev_level = esp_log_level_get("i2s_std");
    for (size_t i = OSYNTH_MIC_CLK_SKIP; i < n_attempts; ++i) {
        esp_log_level_set("i2s_std",
                          (i + 1 < n_attempts) ? ESP_LOG_NONE : prev_level);
        err = mic_init_std(kClockAttempts[i].src, kClockAttempts[i].rate_div);
        if (err == ESP_OK) {
            src_name = kClockAttempts[i].name;
            break;
        }
        ESP_LOGW(TAG, "clock source '%s' refused (%s)", kClockAttempts[i].name,
                 esp_err_to_name(err));
    }
    esp_log_level_set("i2s_std", prev_level);

    if (err == ESP_OK) err = i2s_channel_enable(s_rx);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mic port did not come up (%s); no mic input",
                 esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = nullptr;
        return err;
    }

    /* Everything a bench session needs to tell "wrong pin" from "wrong slot"
     * from "wrong clock", in one line, before a single sample is read. */
    ESP_LOGI(TAG, "mic: %s, din=%d bclk=%d ws=%d mclk=%d, %d-bit slots, clk %s",
             SYNTH_MIC_SHARE_CLOCKS ? "slave on the DAC's clocks"
                                    : "master on its own clocks",
             (int)OSYNTH_MIC_DIN, (int)OSYNTH_MIC_BCLK, (int)OSYNTH_MIC_WS,
             (int)OSYNTH_MIC_MCLK, SYNTH_MIC_SLOT_BITS, src_name);
    ESP_LOGI(TAG, "mic: %s, digital gain %d bits (+%d dB)", kChannelName,
             kShift, kShift * 6);
#if OSYNTH_MIC_DUMP_GPIO
    /* Read each row for one thing: the clock pins claimed as *outputs* with an
     * I2S signal on them, and DIN claimed as an *input*. A pad that is neither
     * is one the matrix never took, which is the difference between a port that
     * is misconfigured and a port that is not connected to anything. */
    (void)gpio_dump_io_configuration(
        stdout, pin_mask(OSYNTH_MIC_DIN) | pin_mask(OSYNTH_MIC_BCLK) |
                    pin_mask(OSYNTH_MIC_WS) | pin_mask(OSYNTH_MIC_MCLK));
#endif
    /* The baseline: clocks running, codec not yet told anything. main.cpp takes
     * the same reading again after codec_mic_init(), and the pair is the point
     * — DIN standing still here means nothing at all, since nothing has asked
     * the converter for a sample yet. */
    audio_source_mic_probe_pads("port up");
#if OSYNTH_MIC_LOOPBACK_TEST
    /* Loud, because a build left in this mode reads the synth instead of the
     * room and everything about it otherwise looks like a working microphone. */
    ESP_LOGW(TAG,
             "mic: LOOPBACK TEST %d ACTIVE — din is pin %d (%s), not a "
             "microphone. Set OSYNTH_MIC_LOOPBACK_TEST back to 0.",
             OSYNTH_MIC_LOOPBACK_TEST, (int)OSYNTH_MIC_DIN,
             OSYNTH_MIC_LOOPBACK_TEST == 2 ? "this port's own WS"
                                           : "the DAC's data line");
#endif
    return ESP_OK;
}

bool audio_source_mic_ready(void) { return s_rx != nullptr; }

esp_err_t SYNTH_RENDER_IRAM audio_source_mic_read(int16_t* interleaved,
                                                  size_t frames,
                                                  size_t* frames_read) {
    *frames_read = 0;
    if (s_rx == nullptr) return ESP_ERR_INVALID_STATE;
    /* s_raw is sized for one render block and the caller never asks for more,
     * but the clamp is a byte's worth of instructions against a stack smash
     * from the audio task, which is not a failure anyone would debug twice. */
    if (frames > SYNTH_BLOCK_SIZE) frames = SYNTH_BLOCK_SIZE;

    size_t bytes = 0;
    /* Zero timeout, exactly as the line source: this must never block and
     * never pace. The sink's blocking write stays the only clock in the
     * system, and a short read is the caller's business to zero-fill. */
    const esp_err_t err = i2s_channel_read(
        s_rx, s_raw, frames * 2 * sizeof(mic_word_t), &bytes, 0);
    const size_t got = bytes / (2 * sizeof(mic_word_t));

    /* IRAM, and the conversion loop lives here rather than in audio_io.cpp,
     * because this is where the device's shape is known: the slot width, which
     * slot is real, and whether the other one is a second microphone or the
     * silence a mono part leaves behind. audio_io.cpp gets a stereo int16
     * block from either source and never learns which. */
    uint32_t or_l = 0, or_r = 0;
    for (size_t i = 0; i < got; ++i) {
        /* Both slots, whichever one is being used: which half of the frame a
         * mono converter lands in is a property of its own output routing, and
         * seeing the other one is how that gets settled rather than guessed. */
        or_l |= mag_bits(s_raw[i * 2]);
        or_r |= mag_bits(s_raw[i * 2 + 1]);
#if defined(CONFIG_OSYNTH_MIC_STEREO)
        interleaved[i * 2] = narrow(s_raw[i * 2]);
        interleaved[i * 2 + 1] = narrow(s_raw[i * 2 + 1]);
#else
        /* Duplicated, not panned to one side and not halved: a mono source
         * has to fold to itself. The looper records (L+R)/2 when loop.mono is
         * on, and a mic sitting in one channel would come back 6 dB down in
         * every take while metering correctly — the same trap the ES8388's
         * differential mode set, resolved the same way and in the same place.
         */
        const int16_t m = narrow(s_raw[i * 2 + kSlot]);
        interleaved[i * 2] = m;
        interleaved[i * 2 + 1] = m;
#endif
    }
    s_raw_or[0].fetch_or(or_l, std::memory_order_relaxed);
    s_raw_or[1].fetch_or(or_r, std::memory_order_relaxed);

    *frames_read = got;
    return err;
}

void audio_source_mic_raw_take(uint32_t* or_l, uint32_t* or_r) {
    *or_l = s_raw_or[0].exchange(0, std::memory_order_relaxed);
    *or_r = s_raw_or[1].exchange(0, std::memory_order_relaxed);
}

#endif /* SYNTH_ENABLE_MIC_IN */
