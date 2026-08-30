/*
 * osynth — drum kit loading (Session 22). See drum_kit.h for the contract.
 */
#include "drum_kit.h"

#include <cstdio>
#include <cstring>
#include <strings.h> /* strcasecmp */

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"

#include "sampler.h"
#include "synth_config.h"

static const char* TAG = "drumkit";

/* The factory image, embedded by CMake (target_add_binary_data). The symbol
 * names are derived from the file name — keep them in sync with the
 * add_custom_command output in CMakeLists.txt.
 *
 * The asm() label is how GCC and Clang reach a linker symbol whose name is not
 * a valid C identifier, which is what the objcopy-style `_binary_<file>_start`
 * is. MSVC has no equivalent, so the host build generates a C file defining
 * these two as ordinary identifiers instead (tools/bin2c.py, driven from
 * port/host/CMakeLists.txt) and declares them plainly here. Same bytes, same
 * names in this translation unit; only the route to the linker differs. */
#if defined(_MSC_VER)
/* Pointers rather than arrays, because the generated file cannot portably
 * place a second array symbol exactly one past the end of the first. The two
 * uses below -- the pointer difference on line 146 and passing _start as a
 * const uint8_t* on 147 -- read identically either way. */
extern "C" const uint8_t* const drumkit_bin_start;
extern "C" const uint8_t* const drumkit_bin_end;
#else
extern "C" const uint8_t drumkit_bin_start[] asm("_binary_drumkit_bin_start");
extern "C" const uint8_t drumkit_bin_end[] asm("_binary_drumkit_bin_end");
#endif

namespace {

/* Validates one slot descriptor against the image bounds. A kit is data from
 * an SD card as often as it is from our own build, so every offset is
 * checked before it can become a pointer the audio task dereferences. */
bool slot_ok(const drum_kit_slot_t& s, size_t len) {
    if (s.frames == 0) return true; /* deliberately empty slot */
    if (s.data_offset < sizeof(drum_kit_header_t)) return false;
    if (s.format != DRUM_FMT_ULAW && s.format != DRUM_FMT_PCM16) return false;
    const uint32_t bytes = s.frames * (s.format == DRUM_FMT_PCM16 ? 2u : 1u);
    if (bytes / (s.format == DRUM_FMT_PCM16 ? 2u : 1u) != s.frames) {
        return false; /* frames * width overflowed */
    }
    if (s.data_offset > len || bytes > len - s.data_offset) return false;
    if (s.rate < 4000 || s.rate > 96000) return false;
    if (s.loop_end != 0 &&
        (s.loop_end > s.frames || s.loop_start >= s.loop_end)) {
        return false;
    }
    /* PCM16 is read as int16 — a misaligned offset would fault on Xtensa. */
    if (s.format == DRUM_FMT_PCM16 && (s.data_offset & 1u) != 0) return false;
    return true;
}

void copy_name(char* dst, size_t dst_len, const char* src, size_t src_len) {
    const size_t n = src_len < dst_len - 1 ? src_len : dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0'; /* the field is NUL-padded, not NUL-terminated */
}

} // namespace

bool drum_kit_parse(const uint8_t* img, size_t len, void* owned,
                    size_t owned_bytes, drum_kit_t* out) {
    if (img == nullptr || out == nullptr) return false;
    if (len < sizeof(drum_kit_header_t)) {
        ESP_LOGW(TAG, "image too small (%u B)", (unsigned)len);
        return false;
    }
    drum_kit_header_t hdr;
    memcpy(&hdr, img, sizeof(hdr)); /* the image may be unaligned in flash */
    if (memcmp(hdr.magic, DRUM_KIT_MAGIC, DRUM_KIT_MAGIC_LEN) != 0) {
        ESP_LOGW(TAG, "bad magic — not a kit image");
        return false;
    }
    if (hdr.version != DRUM_KIT_VERSION) {
        ESP_LOGW(TAG, "kit version %u, firmware expects %u", hdr.version,
                 DRUM_KIT_VERSION);
        return false;
    }
    if (hdr.slot_count > DRUM_KIT_MAX_SLOTS) {
        ESP_LOGW(TAG, "kit declares %u slots, max %d", hdr.slot_count,
                 DRUM_KIT_MAX_SLOTS);
        return false;
    }
    if (hdr.total_bytes > len) {
        ESP_LOGW(TAG, "header says %u B, have %u B", (unsigned)hdr.total_bytes,
                 (unsigned)len);
        return false;
    }
    len = hdr.total_bytes; /* trailing padding (SD sector fill) is not ours */
    const size_t table_bytes = (size_t)hdr.slot_count * sizeof(drum_kit_slot_t);
    if (sizeof(drum_kit_header_t) + table_bytes > len) {
        ESP_LOGW(TAG, "slot table runs past the end of the image");
        return false;
    }

    /* CRC covers everything after the header. zlib.crc32 in the generator and
     * esp_rom_crc32_le are the same polynomial and the same convention. */
    const uint32_t crc = esp_rom_crc32_le(
        0, img + sizeof(drum_kit_header_t), len - sizeof(drum_kit_header_t));
    if (crc != hdr.crc32) {
        ESP_LOGW(TAG, "CRC mismatch (header %08x, computed %08x)",
                 (unsigned)hdr.crc32, (unsigned)crc);
        return false;
    }

    drum_kit_t kit = {};
    copy_name(kit.name, sizeof(kit.name), hdr.name, sizeof(hdr.name));
    kit.owned = owned;
    kit.owned_bytes = owned_bytes;

    for (int i = 0; i < hdr.slot_count; ++i) {
        drum_kit_slot_t s;
        memcpy(&s, img + sizeof(drum_kit_header_t) + (size_t)i * sizeof(s),
               sizeof(s));
        if (!slot_ok(s, len)) {
            ESP_LOGW(TAG, "slot %d is out of bounds — kit rejected", i);
            return false;
        }
        drum_sample_t& d = kit.slots[i];
        d.data = s.frames ? img + s.data_offset : nullptr;
        d.frames = s.frames;
        d.rate = s.rate;
        d.loop_start = s.loop_start;
        d.loop_end = s.loop_end;
        d.gain = (s.gain > 0.0f && s.gain <= 8.0f) ? s.gain : 1.0f;
        d.pan = (s.pan >= -1.0f && s.pan <= 1.0f) ? s.pan : 0.0f;
        d.format = s.format;
        d.choke_group = s.choke_group;
        d.note = s.note & 0x7F;
        copy_name(d.name, sizeof(d.name), s.name, sizeof(s.name));
    }
    kit.slot_count = hdr.slot_count;
    *out = kit;
    return true;
}

