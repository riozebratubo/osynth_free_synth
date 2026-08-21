#!/usr/bin/env python3
"""S37g - make the pad probe say *why* a pin is not switching.

The S37f cold boot delivered a genuine power-on register file and it exonerates
the driver:

    reset regs 00: 1f 00 00 10 10 00 03 00     <- REG06 = 0x03, REG07 = 0x00
    reset regs 08: ff 00 00 00 20 fc 6a 00     <- REG09/0A = 0x00
    reset regs 10: 13 7c 02 40 10 00 04 00
    reset regs 18: 00 00 00 0c 4c
    reset regs 44: 00 00

REG07's bits 7:6 - the tri-state field - are **zero at reset**, and so are
REG06's bits 7:5. The absolute writes this file used were never clearing
anything the vendor preserves, so S37e's masked writes are correct and change
nothing. Same for REG02/03/04. That whole line of enquiry is closed: the
register table is not the fault, on any reading.

What is left is one ambiguity in the pad probe. It pulls each pad *down* while
sampling, so "STUCK at 0" cannot tell an output actively driving zeros from a
pin nothing drives at all - and on ASDOUT those are completely different
findings. A codec clocked, unmuted and converting silence holds its data pin low
legitimately; a codec whose output stage is off, or a pin that reaches no codec,
floats.

So the probe now does what probe_din() does, once per pad and after the codec is
configured: read under a pull-up, read under a pull-down, and classify.

  toggles          a real signal
  FLOATS           follows both pulls - nothing is driving this pad
  held LOW/HIGH    ignores a pull - something is driving it, but statically

Kept per the project's artifacts rule.
"""
import io
import sys


def edit(path, subs):
    with io.open(path, encoding='utf-8', newline='') as f:
        src = f.read()
    for old, new in subs:
        if new in src:
            print('  (already applied) %r' % old[:50])
            continue
        n = src.count(old)
        if n != 1:
            sys.exit('%s: anchor count %d: %r' % (path, n, old[:70]))
        src = src.replace(old, new)
    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(src)
    print('patched %s' % path)


D = u'—'  # em dash

edit('components/audio_io/source_mic.cpp', [(
    u''' * DIN is in the list too, as a second opinion on the `raw` field in the
 * heartbeat: same reading, arrived at without the I2S receiver in the path. */''',
    u''' * DIN is in the list too, as a second opinion on the `raw` field in the
 * heartbeat: same reading, arrived at without the I2S receiver in the path.
 *
 * Each pad is read twice, under a pull-up and under a pull-down (S37g), because
 * "not switching" is two findings and on a data pin they point opposite ways.
 * A converter that is clocked, unmuted and converting silence holds ASDOUT low
 * on purpose; a converter whose output stage is off {D} or a pad that reaches no
 * converter at all {D} floats. The internal pulls are ~45k, so any real driver
 * wins both readings and a floating pin follows both. Same instrument as
 * probe_din() above, applied to every pad and, crucially, applied again once
 * the codec has been configured. */'''.replace('{D}', D)), (
    u'''    const int kSamples = 4096;
    for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
        const gpio_num_t pin = kProbes[i].pin;
        if ((int)pin < 0) continue;
        if (gpio_input_enable(pin) != ESP_OK) continue;
        /* Pull down while sampling. A pad this chip is driving push-pull does
         * not notice a 45k pull, and a pad nobody drives stops wandering {D}
         * without which an undriven CMOS input can read as "toggles" and turn
         * this instrument into a source of false positives, which is exactly
         * what it exists to stop. */
        (void)gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        int ones = 0;
        for (int n = 0; n < kSamples; ++n) ones += gpio_get_level(pin);
        (void)gpio_set_pull_mode(pin, GPIO_FLOATING);
        if (ones == 0 || ones == kSamples) {
            ESP_LOGW(TAG,
                     "pad probe (%s): %s GPIO%d is STUCK at %d over %d samples "
                     "{D} this pad is not switching",
                     when, kProbes[i].name, (int)pin, ones ? 1 : 0, kSamples);
        } else {
            ESP_LOGI(TAG, "pad probe (%s): %s GPIO%d toggles (high %d%% of %d)",
                     when, kProbes[i].name, (int)pin,
                     ones * 100 / kSamples, kSamples);
        }
    }
}
'''.replace('{D}', D),
    u'''    const int kSamples = 4096;
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
                     "pad probe (%s): %s GPIO%d FLOATS {D} it follows both pulls, "
                     "so nothing is driving this pad",
                     when, kProbes[i].name, (int)pin);
        } else {
            ESP_LOGW(TAG,
                     "pad probe (%s): %s GPIO%d is held %s {D} something drives "
                     "it, but statically",
                     when, kProbes[i].name, (int)pin, dn ? "HIGH" : "LOW");
        }
    }
}
'''.replace('{D}', D))])

print('done')
