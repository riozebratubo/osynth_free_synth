/*
 * osynth — drum kit loading (Session 22), private to components/drums.
 *
 * Turns a kit image (drum_kit_fmt.h) into a table of ready-to-play samples.
 * Three sources, all producing the same in-memory shape so the voices never
 * learn where a kit came from:
 *
 *   factory   the image linked into the firmware as .rodata. Flash-mapped:
 *             `data` points straight into the XIP window, nothing is copied,
 *             the kit costs 0 bytes of RAM.
 *   .okit     the same image read off an SD card into PSRAM.
 *   WAV dir   a folder of .wav one-shots on an SD card, converted to mono
 *             PCM16 in PSRAM. Slot order follows the sorted file names, and
 *             a leading "NN_" (or "NN-") prefix pins a file to a slot.
 *
 * A kit is only ever published to the audio task once it is fully built and
 * validated (magic, version, CRC, and every slot's extent inside the image);
 * a half-parsed kit must never become a sample pointer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "drum_kit_fmt.h"
#include "drums.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One playable slot.
 *
 * `data` is const because the factory kit lives in read-only flash. Who owns
 * it depends on where the kit came from, and the rule is stated on the kit
 * rather than guessed per slot — see `per_slot_owned` below.
 *
 * The four fields after `format` are the S44 per-pad performance settings.
 * They live here, in the kit, and not in parameter space: a parameter would
 * not follow a kit switch, so `drum7.rev` would mean "reversed" on one kit and
 * silently reverse a completely different sample on the next. Kits are the
 * thing being edited, so the kit is where the edit belongs — and it is what
 * makes copying a pad between kits carry its settings with it. */
typedef struct {
    const uint8_t* data; /* frame 0; nullptr for an empty slot */
    uint32_t frames;
    uint32_t rate;
    uint32_t loop_start;
    uint32_t loop_end; /* 0 = one-shot */
    float gain;
    float pan;
    uint8_t format; /* DRUM_FMT_* */
    uint8_t choke_group;
    uint8_t note;
    /* --- S44 per-pad playback --- */
    uint8_t play_mode;  /* DRUM_PLAY_*                                      */
    uint8_t reverse;    /* play the frames back to front                    */
    float start_ofs;    /* 0..1 into the sample; where a hit begins         */
    /* The block this slot owns on its own, and its size, for the pool
     * accounting in sampler.h. Only meaningful on a kit whose
     * `per_slot_owned` is set; nullptr on a pad that was never recorded. */
    void* owned;
    size_t owned_bytes;
    char name[DRUM_SLOT_NAME_MAX];
} drum_sample_t;

/* The mixer values a kit remembers for its own pads (S44).
 *
 * These mirror the `drumN.level/pan/tune/decay` parameters, which stay the
 * live, automatable, preset-storable layer — the point is not to replace them
 * but to give them somewhere to live *per kit*. With one kit that distinction
 * did not exist; with nine it is the difference between a kit sounding the way
 * you left it and inheriting whatever the last kit was set to. Selecting a kit
 * pushes these into the parameters; a preset loaded afterwards still wins,
 * which is the ordering a player expects when they load a patch on purpose. */
typedef struct {
    float level;
    float pan;
    float tune;
    float decay;
} drum_slot_mix_t;

typedef struct {
    char name[DRUM_KIT_NAME_MAX];
    int slot_count;
    drum_sample_t slots[DRUM_KIT_MAX_SLOTS];
    drum_slot_mix_t mix[DRUM_KIT_MAX_SLOTS];
    /* Whole-image ownership: the single heap block the whole kit was parsed
     * out of, or nullptr for the flash-mapped ROM kit. Mutually exclusive with
     * per_slot_owned. */
    void* owned;
    size_t owned_bytes;
    /* Set on a user kit (S44), where each pad carries its own block because
     * pads are recorded and erased one at a time. drum_kit_free() reads this
     * to know which of the two release paths applies; nothing else has to
     * care, which is the point of stating it once here. */
    bool per_slot_owned;
    /* A user kit that has been changed since it was last written to storage.
     * The app shows it, and the "save on quiet" path uses it to avoid
     * rewriting a card full of kits nobody touched. */
    bool dirty;
} drum_kit_t;