esp_err_t drum_kit_load_rom(drum_kit_t* out) {
    const size_t len = (size_t)(drumkit_bin_end - drumkit_bin_start);
    if (!drum_kit_parse(drumkit_bin_start, len, nullptr, 0, out)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out->slot_count == 0) {
        ESP_LOGW(TAG,
                 "factory kit is empty — no sample pack was present at build "
                 "time (see tools/gen_drumkit.py)");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "factory kit '%s': %d slots, %u KB in flash (.rodata)",
             out->name, out->slot_count, (unsigned)(len / 1024));
    return ESP_OK;
}

void drum_kit_free(drum_kit_t* kit) {
    if (kit == nullptr) return;
    /* Two ownership shapes, and the kit says which one applies — see
     * per_slot_owned in drum_kit.h. Getting this wrong in either direction is
     * a leak or a double free, which is exactly why it is a field on the kit
     * and not something inferred from where the kit came from. */
    if (kit->per_slot_owned) {
        drum_kit_free_user(kit);
        return;
    }
    if (kit->owned != nullptr) heap_caps_free(kit->owned);
    memset(kit, 0, sizeof(*kit));
}

void drum_kit_free_user(drum_kit_t* kit) {
    if (kit == nullptr) return;
    for (int i = 0; i < DRUM_KIT_MAX_SLOTS; ++i) {
        drum_sample_t& s = kit->slots[i];
        if (s.owned != nullptr) sampler_pool_free(s.owned, s.owned_bytes);
        s.owned = nullptr;
        s.owned_bytes = 0;
        s.data = nullptr;
        s.frames = 0;
    }
}

/* ===================== SD-card and user kits =========================== */

/* The mount machinery below is shared by two features that arrived four
 * sessions apart: loading a prepared kit off a card (S22) and persisting the
 * recordable kits (S44). Either one being enabled is enough to need it, which
 * is why the guard is wider than the option it used to carry. */
#if CONFIG_OSYNTH_DRUM_SD_KITS || SYNTH_SAMPLE_KITS > 0

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(SYNTH_TARGET_HOST)
/* Everything below the mount is plain stdio -- opendir, fopen, mkdir -- and a
 * host has all of it. What it has no equivalent for is the bring-up: an SPI
 * bus, an SDSPI device, a FAT mount and a power rail. So those are replaced
 * and the rest is used unchanged, exactly as the looper's store does it. */
#include "host_paths.h"
#define OSYNTH_SD_LDO 0
#else
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#if defined(CONFIG_OSYNTH_SD_PWR_LDO_CHAN) && CONFIG_OSYNTH_SD_PWR_LDO_CHAN > 0
#include "esp_ldo_regulator.h"
#define OSYNTH_SD_LDO 1
#else
#define OSYNTH_SD_LDO 0
#endif
#endif /* SYNTH_TARGET_HOST */

