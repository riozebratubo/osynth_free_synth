/*
 * osynth — looper track persistence backends (Session 16).
 * See loop_store.h for the contract and blob format.
 *
 * Flash backend: raw esp_flash access to the region above the partition
 * table — no partition entry needed, so one partitions.csv keeps serving
 * both targets; the region end comes from the Kconfig module choice
 * (N8R8/N16R8) and is trimmed to the *detected* chip size at boot, so a
 * wrong menu choice degrades to a warning, never a corrupt write. All
 * transfers bounce through a small internal-RAM buffer: the track buffers
 * live in PSRAM, and routing PSRAM pointers into the flash driver is the
 * kind of restriction that varies per IDF version — the bounce makes it a
 * non-question for ~zero cost against multi-second flash programming.
 *
 * SD backend: SDSPI + FAT on the FSPI pins. The bus is initialized once;
 * mounting is retried lazily on every operation so a card inserted after
 * boot just works. An I/O failure unmounts, so the next attempt remounts
 * a re-inserted card.
 *
 * Blobs store tracks IMA-ADPCM since v2 (see loop_store.h; mono sets
 * since S19 pack two frames per byte). Since S20 the PSRAM tracks hold
 * the same bytes, so saves and v2 loads are plain copies; only legacy v1
 * raw blobs still run the encoder here (on the loop_ctl task) to convert
 * into the live format.
 */
#include "loop_store.h"

#include <cstring>

#include "esp_log.h"
#include "sdkconfig.h"

#include "loop_adpcm.h"
#include "synth_config.h"

static const char* TAG = "loop_store";

#if CONFIG_SPIRAM && SYNTH_ENABLE_LOOP_PERSIST

#include "esp_heap_caps.h"

namespace {

enum : uint8_t {
    CODEC_RAW = 0,       /* v1 blobs: raw stereo int16 */
    CODEC_IMA_ADPCM = 1, /* stereo, one byte per frame (L high nibble) */
    CODEC_IMA_ADPCM_MONO = 2, /* S19 mono sets: two frames per byte */
};

struct StoreHdr {
    char magic[4];        /* "OSL1" */
    uint8_t version;      /* 1 = raw only, 2 = codec byte below is live */
    uint8_t filled;       /* bitmask of stored tracks */
    uint8_t tracks;       /* LOOP_TRACKS */
    uint8_t codec;        /* CODEC_*; was rsvd (== 0 == raw) in v1 blobs */
    uint32_t loop_frames; /* stereo frames per track */
    uint32_t sample_rate;
    uint32_t track_bytes; /* per stored track: raw 4 B/frame, adpcm 1 B/frame */
    uint32_t rsvd2[3];
};
static_assert(sizeof(StoreHdr) == 32, "blob header is 32 bytes");

constexpr uint32_t kHdrSize = sizeof(StoreHdr);

uint32_t codec_track_bytes(uint8_t codec, uint32_t loop_frames) {
    switch (codec) {
        case CODEC_IMA_ADPCM:
            return loop_frames * LOOP_STORE_BYTES_PER_FRAME;
        case CODEC_IMA_ADPCM_MONO:
            return (loop_frames + 1) / 2; /* odd tail pads the low nibble */
        default:
            return loop_frames * 4;
    }
}

bool hdr_valid(const StoreHdr& h) {
    if (memcmp(h.magic, "OSL1", 4) != 0) return false;
    if (h.version != 1 && h.version != 2) return false;
    const uint8_t codec = h.version == 1 ? CODEC_RAW : h.codec;
    if (codec != CODEC_RAW && codec != CODEC_IMA_ADPCM &&
        codec != CODEC_IMA_ADPCM_MONO) {
        return false;
    }
    if (h.tracks != LOOP_TRACKS || h.filled == 0) return false;
    if (h.sample_rate != (uint32_t)SYNTH_SAMPLE_RATE) {
        ESP_LOGW(TAG, "slot sample rate %u != build %d — refusing",
                 (unsigned)h.sample_rate, SYNTH_SAMPLE_RATE);
        return false;
    }
    if (h.loop_frames == 0 ||
        h.track_bytes != codec_track_bytes(codec, h.loop_frames)) {
        return false;
    }
    if (h.track_bytes > 16u * 1024 * 1024) return false; /* sanity */
    return true;
}

uint8_t hdr_codec(const StoreHdr& h) {
    return h.version == 1 ? CODEC_RAW : h.codec;
}

void fill_hdr(StoreHdr& h, uint32_t loop_frames, uint8_t filled,
              uint8_t codec) {
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "OSL1", 4);
    h.version = 2;
    h.filled = filled;
    h.tracks = LOOP_TRACKS;
    h.codec = codec;
    h.loop_frames = loop_frames;
    h.sample_rate = (uint32_t)SYNTH_SAMPLE_RATE;
    h.track_bytes = codec_track_bytes(codec, loop_frames);
}

