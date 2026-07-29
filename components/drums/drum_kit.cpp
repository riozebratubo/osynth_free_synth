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

#include "synth_config.h"

static const char* TAG = "drumkit";

/* The factory image, embedded by CMake (target_add_binary_data). The symbol
 * names are derived from the file name — keep them in sync with the
 * add_custom_command output in CMakeLists.txt. */
extern "C" const uint8_t drumkit_bin_start[] asm("_binary_drumkit_bin_start");
extern "C" const uint8_t drumkit_bin_end[] asm("_binary_drumkit_bin_end");

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
    if (kit->owned != nullptr) heap_caps_free(kit->owned);
    memset(kit, 0, sizeof(*kit));
}

/* ===================== SD-card kits ==================================== */

#if CONFIG_OSYNTH_DRUM_SD_KITS

#include <dirent.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace {

constexpr const char* kMount = "/sd";
constexpr const char* kKitDir = "/sd/osynth/kits";
/* PSRAM ceiling for one loaded kit. Generous for mu-law .okit images (the
 * factory kit is ~240 KB) and enough for a folder of WAVs, while leaving the
 * looper the bulk of the pool. */
constexpr size_t kMaxKitBytes = 2 * 1024 * 1024;

sdmmc_card_t* s_card = nullptr;

/* Idempotent bring-up. The looper's S16 SD backend may already own the bus
 * and the mount point; both are shared, so "already there" is success, not
 * an error. */
bool ensure_mounted() {
    struct stat st;
    if (stat(kMount, &st) == 0) return true; /* someone mounted it already */

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
            if (acc > 0.999f) acc = 0.999f;
            if (acc < -0.999f) acc = -0.999f;
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
        char path[192];
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
        char path[192];
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

    char path[192];
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

#else /* !CONFIG_OSYNTH_DRUM_SD_KITS */

bool drum_kit_sd_supported(void) { return false; }
int drum_kit_scan_sd(char (*)[DRUM_KIT_NAME_MAX], int) { return 0; }
esp_err_t drum_kit_load_sd(const char*, drum_kit_t*) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OSYNTH_DRUM_SD_KITS */