namespace {

#if defined(SYNTH_TARGET_HOST)
/* Resolved once in drum_kit_storage_init(). kMount has no meaning without a
 * filesystem to mount, so only kKitDir is read. */
char s_kit_dir[512];
const char* kKitDir = s_kit_dir;
#else
constexpr const char* kMount = "/sd";
constexpr const char* kKitDir = "/sd/osynth/kits";
#endif

/* Bytes for a kit path. "/sd/osynth/kits/" plus a 120-character name fits the
 * 192 these buffers had; a host data directory is a full user-profile path and
 * does not. Every path here is built with snprintf, so the failure would be a
 * silently truncated name -- a kit written to, or looked for, in the wrong
 * place. The looper's store carries the same constant for the same reason. */
#if defined(SYNTH_TARGET_HOST)
constexpr size_t kKitPathMax = 768;
#else
constexpr size_t kKitPathMax = 192;
#endif
/* PSRAM ceiling for one loaded kit. Generous for mu-law .okit images (the
 * factory kit is ~240 KB) and enough for a folder of WAVs, while leaving the
 * looper the bulk of the pool. */
constexpr size_t kMaxKitBytes = 2 * 1024 * 1024;

#if !defined(SYNTH_TARGET_HOST)
sdmmc_card_t* s_card = nullptr;
#endif

#if defined(SYNTH_TARGET_HOST)

/* No rail, no bus, no card: "mounted" is "the directory exists", and
 * drum_kit_storage_init() below is what creates it. Kept as a function so the
 * call sites read the same on both backends. */
bool ensure_mounted() { return s_kit_dir[0] != '\0'; }

#else

/* The SD rail's on-chip LDO — the looper's copy of this in loop_store.cpp
 * carries the reasoning. Duplicated rather than shared because these two
 * components bring the bus up independently and neither may depend on the
 * other (looper already requires drums); the regulator refcounts a
 * non-adjustable channel, so whichever gets here first turns the rail on and
 * the second one simply joins. */
void sd_power_up() {
#if OSYNTH_SD_LDO
    static esp_ldo_channel_handle_t s_ldo = nullptr;
    if (s_ldo != nullptr) return;
    esp_ldo_channel_config_t cfg = {};
    cfg.chan_id = CONFIG_OSYNTH_SD_PWR_LDO_CHAN;
    cfg.voltage_mv = CONFIG_OSYNTH_SD_PWR_LDO_MV;
    const esp_err_t err = esp_ldo_acquire_channel(&cfg, &s_ldo);
    if (err != ESP_OK) {
        s_ldo = nullptr;
        ESP_LOGE(TAG, "SD rail LDO%d refused (%s) — kits will not load",
                 CONFIG_OSYNTH_SD_PWR_LDO_CHAN, esp_err_to_name(err));
    }
#endif
}

/* Idempotent bring-up. The looper's S16 SD backend may already own the bus
 * and the mount point; both are shared, so "already there" is success, not
 * an error. */
bool ensure_mounted() {
    struct stat st;
    if (stat(kMount, &st) == 0) return true; /* someone mounted it already */

    sd_power_up();
    spi_bus_config_t bus = {};
    bus.mosi_io_num = CONFIG_OSYNTH_SD_MOSI_GPIO;
    bus.miso_io_num = CONFIG_OSYNTH_SD_MISO_GPIO;
    bus.sclk_io_num = CONFIG_OSYNTH_SD_SCK_GPIO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* already up = fine */
        ESP_LOGW(TAG, "SPI bus init failed (%s)", esp_err_to_name(err));
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = (gpio_num_t)CONFIG_OSYNTH_SD_CS_GPIO;
    dev.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    cfg.format_if_mount_failed = false;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 16 * 1024;
    err = esp_vfs_fat_sdspi_mount(kMount, &host, &dev, &cfg, &s_card);
    if (err != ESP_OK) {
        s_card = nullptr;
        ESP_LOGW(TAG, "SD mount failed (%s) — card inserted?",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

#endif /* SYNTH_TARGET_HOST */

bool has_ext(const char* name, const char* ext) {
    const size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

/* ---- minimal WAV reader for folder kits ----
 * Chunk-walking rather than "assume the data chunk is at offset 44": real
 * files carry LIST/fact/cue chunks in front of it. Converts to mono int16 at
 * the file's own rate; resampling happens at playback time anyway. */
struct WavInfo {
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
    uint16_t fmt_tag;
    long data_pos;
    uint32_t data_bytes;
};

bool wav_open(FILE* f, WavInfo* w) {
    char riff[12];
    if (fread(riff, 1, 12, f) != 12) return false;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool have_fmt = false;
    bool have_data = false;
    /* Every chunk must leave the file further along than it started, and the
     * walk stops the moment one does not. `sz` comes straight off an SD card
     * and is not a length we get to trust: as a long it can be negative, and
     * a chunk declaring 0xFFFFFFF8 seeks back exactly the 8 bytes the header
     * read advanced, so the loop sat on one offset forever — fread kept
     * succeeding on the same bytes and drum_ctl (prio 4, core 0) span without
     * ever yielding, starving ble_cmd and persist underneath it. */
    long pos = ftell(f);
    for (;;) {
        char id[4];
        uint32_t sz;
        if (fread(id, 1, 4, f) != 4) break;
        if (fread(&sz, 4, 1, f) != 1) break;
        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) return false;
            memcpy(&w->fmt_tag, fmt + 0, 2);
            memcpy(&w->channels, fmt + 2, 2);
            memcpy(&w->rate, fmt + 4, 4);
            memcpy(&w->bits, fmt + 14, 2);
            if (sz > 16) fseek(f, (long)(sz - 16), SEEK_CUR);
            have_fmt = true;
            if (have_data) return true; /* `data` came first — legal, and the
                                         * old code walked to EOF and failed */
        } else {
            if (memcmp(id, "data", 4) == 0 && !have_data) {
                w->data_pos = ftell(f);
                w->data_bytes = sz;
                have_data = true;
                if (have_fmt) return true; /* the usual ordering */
            }
            fseek(f, (long)sz, SEEK_CUR);
        }
        if (sz & 1) fseek(f, 1, SEEK_CUR); /* chunks are word-aligned */
        const long next = ftell(f);
        if (next <= pos) return false; /* malformed size: the walk cannot end */
        pos = next;
    }
    return false;
}

/* Reads a WAV into `dst` as mono int16. Returns frames written, 0 on error. */
uint32_t wav_read_mono(FILE* f, const WavInfo& w, int16_t* dst,
                       uint32_t max_frames) {
    const int ch = w.channels ? w.channels : 1;
    const int bytes_per_sample = w.bits / 8;
    if (bytes_per_sample <= 0 || bytes_per_sample > 4) return 0;
    const int frame_bytes = bytes_per_sample * ch;
    uint32_t frames = w.data_bytes / (uint32_t)frame_bytes;
    if (frames > max_frames) frames = max_frames;
    if (fseek(f, w.data_pos, SEEK_SET) != 0) return 0;

    /* One sector-ish staging buffer, converted frame by frame. */
    /* Staged on the caller's stack, so keep it modest: drum_ctl has 8 KB and
     * the directory listing above already claims a couple of them.
     *
     * The frames-per-pass count is derived from the buffer rather than fixed.
     * `bits` is bounded above (<= 32) but `channels` comes straight off the
     * file and is not: a 5.1 stem carries 18-byte frames, and a fixed 128
     * frames per pass then asked fread() for 2304 bytes of a 1 KB buffer. */
    constexpr size_t kStageBytes = 1024;
    uint8_t buf[kStageBytes];
    const uint32_t chunk_frames = (uint32_t)(kStageBytes / (size_t)frame_bytes);
    if (chunk_frames == 0) return 0; /* > 1 KB per frame: not a sane WAV */
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > chunk_frames) n = chunk_frames;
        const size_t want = (size_t)n * (size_t)frame_bytes;
        if (fread(buf, 1, want, f) != want) break;
        for (uint32_t i = 0; i < n; ++i) {
            float acc = 0.0f;
            for (int c = 0; c < ch; ++c) {
                const uint8_t* p = buf + (size_t)i * frame_bytes +
                                   (size_t)c * bytes_per_sample;
                float v = 0.0f;
                if (w.fmt_tag == 3 && w.bits == 32) {
                    float fv;
                    memcpy(&fv, p, 4);
                    v = fv;
                } else if (w.bits == 16) {
                    int16_t s;
                    memcpy(&s, p, 2);
                    v = (float)s / 32768.0f;
                } else if (w.bits == 24) {
                    int32_t s = (int32_t)p[0] | ((int32_t)p[1] << 8) |
                                ((int32_t)p[2] << 16);
                    if (s & 0x800000) s -= 0x1000000;
                    v = (float)s / 8388608.0f;
                } else if (w.bits == 8) {
                    v = ((float)p[0] - 128.0f) / 128.0f;
                } else if (w.bits == 32) {
                    int32_t s;
                    memcpy(&s, p, 4);
                    v = (float)s / 2147483648.0f;
                }
                acc += v;
            }
            acc /= (float)ch;
            /* Written as a rejection rather than two clamps because of the
             * one input this decoder does not control: a 32-bit float WAV
             * (fmt_tag 3) may carry a NaN, which passes `> 0.999f` and
             * `< -0.999f` both, and converting one is undefined — a wild
             * int16 written into a pad from a file off the SD card. This form
             * sends it to silence and clamps everything else exactly as
             * before. */
            if (!(acc > -0.999f && acc < 0.999f)) {
                acc = (acc > 0.0f) ? 0.999f : (acc < 0.0f ? -0.999f : 0.0f);
            }
            dst[done + i] = (int16_t)(acc * 32767.0f);
        }
        done += n;
    }
    return done;
}

/* "03_snare.wav" / "3-snare.wav" -> slot 3. Returns -1 when unprefixed. */
int slot_prefix(const char* fname) {
    if (fname[0] < '0' || fname[0] > '9') return -1;
    int v = 0, i = 0;
    while (fname[i] >= '0' && fname[i] <= '9' && i < 3) {
        v = v * 10 + (fname[i] - '0');
        ++i;
    }
    if (fname[i] != '_' && fname[i] != '-') return -1;
    return v < DRUM_KIT_MAX_SLOTS ? v : -1;
}

void base_name(const char* fname, char* out, size_t out_len) {
    const char* p = fname;
    const int pref = slot_prefix(fname);
    if (pref >= 0) {
        while (*p && *p != '_' && *p != '-') ++p;
        if (*p) ++p;
    }
    size_t n = 0;
    while (p[n] && p[n] != '.' && n < out_len - 1) {
        out[n] = p[n];
        ++n;
    }
    out[n] = '\0';
}

esp_err_t load_wav_dir(const char* dir, const char* kit_name,
                       drum_kit_t* out) {
    DIR* d = opendir(dir);
    if (d == nullptr) return ESP_ERR_NOT_FOUND;

    /* Pass 1: collect names so slots follow sorted order, not FAT order. */
    char files[DRUM_KIT_MAX_SLOTS][64];
    int explicit_slot[DRUM_KIT_MAX_SLOTS];
    int count = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && count < DRUM_KIT_MAX_SLOTS) {
        if (!has_ext(e->d_name, ".wav")) continue;
        /* strlcpy, not strncpy: it always terminates, so there is nothing for
         * -Wstringop-truncation to complain about (and it matches the rest of
         * the codebase). */
        strlcpy(files[count], e->d_name, sizeof(files[0]));
        explicit_slot[count] = slot_prefix(e->d_name);
        ++count;
    }
    closedir(d);
    if (count == 0) return ESP_ERR_NOT_FOUND;
    for (int i = 1; i < count; ++i) { /* insertion sort, <= 32 entries */
        char tmp[64];
        strcpy(tmp, files[i]);
        const int ts = explicit_slot[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(files[j], tmp) > 0) {
            strcpy(files[j + 1], files[j]);
            explicit_slot[j + 1] = explicit_slot[j];
            --j;
        }
        strcpy(files[j + 1], tmp);
        explicit_slot[j + 1] = ts;
    }