/* Legacy v1 conversion: raw stereo int16 frames -> stereo ADPCM (the S20
 * PSRAM/live format, one byte per frame, L in the high nibble). State
 * carries across chunks; both channels start zeroed at frame 0, matching
 * the render path's wrap-reset contract (loop_adpcm.h). */
void encode_raw_frames(osynth::adpcm::Ch& cl, osynth::adpcm::Ch& cr,
                       const int16_t* src, uint8_t* dst, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t l = osynth::adpcm::encode(cl, src[2 * i]);
        const uint8_t r = osynth::adpcm::encode(cr, src[2 * i + 1]);
        dst[i] = (uint8_t)((l << 4) | r);
    }
}

} // namespace

#if !SYNTH_LOOP_STORE_SD

/* ================= flash-region backend (slot 0 only) ================= */

#include "esp_flash.h"

namespace {

constexpr uint32_t kFlashBase = 0x400000; /* right after `storage` */
#if CONFIG_OSYNTH_FLASH_32MB
constexpr uint32_t kFlashEndCfg = 0x2000000; /* 32 MB module */
#elif CONFIG_OSYNTH_FLASH_16MB
constexpr uint32_t kFlashEndCfg = 0x1000000; /* 16 MB module */
#else
constexpr uint32_t kFlashEndCfg = 0x800000; /* 8 MB module */
#endif
constexpr uint32_t kSector = 0x1000;
constexpr size_t kBounce = 8192; /* internal-RAM staging chunk */

uint32_t s_flash_end = 0; /* 0 = backend unavailable */
uint8_t* s_bounce = nullptr;

bool ensure_bounce() {
    if (s_bounce == nullptr) {
        s_bounce = (uint8_t*)heap_caps_malloc(
            kBounce, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return s_bounce != nullptr;
}

/* Data for stored track i sits at kFlashBase + kSector + i * track_bytes
 * (the header keeps its own first sector, written last). */

esp_err_t flash_write_chunked(uint32_t addr, const uint8_t* src, size_t len) {
    while (len > 0) {
        const size_t n = len < kBounce ? len : kBounce;
        memcpy(s_bounce, src, n);
        esp_err_t err = esp_flash_write(nullptr, s_bounce, addr, n);
        if (err != ESP_OK) return err;
        addr += n;
        src += n;
        len -= n;
    }
    return ESP_OK;
}

esp_err_t flash_read_chunked(uint32_t addr, uint8_t* dst, size_t len) {
    while (len > 0) {
        const size_t n = len < kBounce ? len : kBounce;
        esp_err_t err = esp_flash_read(nullptr, s_bounce, addr, n);
        if (err != ESP_OK) return err;
        memcpy(dst, s_bounce, n);
        addr += n;
        dst += n;
        len -= n;
    }
    return ESP_OK;
}

} // namespace

extern "C" esp_err_t loop_store_init(void) {
    uint32_t phys = 0;
    esp_err_t err = esp_flash_get_physical_size(nullptr, &phys);
    if (err != ESP_OK || phys <= kFlashBase + 2 * kSector) {
        ESP_LOGW(TAG,
                 "flash backend unavailable: chip %u KB leaves no room above "
                 "0x%06x — loop save/load disabled",
                 (unsigned)(phys / 1024), (unsigned)kFlashBase);
        s_flash_end = 0;
        return ESP_OK;
    }
    s_flash_end = phys < kFlashEndCfg ? phys : kFlashEndCfg;
    if (phys < kFlashEndCfg) {
        ESP_LOGW(TAG,
                 "chip is %u MB but the Kconfig module choice expects %u MB — "
                 "loops region trimmed",
                 (unsigned)(phys >> 20), (unsigned)(kFlashEndCfg >> 20));
    }
    ESP_LOGI(TAG,
             "flash backend: region 0x%06x-0x%06x (%u KB, ~%u s stereo "
             "adpcm, 2x mono), 1 slot, save/load need the transport stopped",
             (unsigned)kFlashBase, (unsigned)s_flash_end,
             (unsigned)((s_flash_end - kFlashBase) / 1024),
             (unsigned)((s_flash_end - kFlashBase - kSector) /
                        (SYNTH_SAMPLE_RATE * LOOP_STORE_BYTES_PER_FRAME)));
    return ESP_OK;
}

extern "C" bool loop_store_ready(void) { return s_flash_end != 0; }
extern "C" const char* loop_store_backend_name(void) { return "flash"; }
extern "C" int loop_store_slots(void) { return LOOP_STORE_SLOTS_FLASH; }
extern "C" bool loop_store_needs_stopped(void) { return true; }

extern "C" esp_err_t loop_store_save(int slot, uint32_t loop_frames,
                                     uint8_t filled,
                                     uint8_t* const bufs[LOOP_TRACKS],
                                     bool mono) {
    if (s_flash_end == 0) return ESP_ERR_INVALID_STATE;
    if (slot != 0) {
        ESP_LOGW(TAG, "flash backend has a single slot (0)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!ensure_bounce()) return ESP_ERR_NO_MEM;
    const uint8_t codec = mono ? CODEC_IMA_ADPCM_MONO : CODEC_IMA_ADPCM;
    const uint32_t track_bytes = codec_track_bytes(codec, loop_frames);
    int count = 0;
    for (int t = 0; t < LOOP_TRACKS; ++t) count += (filled >> t) & 1;
    const uint32_t total = kSector + (uint32_t)count * track_bytes;
    if (kFlashBase + total > s_flash_end) {
        ESP_LOGW(TAG,
                 "set too big for the loops region: %u KB > %u KB "
                 "(fewer tracks or a shorter loop)",
                 (unsigned)(total / 1024),
                 (unsigned)((s_flash_end - kFlashBase) / 1024));
        return ESP_ERR_NO_MEM;
    }
    const uint32_t erase_len = (total + kSector - 1) & ~(kSector - 1);
    ESP_LOGI(TAG, "saving %d track(s) (%u KB %sadpcm) to flash…", count,
             (unsigned)(total / 1024), mono ? "mono " : "");
    esp_err_t err = esp_flash_erase_region(nullptr, kFlashBase, erase_len);
    if (err != ESP_OK) return err;
    /* S20: the PSRAM buffers already hold the stored bytes — plain copy
     * (chunked through the internal bounce; the flash driver must not see
     * PSRAM pointers) */
    uint32_t addr = kFlashBase + kSector;
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        if (((filled >> t) & 1) == 0) continue;
        err = flash_write_chunked(addr, bufs[t], track_bytes);
        if (err != ESP_OK) return err;
        addr += track_bytes;
    }
    StoreHdr h;
    fill_hdr(h, loop_frames, filled, codec);
    /* header last: a torn save can never validate */
    return flash_write_chunked(kFlashBase, (const uint8_t*)&h, kHdrSize);
}

extern "C" esp_err_t loop_store_probe(int slot, uint32_t* loop_frames,
                                      uint8_t* filled, bool* mono) {
    if (s_flash_end == 0) return ESP_ERR_INVALID_STATE;
    if (slot != 0) return ESP_ERR_INVALID_ARG;
    if (!ensure_bounce()) return ESP_ERR_NO_MEM;
    StoreHdr h;
    esp_err_t err = flash_read_chunked(kFlashBase, (uint8_t*)&h, kHdrSize);
    if (err != ESP_OK) return err;
    if (!hdr_valid(h)) return ESP_ERR_NOT_FOUND;
    *loop_frames = h.loop_frames;
    *filled = h.filled;
    *mono = hdr_codec(h) == CODEC_IMA_ADPCM_MONO;
    return ESP_OK;
}

extern "C" esp_err_t loop_store_read_track(int slot, int packed_idx,
                                           uint8_t* dst,
                                           uint32_t loop_frames) {
    if (s_flash_end == 0 || slot != 0) return ESP_ERR_INVALID_STATE;
    if (!ensure_bounce()) return ESP_ERR_NO_MEM;
    /* re-read the header: the codec (stereo/mono adpcm vs legacy v1 raw)
     * decides the track stride and whether a conversion is needed */
    StoreHdr h;
    esp_err_t err = flash_read_chunked(kFlashBase, (uint8_t*)&h, kHdrSize);
    if (err != ESP_OK) return err;
    if (!hdr_valid(h) || h.loop_frames != loop_frames) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t addr =
        kFlashBase + kSector + (uint32_t)packed_idx * h.track_bytes;
    if (hdr_codec(h) != CODEC_RAW) {
        /* v2 stereo/mono adpcm: the blob bytes are the PSRAM format */
        return flash_read_chunked(addr, dst, h.track_bytes);
    }
    /* legacy v1 raw stereo int16: encode into the S20 stereo-adpcm track */
    osynth::adpcm::Ch cl, cr;
    uint32_t left = loop_frames;
    uint32_t a = addr;
    while (left > 0) {
        const uint32_t n =
            left < kBounce / 4 ? left : (uint32_t)(kBounce / 4);
        err = esp_flash_read(nullptr, s_bounce, a, (size_t)n * 4);
        if (err != ESP_OK) return err;
        encode_raw_frames(cl, cr, (const int16_t*)s_bounce, dst, n);
        dst += n;
        a += n * 4;
        left -= n;
    }
    return ESP_OK;
}

#else /* SYNTH_LOOP_STORE_SD */

/* ================= SD-card backend (SDSPI + FAT, 8 slots) ============= */

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace {

constexpr const char* kMount = "/sd";
constexpr const char* kDir = "/sd/osynth";
constexpr size_t kChunk = 8192; /* staging for the legacy v1 raw->adpcm
                                 * conversion (2048 frames per pass) */

bool s_bus_up = false;
sdmmc_card_t* s_card = nullptr;
uint8_t* s_chunk = nullptr;

bool ensure_chunk() {
    if (s_chunk == nullptr) {
        s_chunk = (uint8_t*)heap_caps_malloc(
            kChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return s_chunk != nullptr;
}

void slot_path(char* out, size_t n, int slot, bool tmp) {
    snprintf(out, n, "%s/loop%d.%s", kDir, slot, tmp ? "tmp" : "olp");
}

bool ensure_mounted() {
    if (!s_bus_up) return false;
    if (s_card != nullptr) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = (gpio_num_t)CONFIG_OSYNTH_SD_CS_GPIO;
    dev.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 2;
    mount_cfg.allocation_unit_size = 16 * 1024;
    esp_err_t err = esp_vfs_fat_sdspi_mount(kMount, &host, &dev, &mount_cfg,
                                            &s_card);
    if (err != ESP_OK) {
        s_card = nullptr;
        ESP_LOGW(TAG, "SD mount failed (%s) — card inserted?",
                 esp_err_to_name(err));
        return false;
    }
    mkdir(kDir, 0775); /* EEXIST is fine */
    ESP_LOGI(TAG, "SD mounted: %llu MB",
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >>
                 20);
    return true;
}

/* An I/O error usually means the card went away: unmount so the next
 * operation attempts a fresh mount. */
void drop_mount() {
    if (s_card != nullptr) {
        esp_vfs_fat_sdcard_unmount(kMount, s_card);
        s_card = nullptr;
        ESP_LOGW(TAG, "SD unmounted after I/O error — will remount on retry");
    }
}

} // namespace

extern "C" esp_err_t loop_store_init(void) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = CONFIG_OSYNTH_SD_MOSI_GPIO;
    bus.miso_io_num = CONFIG_OSYNTH_SD_MISO_GPIO;
    bus.sclk_io_num = CONFIG_OSYNTH_SD_SCK_GPIO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed (%s) — SD backend disabled",
                 esp_err_to_name(err));
        s_bus_up = false;
        return ESP_OK;
    }
    s_bus_up = true;
    ESP_LOGI(TAG,
             "SD backend: SPI2 cs=%d sck=%d mosi=%d miso=%d, %d slots, "
             "save/load work while playing",
             CONFIG_OSYNTH_SD_CS_GPIO, CONFIG_OSYNTH_SD_SCK_GPIO,
             CONFIG_OSYNTH_SD_MOSI_GPIO, CONFIG_OSYNTH_SD_MISO_GPIO,
             LOOP_STORE_SLOTS_SD);
    ensure_mounted(); /* warns if no card; retried per operation */
    return ESP_OK;
}

extern "C" bool loop_store_ready(void) { return s_bus_up; }
extern "C" const char* loop_store_backend_name(void) { return "sd"; }
extern "C" int loop_store_slots(void) { return LOOP_STORE_SLOTS_SD; }
extern "C" bool loop_store_needs_stopped(void) { return false; }

extern "C" esp_err_t loop_store_save(int slot, uint32_t loop_frames,
                                     uint8_t filled,
                                     uint8_t* const bufs[LOOP_TRACKS],
                                     bool mono) {
    if (slot < 0 || slot >= LOOP_STORE_SLOTS_SD) return ESP_ERR_INVALID_ARG;
    if (!ensure_mounted()) return ESP_ERR_INVALID_STATE;
    char tmp[48], path[48];
    slot_path(tmp, sizeof(tmp), slot, true);
    slot_path(path, sizeof(path), slot, false);
    FILE* f = fopen(tmp, "wb");
    if (f == nullptr) {
        drop_mount();
        return ESP_FAIL;
    }
    const uint8_t codec = mono ? CODEC_IMA_ADPCM_MONO : CODEC_IMA_ADPCM;
    const uint32_t track_bytes = codec_track_bytes(codec, loop_frames);
    StoreHdr h;
    fill_hdr(h, loop_frames, filled, codec);
    bool ok = fwrite(&h, 1, kHdrSize, f) == kHdrSize;
    /* S20: the PSRAM buffers already hold the stored bytes — plain copy
     * (FATFS is fine with PSRAM sources) */
    for (int t = 0; ok && t < LOOP_TRACKS; ++t) {
        if (((filled >> t) & 1) == 0) continue;
        ok = fwrite(bufs[t], 1, track_bytes, f) == track_bytes;
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp);
        drop_mount();
        return ESP_FAIL;
    }
    remove(path); /* FAT rename does not overwrite */
    if (rename(tmp, path) != 0) {
        remove(tmp);
        drop_mount();
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t loop_store_probe(int slot, uint32_t* loop_frames,
                                      uint8_t* filled, bool* mono) {
    if (slot < 0 || slot >= LOOP_STORE_SLOTS_SD) return ESP_ERR_INVALID_ARG;
    if (!ensure_mounted()) return ESP_ERR_INVALID_STATE;
    char path[48];
    slot_path(path, sizeof(path), slot, false);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return ESP_ERR_NOT_FOUND;
    StoreHdr h;
    const bool ok = fread(&h, 1, kHdrSize, f) == kHdrSize;
    fclose(f);
    if (!ok || !hdr_valid(h)) return ESP_ERR_NOT_FOUND;
    *loop_frames = h.loop_frames;
    *filled = h.filled;
    *mono = hdr_codec(h) == CODEC_IMA_ADPCM_MONO;
    return ESP_OK;
}

extern "C" esp_err_t loop_store_read_track(int slot, int packed_idx,
                                           uint8_t* dst,
                                           uint32_t loop_frames) {
    if (slot < 0 || slot >= LOOP_STORE_SLOTS_SD) return ESP_ERR_INVALID_ARG;
    if (!ensure_mounted()) return ESP_ERR_INVALID_STATE;
    char path[48];
    slot_path(path, sizeof(path), slot, false);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return ESP_ERR_NOT_FOUND;
    /* the header's codec (stereo/mono adpcm vs legacy v1 raw) decides the
     * track stride and whether a conversion is needed */
    StoreHdr h;
    bool ok = fread(&h, 1, kHdrSize, f) == kHdrSize;
    if (ok && (!hdr_valid(h) || h.loop_frames != loop_frames)) {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }
    ok = ok && fseek(f, (long)(kHdrSize + (uint32_t)packed_idx * h.track_bytes),
                     SEEK_SET) == 0;
    if (ok && hdr_codec(h) != CODEC_RAW) {
        /* v2 stereo/mono adpcm: the blob bytes are the PSRAM format */
        ok = fread(dst, 1, h.track_bytes, f) == h.track_bytes;
    } else if (ok) {
        /* legacy v1 raw stereo int16: encode into the stereo-adpcm track */
        ok = ensure_chunk();
        osynth::adpcm::Ch cl, cr;
        uint32_t left = loop_frames;
        while (ok && left > 0) {
            const uint32_t n =
                left < kChunk / 4 ? left : (uint32_t)(kChunk / 4);
            ok = fread(s_chunk, 1, (size_t)n * 4, f) == (size_t)n * 4;
            if (ok) {
                encode_raw_frames(cl, cr, (const int16_t*)s_chunk, dst, n);
                dst += n;
            }
            left -= n;
        }
    }
    fclose(f);
    if (!ok) {
        drop_mount();
        return ESP_FAIL;
    }
    return ESP_OK;
}

#endif /* SYNTH_LOOP_STORE_SD */

#endif /* CONFIG_SPIRAM && SYNTH_ENABLE_LOOP_PERSIST */
