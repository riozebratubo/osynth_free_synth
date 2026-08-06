/*
 * osynth — silence the ES8388 from the second-stage bootloader.
 *
 * An ES8388 powers up with its output drivers live and its registers in a
 * default state that is not silence, and nothing in the firmware can reach it
 * until I2C exists. codec_early_mute() (codec.h) already does the earliest
 * mute the *application* can manage, at the top of app_main — but app_main is
 * a long way into a cold boot: ROM bootloader, second-stage bootloader, flash
 * mapping, app image load. All of it audible.
 *
 * This is the same two register writes, hundreds of milliseconds earlier, from
 * the bootloader itself. The two compose rather than fight: both write exactly
 * the values codec_init()'s table opens with, so whichever runs, the chip is
 * in the state that table expects.
 *
 * What is *not* fixed here is power-on up to this point — the ROM bootloader
 * runs before any of our code exists. Only hardware can cover that: holding
 * the codec in reset, or gating the amp. See PINMAP.md.
 *
 * Constraints of the environment, which is why this is bit-banged rather than
 * using the I2C driver:
 *   - No drivers. The bootloader links bootloader_support and the ROM, and
 *     nothing else. Registers and esp_rom_* only.
 *   - bootloader_before_init() runs *before BSS is zeroed*, so nothing here
 *     may depend on a zero-initialised static. Everything below is a local or
 *     a compile-time constant.
 *   - No ACK checking, deliberately. Reading SDA back would mean configuring
 *     the pad as an input and dealing with clock stretching, and there is
 *     nothing useful to do with a NACK this early anyway. Both possible ES8388
 *     addresses are written to; the one that is not there simply does not
 *     answer, which costs a few hundred microseconds.
 */
#include "sdkconfig.h"

/* Tells the linker to keep this translation unit. The hooks below override
 * weak symbols in the bootloader, and without a strong symbol that something
 * actually references, the whole archive member is dropped and the hooks
 * silently never run. */
void bootloader_hooks_include(void) {}

#if CONFIG_OSYNTH_FRONTEND_ES8388

#include <stdint.h>

#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#define SDA_PIN CONFIG_OSYNTH_ES8388_I2C_SDA_GPIO
#define SCL_PIN CONFIG_OSYNTH_ES8388_I2C_SCL_GPIO

#if SDA_PIN > 31 || SCL_PIN > 31
/* Pads above 31 live in the GPIO_*1_REG bank, which the single-word writes
 * below do not reach. Nothing has needed it yet; say so rather than driving
 * the wrong pin. */
#error "osynth bootloader mute: ES8388 I2C pins above GPIO31 are not handled"
#endif

#define SDA_MASK (1u << SDA_PIN)
#define SCL_MASK (1u << SCL_PIN)

/* ~100 kHz. Speed is worth nothing here and margin is worth a lot: the pull-up
 * is whatever the board fitted, and this runs once. */
#define HALF_BIT_US 5

/* Open-drain by hand: the output latch is parked at 0 once, so "drive low" is
 * enabling the output and "release" is disabling it and letting the pull-up
 * do the rest. Never drives a line high — two devices pushing opposite ways on
 * an I2C bus is exactly what open-drain exists to prevent. */
static inline void sda_low(void) { REG_WRITE(GPIO_ENABLE_W1TS_REG, SDA_MASK); }
static inline void sda_release(void) {
    REG_WRITE(GPIO_ENABLE_W1TC_REG, SDA_MASK);
}
static inline void scl_low(void) { REG_WRITE(GPIO_ENABLE_W1TS_REG, SCL_MASK); }
static inline void scl_release(void) {
    REG_WRITE(GPIO_ENABLE_W1TC_REG, SCL_MASK);
}
static inline void tick(void) { esp_rom_delay_us(HALF_BIT_US); }

static void i2c_start(void) {
    sda_release();
    scl_release();
    tick();
    sda_low();
    tick();
    scl_low();
    tick();
}

static void i2c_stop(void) {
    sda_low();
    tick();
    scl_release();
    tick();
    sda_release();
    tick();
}

static void i2c_byte(uint8_t v) {
    for (int i = 0; i < 8; ++i) {
        if (v & 0x80u) {
            sda_release();
        } else {
            sda_low();
        }
        v = (uint8_t)(v << 1);
        tick();
        scl_release();
        tick();
        scl_low();
    }
    /* ACK slot: hand SDA back so the device can pull it down, clock it, move
     * on without looking. */
    sda_release();
    tick();
    scl_release();
    tick();
    scl_low();
    tick();
}

static void write_reg(uint8_t addr7, uint8_t reg, uint8_t val) {
    i2c_start();
    i2c_byte((uint8_t)(addr7 << 1)); /* write */
    i2c_byte(reg);
    i2c_byte(val);
    i2c_stop();
}

static void mute_codec(void) {
    esp_rom_gpio_pad_select_gpio(SDA_PIN);
    esp_rom_gpio_pad_select_gpio(SCL_PIN);
    /* The board is supposed to carry 1k pull-ups (PINMAP.md); the internal
     * ones are far weaker and no substitute, but they cost nothing and turn a
     * missing external pull-up into a slow bus rather than a dead one. */
    esp_rom_gpio_pad_pullup_only(SDA_PIN);
    esp_rom_gpio_pad_pullup_only(SCL_PIN);
    /* Park the output latch low, then release both lines. From here on only
     * the enable bits move. */
    REG_WRITE(GPIO_OUT_W1TC_REG, SDA_MASK | SCL_MASK);
    REG_WRITE(GPIO_ENABLE_W1TC_REG, SDA_MASK | SCL_MASK);
    esp_rom_delay_us(50); /* let the pull-ups settle before the first START */

    /* CE selects between them and the firmware probes both, so both get told.
     * 0x19 DACCONTROL3 = 0x04 mutes the DAC; 0x04 DACPOWER = 0xc0 powers the
     * output drivers down. Same values, same order, as codec_early_mute(). */
    for (uint8_t addr = 0x10; addr <= 0x11; ++addr) {
        write_reg(addr, 0x19, 0x04);
        write_reg(addr, 0x04, 0xc0);
    }
}

/* Before the bootloader initialises anything — the earliest point any code of
 * ours runs. GPIO and IO_MUX are in the always-on domain and the ROM has
 * already used them, so they are usable this early even though flash and BSS
 * are not. */
void bootloader_before_init(void) {
    mute_codec();
    esp_rom_printf("osynth: es8388 muted from the bootloader\n");
}

/* Again, a few milliseconds later. Cheap insurance rather than a belief that
 * it is needed: if the pads turn out not to behave that early on some target,
 * this still lands long before app_main, and a second mute on an already-muted
 * codec writes the values it already holds. */
void bootloader_after_init(void) { mute_codec(); }

#endif /* CONFIG_OSYNTH_FRONTEND_ES8388 */