    /* Pass 2: size the PSRAM block, then read the files into it. */
    drum_kit_t kit = {};
    strlcpy(kit.name, kit_name, sizeof(kit.name));

    uint8_t* pool = (uint8_t*)heap_caps_malloc(kMaxKitBytes, MALLOC_CAP_SPIRAM);
    if (pool == nullptr) {
        ESP_LOGW(TAG, "no PSRAM for a %u KB kit pool",
                 (unsigned)(kMaxKitBytes / 1024));
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    int next_free = 0;
    for (int i = 0; i < count; ++i) {
        int slot = explicit_slot[i];
        if (slot < 0) {
            while (next_free < DRUM_KIT_MAX_SLOTS &&
                   kit.slots[next_free].frames != 0) {
                ++next_free;
            }
            slot = next_free;
        }
        if (slot >= DRUM_KIT_MAX_SLOTS || kit.slots[slot].frames != 0) continue;

        /* Precision-bounded %s throughout the path builds below: a FAT
         * directory entry is char[256], so an unbounded "%s/%s" cannot be
         * proven to fit and -Wformat-truncation rejects it — and a long
         * filename really would truncate. */
        char path[kKitPathMax];
        snprintf(path, sizeof(path), "%.100s/%.63s", dir, files[i]);
        FILE* f = fopen(path, "rb");
        if (f == nullptr) continue;
        WavInfo w = {};
        if (!wav_open(f, &w)) {
            ESP_LOGW(TAG, "%s: unreadable WAV", files[i]);
            fclose(f);
            continue;
        }
        /* int16 must land 2-byte aligned inside the pool. */
        used = (used + 1u) & ~(size_t)1u;
        const uint32_t room = (uint32_t)((kMaxKitBytes - used) / 2);
        const uint32_t frames =
            wav_read_mono(f, w, (int16_t*)(pool + used), room);
        fclose(f);
        if (frames == 0) continue;

        drum_sample_t& s = kit.slots[slot];
        s.data = pool + used;
        s.frames = frames;
        s.rate = w.rate ? w.rate : 44100;
        s.gain = 1.0f;
        s.pan = 0.0f;
        s.format = DRUM_FMT_PCM16;
        s.choke_group = 0;
        /* Nothing on an SD card tells us the GM note; lay the folder out
         * chromatically from the GM kick so every slot is at least
         * reachable from a keyboard. */
        s.note = (uint8_t)(36 + slot);
        base_name(files[i], s.name, sizeof(s.name));
        used += (size_t)frames * 2;
        if (slot + 1 > kit.slot_count) kit.slot_count = slot + 1;
        if (used + 4096 >= kMaxKitBytes) {
            ESP_LOGW(TAG, "kit pool full after %d slots", kit.slot_count);
            break;
        }
    }

    if (kit.slot_count == 0) {
        heap_caps_free(pool);
        return ESP_ERR_NOT_FOUND;
    }
    /* Shrink to what was actually used — a 2 MB reservation for a 300 KB kit
     * would starve the looper. */
    void* shrunk = heap_caps_realloc(pool, used, MALLOC_CAP_SPIRAM);
    if (shrunk != nullptr && shrunk != pool) {
        const ptrdiff_t delta = (const uint8_t*)shrunk - pool;
        for (int i = 0; i < kit.slot_count; ++i) {
            if (kit.slots[i].data) kit.slots[i].data += delta;
        }
        pool = (uint8_t*)shrunk;
    }
    kit.owned = pool;
    kit.owned_bytes = used;
    *out = kit;
    ESP_LOGI(TAG, "SD kit '%s': %d slots, %u KB PSRAM (WAV folder)", kit.name,
             kit.slot_count, (unsigned)(used / 1024));
    return ESP_OK;
}

} // namespace