/* Parses `img` in place — the returned kit points into it, so `img` must
 * outlive the kit. `owned` is stored on the kit for drum_kit_free() to
 * release (pass nullptr for flash-resident images). Returns false and leaves
 * `out` untouched on any structural problem; the reason is logged. */
bool drum_kit_parse(const uint8_t* img, size_t len, void* owned,
                    size_t owned_bytes, drum_kit_t* out);

/* The kit built into the firmware. Fails only if the build embedded an empty
 * image (no sample pack was present at build time). */
esp_err_t drum_kit_load_rom(drum_kit_t* out);

void drum_kit_free(drum_kit_t* kit);

/* ---- SD-card kits (PSRAM targets with the SD bus configured) ---- */

/* Mounts the card if it is not already mounted (idempotent, and safe when
 * the looper's SD backend got there first) and lists selectable kits under
 * /sd/osynth/kits: `<name>.okit` files and folders of .wav files. Returns
 * the number written, 0 if there is no card or no kits. */
int drum_kit_scan_sd(char names[][DRUM_KIT_NAME_MAX], int max);

/* Loads a kit previously reported by drum_kit_scan_sd(). Blocking I/O —
 * control tasks only. */
esp_err_t drum_kit_load_sd(const char* name, drum_kit_t* out);

/* True when the build can talk to an SD card at all. */
bool drum_kit_sd_supported(void);

/* ---- user kits (S44) ----------------------------------------------------
 *
 * The eight recordable kits are *fixed slots*, not whatever happens to be on
 * the card: user kit N is the folder `kitN` and nothing else, so "kit 3" means
 * the same kit after a reboot, after a card swap, and on a machine with no
 * card at all. That is what lets the app draw eight stable buttons and what
 * lets `smp.arm` name a pad in a kit that has never been saved.
 *
 * A kit's display name lives in its sidecar rather than in the folder name,
 * for the same reason: renaming a kit must not move it.
 *
 * Storage is chosen once, at init, by drum_kit_storage_init():
 *   SD       /sd/osynth/kits/kitN/  — one WAV per pad plus `kit.oks`.
 *            Preferred whenever a card is present. Also the only backend a
 *            person can load samples into from a computer.
 *   LittleFS /lfs/kits/kitN/        — the same layout on the 1 MB `storage`
 *            partition, shared with the presets. Roughly ten seconds of audio
 *            in total across every kit, so it is a fallback and is reported
 *            as one, not a second first-class option.
 *   none     neither is available; recording still works and is lost at power
 *            off, which is stated in the log and shown in the app rather than
 *            discovered.
 */

/* Picks the backend and creates the kit directory. Call once from drums_init()
 * before any user kit is loaded. Never fails: "no storage" is a valid outcome
 * and the one every classic-ESP32 build gets. */
void drum_kit_storage_init(void);

/* "sd", "lfs" or "none" — what the app shows next to the kit list, and what
 * decides whether the Save button is offered at all. */
const char* drum_kit_storage_name(void);

/* Reads user kit `index` (1..OSYNTH_SAMPLE_KITS) into `out`, allocating each
 * pad through the sampler pool. A kit with no folder yet is not an error: the
 * result is a valid empty kit named "Kit N", which is exactly what a fresh
 * instrument should offer. Blocking I/O — control tasks only. */
esp_err_t drum_kit_load_user(int index, drum_kit_t* out);

/* Writes a user kit back: one `NN_name.wav` per populated pad plus the
 * sidecar. Writes nothing and returns ESP_OK when the kit is not `dirty`.
 *
 * On the LittleFS backend this is a flash write, which stalls the render chain
 * (ESP-IDF disables the cache and parks the other core), so the caller is
 * expected to have waited for audio_io_quiet_ms() — the same rule persist.h
 * documents at length. On SD there is no such hazard. */
esp_err_t drum_kit_save_user(int index, const drum_kit_t* kit);

/* Releases every pad block a user kit owns, back into the pool. */
void drum_kit_free_user(drum_kit_t* kit);

#ifdef __cplusplus
}
#endif