bool drum_kit_sd_supported(void) { return true; }

int drum_kit_scan_sd(char names[][DRUM_KIT_NAME_MAX], int max) {
    if (!ensure_mounted()) return 0;
    DIR* d = opendir(kKitDir);
    if (d == nullptr) {
        ESP_LOGI(TAG, "no %s directory — SD kits unavailable", kKitDir);
        return 0;
    }
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && n < max) {
        if (e->d_name[0] == '.') continue;
        char path[kKitPathMax];
        snprintf(path, sizeof(path), "%s/%.120s", kKitDir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            strlcpy(names[n], e->d_name, DRUM_KIT_NAME_MAX);
            ++n;
        } else if (has_ext(e->d_name, ".okit")) {
            /* Store without the extension; load_sd re-adds it. */
            size_t len = strlen(e->d_name) - 5;
            if (len > DRUM_KIT_NAME_MAX - 1) len = DRUM_KIT_NAME_MAX - 1;
            memcpy(names[n], e->d_name, len);
            names[n][len] = '\0';
            ++n;
        }
    }
    closedir(d);
    return n;
}

esp_err_t drum_kit_load_sd(const char* name, drum_kit_t* out) {
    if (name == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
    if (!ensure_mounted()) return ESP_ERR_NOT_FOUND;

    char path[kKitPathMax];
    snprintf(path, sizeof(path), "%s/%.100s.okit", kKitDir, name);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) { /* not an image — try a folder of WAVs */
        snprintf(path, sizeof(path), "%s/%.100s", kKitDir, name);
        return load_wav_dir(path, name, out);
    }

    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (size_t)len > kMaxKitBytes) {
        ESP_LOGW(TAG, "%s: implausible size (%ld B)", path, len);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
        fclose(f);
        ESP_LOGW(TAG, "no PSRAM for a %ld B kit", len);
        return ESP_ERR_NO_MEM;
    }
    const size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        heap_caps_free(buf);
        return ESP_ERR_INVALID_STATE;
    }
    if (!drum_kit_parse(buf, (size_t)len, buf, (size_t)len, out)) {
        heap_caps_free(buf);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "SD kit '%s': %d slots, %ld KB PSRAM (.okit)", out->name,
             out->slot_count, len / 1024);
    return ESP_OK;
}

/* ===================== user kits (S44) ================================= *
 *
 * On disk a user kit is a folder of ordinary WAVs plus one small binary
 * sidecar, and that shape was chosen over a single `.okit` image for one
 * reason: a person with a card reader can open the folder, hear what is in it,
 * drop a file in and take one out. An image would have made the firmware the
 * only thing that could read a kit the player recorded, which is the wrong
 * trade for the one feature whose whole point is that the content is theirs.
 *
 * The sidecar carries what a WAV cannot: play mode, reverse, start offset,
 * choke group, note assignment and the kit's stored mixer. Its absence is not
 * an error — a folder somebody filled by hand still loads, with everything
 * defaulted, which is exactly what "drop your own samples in" has to mean.
 */

namespace {

constexpr char kSidecarMagic[4] = {'O', 'S', 'K', 'S'};
constexpr uint16_t kSidecarVersion = 1;
constexpr char kSidecarName[] = "kit.oks";

/* Per-pad ceiling when reading from storage. Matches the recorder's own take
 * ceiling so that what can be recorded and what can be loaded are the same
 * length — a card file longer than this is truncated with a warning rather
 * than refused, because half a loop in the right pad beats an empty pad. */
constexpr uint32_t kMaxPadFrames =
    (uint32_t)SYNTH_SAMPLE_MAX_SEC * SYNTH_SAMPLE_RATE;

/* Which backend user kits live on. Decided once by drum_kit_storage_init(). */
enum : int { STORE_NONE = 0, STORE_SD = 1, STORE_LFS = 2 };
int s_store = STORE_NONE;

constexpr const char* kLfsKitDir = "/lfs/kits";

#pragma pack(push, 1)
typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t slot_count;
    char name[DRUM_KIT_NAME_MAX];
    uint32_t reserved[2];
} kit_sidecar_hdr_t;

typedef struct {
    uint8_t play_mode;
    uint8_t reverse;
    uint8_t choke_group;
    uint8_t note;
    float start_ofs;
    float gain;
    float pan;
    uint32_t loop_start;
    uint32_t loop_end;
    /* The kit's copy of the drumN.* mixer — see drum_slot_mix_t. */
    float mix_level;
    float mix_pan;
    float mix_tune;
    float mix_decay;
    char name[DRUM_SLOT_NAME_MAX];
} kit_sidecar_slot_t;
#pragma pack(pop)

void kit_dir(int index, char* out, size_t n) {
    snprintf(out, n, "%s/kit%d",
             s_store == STORE_SD ? kKitDir : kLfsKitDir, index);
}

/* Turns a slot name into something a FAT volume will accept. */
void safe_name(const char* in, char* out, size_t n) {
    size_t w = 0;
    for (size_t i = 0; in[i] != '\0' && w + 1 < n; ++i) {
        const char c = in[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-';
        out[w++] = ok ? c : '_';
    }
    if (w == 0) out[w++] = 'p';
    out[w] = '\0';
}

bool write_wav(const char* path, const int16_t* data, uint32_t frames) {
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return false;
    const uint32_t data_bytes = frames * 2u;
    const uint32_t rate = SYNTH_SAMPLE_RATE;
    uint8_t h[44];
    memcpy(h + 0, "RIFF", 4);
    const uint32_t riff = 36u + data_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16;
    memcpy(h + 16, &fmt_len, 4);
    const uint16_t tag = 1, ch = 1, bits = 16, align = 2;
    memcpy(h + 20, &tag, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    const uint32_t byte_rate = rate * 2u;
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
    /* Chunked so a long pad does not need a second copy of itself anywhere,
     * and so a card that stalls fails on a boundary rather than mid-header. */
    const uint32_t kChunk = 4096;
    for (uint32_t done = 0; ok && done < frames;) {
        const uint32_t n = (frames - done > kChunk) ? kChunk : frames - done;
        ok = fwrite(data + done, 2, n, f) == n;
        done += n;
    }
    fclose(f);
    return ok;
}

bool read_sidecar(int index, drum_kit_t* kit) {
    char dir[kKitPathMax];
    char path[kKitPathMax];
    kit_dir(index, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%.120s/%.16s", dir, kSidecarName);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    kit_sidecar_hdr_t h;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) &&
              memcmp(h.magic, kSidecarMagic, 4) == 0 &&
              h.version == kSidecarVersion && h.slot_count <= DRUM_SLOTS;
    if (ok) {
        char nm[DRUM_KIT_NAME_MAX];
        memcpy(nm, h.name, DRUM_KIT_NAME_MAX);
        nm[DRUM_KIT_NAME_MAX - 1] = '\0';
        if (nm[0] != '\0') strlcpy(kit->name, nm, DRUM_KIT_NAME_MAX);
        for (int i = 0; i < h.slot_count; ++i) {
            kit_sidecar_slot_t s;
            if (fread(&s, 1, sizeof(s), f) != sizeof(s)) break;
            drum_sample_t& d = kit->slots[i];
            d.play_mode = s.play_mode <= DRUM_PLAY_LOOP ? s.play_mode
                                                        : DRUM_PLAY_ONESHOT;
            d.reverse = s.reverse ? 1u : 0u;
            d.choke_group = s.choke_group <= 7 ? s.choke_group : 0u;
            d.note = s.note & 0x7F;
            d.start_ofs =
                (s.start_ofs >= 0.0f && s.start_ofs < 1.0f) ? s.start_ofs : 0.0f;
            d.gain = (s.gain > 0.0f && s.gain <= 8.0f) ? s.gain : 1.0f;
            d.pan = (s.pan >= -1.0f && s.pan <= 1.0f) ? s.pan : 0.0f;
            d.loop_start = s.loop_start;
            d.loop_end = s.loop_end;
            kit->mix[i].level = (s.mix_level >= 0.0f && s.mix_level <= 2.0f)
                                    ? s.mix_level : 1.0f;
            kit->mix[i].pan = (s.mix_pan >= -1.0f && s.mix_pan <= 1.0f)
                                  ? s.mix_pan : 0.0f;
            kit->mix[i].tune = (s.mix_tune >= -24.0f && s.mix_tune <= 24.0f)
                                   ? s.mix_tune : 0.0f;
            kit->mix[i].decay = (s.mix_decay >= 0.0f && s.mix_decay <= 1.0f)
                                    ? s.mix_decay : 1.0f;
        }
    }
    fclose(f);
    return ok;
}

bool write_sidecar(int index, const drum_kit_t* kit) {
    char dir[kKitPathMax];
    char path[kKitPathMax];
    kit_dir(index, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%.120s/%.16s", dir, kSidecarName);
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return false;
    kit_sidecar_hdr_t h = {};
    memcpy(h.magic, kSidecarMagic, 4);
    h.version = kSidecarVersion;
    h.slot_count = (uint16_t)DRUM_SLOTS;
    strlcpy(h.name, kit->name, DRUM_KIT_NAME_MAX);
    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h);
    for (int i = 0; ok && i < DRUM_SLOTS; ++i) {
        const drum_sample_t& d = kit->slots[i];
        kit_sidecar_slot_t s = {};
        s.play_mode = d.play_mode;
        s.reverse = d.reverse;
        s.choke_group = d.choke_group;
        s.note = d.note;
        s.start_ofs = d.start_ofs;
        s.gain = d.gain > 0.0f ? d.gain : 1.0f;
        s.pan = d.pan;
        s.loop_start = d.loop_start;
        s.loop_end = d.loop_end;
        s.mix_level = kit->mix[i].level;
        s.mix_pan = kit->mix[i].pan;
        s.mix_tune = kit->mix[i].tune;
        s.mix_decay = kit->mix[i].decay;
        memcpy(s.name, d.name, DRUM_SLOT_NAME_MAX);
        ok = fwrite(&s, 1, sizeof(s), f) == sizeof(s);
    }
    fclose(f);
    return ok;
}

} // namespace

void drum_kit_storage_init(void) {
    if (SYNTH_SAMPLE_KITS == 0) return;

#if defined(SYNTH_TARGET_HOST)
    /* One place to put them, and it is neither an SD card nor a flash
     * partition -- so neither of the two backends below applies, and neither
     * does the choice between them. STORE_SD is the label because it is the
     * one that means "a real filesystem with room": the app reports it as the
     * storage name, and everything it implies here is true -- kits are
     * plentiful, saving does not stall the render chain, and a person can put
     * their own samples in the folder from the machine they are sitting at. */
    if (osynth_host_subdir("kits", s_kit_dir, sizeof(s_kit_dir))) {
        s_store = STORE_SD;
        ESP_LOGI(TAG, "user kits: %s", kKitDir);
    } else {
        s_store = STORE_NONE;
        ESP_LOGW(TAG, "user kits: no directory — recording works, but "
                      "nothing survives a restart");
    }
    return;
#else

    /* SD first, always: it is bigger by three orders of magnitude, it is the
     * only backend a person can load samples into from a computer, and writing
     * to it does not stall the render chain the way a flash write does. */
    if (ensure_mounted()) {
        mkdir("/sd/osynth", 0777); /* may already exist; the mkdir below is
                                    * what actually decides */
        if (mkdir(kKitDir, 0777) == 0 || errno == EEXIST) {
            s_store = STORE_SD;
            ESP_LOGI(TAG, "user kits: %s", kKitDir);
            return;
        }
    }

    /* LittleFS. Not mounted here — presets owns that, and this runs after it
     * for exactly that reason (see drums_kits_load() in drums.cpp). If /lfs is
     * not there, the mount failed or the partition is being formatted, and the
     * honest answer is that there is nowhere to save. */
    struct stat st;
    if (stat("/lfs", &st) == 0) {
        if (mkdir(kLfsKitDir, 0777) == 0 || errno == EEXIST) {
            s_store = STORE_LFS;
            ESP_LOGW(TAG,
                     "user kits: %s — no SD card, so all kits together get "
                     "about ten seconds of audio and every save stalls the "
                     "audio task until it can be taken while quiet",
                     kLfsKitDir);
            return;
        }
    }

    s_store = STORE_NONE;
    ESP_LOGW(TAG, "user kits: nowhere to save — recording works, but nothing "
                  "survives a power cycle");
#endif /* SYNTH_TARGET_HOST */
}

const char* drum_kit_storage_name(void) {
    switch (s_store) {
        case STORE_SD: return "sd";
        case STORE_LFS: return "lfs";
        default: return "none";
    }
}

esp_err_t drum_kit_load_user(int index, drum_kit_t* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_store == STORE_NONE) return ESP_ERR_NOT_SUPPORTED;

    char dir[kKitPathMax];
    kit_dir(index, dir, sizeof(dir));
    DIR* d = opendir(dir);
    /* No folder yet is the normal state of a kit nobody has recorded into. The
     * caller has already built a valid empty kit named "Kit N", so returning
     * "not found" here is information, not a failure. */
    if (d == nullptr) return ESP_ERR_NOT_FOUND;

    /* The sidecar first: it may rename slots and set play modes for pads whose
     * audio is about to be read in below. */
    (void)read_sidecar(index, out);

    int loaded = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (!has_ext(e->d_name, ".wav")) continue;
        const int slot = slot_prefix(e->d_name);
        /* Unprefixed files are skipped rather than packed into free slots.
         * The recorder always writes a prefix, so an unprefixed file is
         * something a person dropped in, and silently deciding which pad it
         * belongs on would move it every time the folder is re-read. */
        if (slot < 0 || slot >= DRUM_SLOTS) continue;
        if (out->slots[slot].data != nullptr) continue;

        char path[kKitPathMax];
        snprintf(path, sizeof(path), "%.120s/%.63s", dir, e->d_name);
        FILE* f = fopen(path, "rb");
        if (f == nullptr) continue;
        WavInfo w = {};
        if (!wav_open(f, &w)) {
            ESP_LOGW(TAG, "kit %d: %s is not a readable WAV", index, e->d_name);
            fclose(f);
            continue;
        }
        const int ch = w.channels ? w.channels : 1;
        const int bps = w.bits / 8;
        uint32_t frames =
            (bps > 0) ? w.data_bytes / (uint32_t)(bps * ch) : 0u;
        if (frames == 0) {
            fclose(f);
            continue;
        }
        if (frames > kMaxPadFrames) {
            ESP_LOGW(TAG, "kit %d pad %d: %s is longer than %u s, truncated",
                     index, slot + 1, e->d_name,
                     (unsigned)SYNTH_SAMPLE_MAX_SEC);
            frames = kMaxPadFrames;
        }
        void* block = sampler_pool_alloc((size_t)frames * 2);
        if (block == nullptr) {
            ESP_LOGE(TAG, "kit %d pad %d: sample pool full, stopping here",
                     index, slot + 1);
            fclose(f);
            break;
        }
        const uint32_t got = wav_read_mono(f, w, (int16_t*)block, frames);
        fclose(f);
        if (got == 0) {
            sampler_pool_free(block, (size_t)frames * 2);
            continue;
        }

        drum_sample_t& s = out->slots[slot];
        s.data = (const uint8_t*)block;
        s.owned = block;
        s.owned_bytes = (size_t)frames * 2;
        s.frames = got;
        s.rate = w.rate ? w.rate : SYNTH_SAMPLE_RATE;
        s.format = DRUM_FMT_PCM16;
        if (s.gain <= 0.0f) s.gain = 1.0f;
        if (s.note == 0) s.note = (uint8_t)(36 + slot);
        /* A loop that outlives the audio it pointed at would index past the
         * end; a hand-dropped file replacing a recorded one is exactly how
         * that happens. */
        if (s.loop_end > got) {
            s.loop_start = 0;
            s.loop_end = 0;
        }
        if (s.name[0] == '\0') base_name(e->d_name, s.name, sizeof(s.name));
        ++loaded;
    }
    closedir(d);

    out->slot_count = DRUM_SLOTS;
    out->per_slot_owned = true;
    out->dirty = false;
    if (loaded > 0) {
        ESP_LOGI(TAG, "kit %d '%s': %d pad(s) from %s", index, out->name,
                 loaded, dir);
    }
    return loaded > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t drum_kit_save_user(int index, const drum_kit_t* kit) {
    if (kit == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_store == STORE_NONE) return ESP_ERR_NOT_SUPPORTED;
    if (!kit->dirty) return ESP_OK;

    char dir[kKitPathMax];
    kit_dir(index, dir, sizeof(dir));
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "kit %d: cannot create %s", index, dir);
        return ESP_FAIL;
    }

    /* Write the new files before removing anything. A save interrupted by a
     * pulled card then costs the *old* copy of one pad rather than the whole
     * kit, which is the failure mode worth having. */
    char written[DRUM_SLOTS][64];
    for (int i = 0; i < DRUM_SLOTS; ++i) written[i][0] = '\0';

    int saved = 0;
    for (int i = 0; i < DRUM_SLOTS; ++i) {
        const drum_sample_t& s = kit->slots[i];
        if (s.data == nullptr || s.frames == 0) continue;
        /* Only PCM16 pads are written: everything the recorder produces is
         * PCM16, and a mu-law pad here would have come from a kit image that
         * has its own file on the card already. */
        if (s.format != DRUM_FMT_PCM16) continue;
        char nm[32];
        safe_name(s.name[0] ? s.name : "pad", nm, sizeof(nm));
        snprintf(written[i], sizeof(written[i]), "%02d_%.24s.wav", i, nm);
        char path[kKitPathMax];
        snprintf(path, sizeof(path), "%.120s/%.63s", dir, written[i]);
        if (!write_wav(path, (const int16_t*)s.data, s.frames)) {
            ESP_LOGE(TAG, "kit %d pad %d: write failed (%s)", index, i + 1,
                     path);
            written[i][0] = '\0';
            continue;
        }
        ++saved;
    }

    /* Now drop whatever the folder still holds for a pad that is empty or that
     * has been renamed. */
    DIR* d = opendir(dir);
    if (d != nullptr) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (!has_ext(e->d_name, ".wav")) continue;
            const int slot = slot_prefix(e->d_name);
            if (slot < 0 || slot >= DRUM_SLOTS) continue;
            if (strcasecmp(e->d_name, written[slot]) == 0) continue;
            char path[kKitPathMax];
            snprintf(path, sizeof(path), "%.120s/%.63s", dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }

    if (!write_sidecar(index, kit)) {
        ESP_LOGW(TAG, "kit %d: sidecar not written — pads will load with "
                      "default settings",
                 index);
    }
    ESP_LOGI(TAG, "kit %d '%s' saved: %d pad(s) to %s", index, kit->name,
             saved, dir);
    return ESP_OK;
}

#else /* no SD kits and no sample kits */

bool drum_kit_sd_supported(void) { return false; }
int drum_kit_scan_sd(char (*)[DRUM_KIT_NAME_MAX], int) { return 0; }
esp_err_t drum_kit_load_sd(const char*, drum_kit_t*) {
    return ESP_ERR_NOT_SUPPORTED;
}
void drum_kit_storage_init(void) {}
const char* drum_kit_storage_name(void) { return "none"; }
esp_err_t drum_kit_load_user(int, drum_kit_t*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drum_kit_save_user(int, const drum_kit_t*) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OSYNTH_DRUM_SD_KITS || SYNTH_SAMPLE_KITS > 0 */
