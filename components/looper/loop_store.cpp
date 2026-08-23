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
/* The export API hands these codec numbers to a BLE client (loop_store.h), so
 * they are wire values now and the two spellings must agree. */
static_assert(CODEC_RAW == LOOP_STORE_CODEC_RAW, "codec numbering drifted");
static_assert(CODEC_IMA_ADPCM == LOOP_STORE_CODEC_ADPCM, "codec numbering drifted");
static_assert(CODEC_IMA_ADPCM_MONO == LOOP_STORE_CODEC_ADPCM_MONO,
              "codec numbering drifted");

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

/* Fills `out` from a header that hdr_valid() has already accepted. Shared by
 * the two backends' loop_store_slot_info(). */
void info_from_hdr(const StoreHdr& h, loop_store_info_t* out) {
    out->loop_frames = h.loop_frames;
    out->sample_rate = h.sample_rate;
    out->track_bytes = h.track_bytes;
    out->filled = h.filled;
    out->codec = hdr_codec(h);
}

/* Bounds one export read against the track it claims to be inside. Rejecting
 * rather than clamping is deliberate — see loop_store.h. */
bool export_range_ok(const StoreHdr& h, int packed_idx, uint32_t offset,
                     uint32_t len) {
    int stored = 0;
    for (int t = 0; t < LOOP_TRACKS; ++t) stored += (h.filled >> t) & 1;
    if (packed_idx < 0 || packed_idx >= stored) return false;
    /* No overflow check needed on offset + len: both are bounded by
     * track_bytes, which hdr_valid() caps at 16 MB. */
    return offset <= h.track_bytes && len <= h.track_bytes - offset;
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
extern "C" bool loop_store_mount(void) { return false; } /* no card here */
extern "C" bool loop_store_ensure_dir(const char*) { return false; }
extern "C" loop_store_card_t loop_store_poll_card(void) {
    return LOOP_STORE_CARD_OK; /* no card to lose */
}
extern "C" void loop_store_card_gone(void) {}
extern "C" uint32_t loop_store_card_serial(void) { return 0; }

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

extern "C" esp_err_t loop_store_slot_info(int slot, loop_store_info_t* out) {
    if (s_flash_end == 0) return ESP_ERR_INVALID_STATE;
    if (slot != 0 || out == nullptr) return ESP_ERR_INVALID_ARG;
    if (!ensure_bounce()) return ESP_ERR_NO_MEM;
    StoreHdr h;
    esp_err_t err = flash_read_chunked(kFlashBase, (uint8_t*)&h, kHdrSize);
    if (err != ESP_OK) return err;
    if (!hdr_valid(h)) return ESP_ERR_NOT_FOUND;
    info_from_hdr(h, out);
    return ESP_OK;
}

extern "C" esp_err_t loop_store_read_slot_bytes(int slot, int packed_idx,
                                                uint32_t offset, uint8_t* dst,
                                                uint32_t len) {
    if (s_flash_end == 0) return ESP_ERR_INVALID_STATE;
    if (slot != 0 || dst == nullptr) return ESP_ERR_INVALID_ARG;
    if (len == 0) return ESP_OK;
    if (!ensure_bounce()) return ESP_ERR_NO_MEM;
    /* Re-read the header on every call. An export is a long sequence of small
     * reads and the alternative — trusting a header cached at the start —
     * would keep serving a slot that a save has since overwritten. 32 bytes
     * against ~2 KB of payload, and this is not a throughput path. */
    StoreHdr h;
    esp_err_t err = flash_read_chunked(kFlashBase, (uint8_t*)&h, kHdrSize);
    if (err != ESP_OK) return err;
    if (!hdr_valid(h)) return ESP_ERR_NOT_FOUND;
    if (!export_range_ok(h, packed_idx, offset, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    return flash_read_chunked(
        kFlashBase + kSector + (uint32_t)packed_idx * h.track_bytes + offset,
        dst, len);
}

#else /* SYNTH_LOOP_STORE_SD */

/* ================= SD-card backend (SDSPI + FAT, 8 slots) ============= */

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#if defined(CONFIG_OSYNTH_SD_PWR_LDO_CHAN) && CONFIG_OSYNTH_SD_PWR_LDO_CHAN > 0
#include "esp_ldo_regulator.h"
#define OSYNTH_SD_LDO 1
#else
#define OSYNTH_SD_LDO 0
#endif

/* Card bring-up diagnostics. The sdmmc/sdspi drivers name the failing
 * command and print its R1 byte at DEBUG only — and every one of those lines
 * is compiled out unless CONFIG_LOG_MAXIMUM_LEVEL is Debug or higher, so a
 * failing mount shows nothing but the ESP_ERR_TIMEOUT. Set this to 1 *and*
 * raise the maximum log verbosity in menuconfig (Component config -> Log ->
 * Maximum log verbosity -> Debug) to get the CMD0/CMD8/CMD55 trail. Leave it
 * at 0 once the card mounts: it is noisy on every operation. */
#define OSYNTH_SD_VERBOSE 0

/* Bring-up escape hatch: skip CMD59 (SD_CRC_ON_OFF) during card init. Some
 * cards stop answering commands entirely once command/data CRC checking is
 * turned on — the signature is a clean CMD0/CMD8/CMD59 trail followed by
 * CMD55 timing out with no R1 at all. IDF exposes this as a host flag for
 * exactly that reason; it costs the data-integrity check on transfers, so
 * only turn it on if it is what makes the card mount. */
#define OSYNTH_SD_SKIP_CRC 0

/* Bus clock after init (init itself is always 400 kHz). 20 MHz is the IDF
 * default and what the streamed looper's throughput budget assumes. Reads
 * tolerate a marginal bus far better than writes do — flying leads, no
 * pull-ups or a long ribbon show up as a card that mounts and reads happily
 * but fails the moment something is written (EIO out of mkdir/fwrite). Drop
 * to 10000 or 4000 if that is what the logs say; the streamed looper needs
 * roughly 430 KB/s at eight tracks, so 10 MHz still has headroom and 4 MHz
 * does not. */
#define OSYNTH_SD_FREQ_KHZ 20000

namespace {

constexpr const char* kMount = "/sd";
constexpr const char* kDir = "/sd/osynth";
constexpr size_t kChunk = 8192; /* staging for the legacy v1 raw->adpcm
                                 * conversion (2048 frames per pass) */

bool s_bus_up = false;
sdmmc_card_t* s_card = nullptr;
uint8_t* s_chunk = nullptr;

/* Re-mount backoff. A mount attempt against an empty slot costs about a
 * second of the calling task (the ACMD41 retry loop inside sdmmc_init), which
 * is nothing once but everything when it repeats: the idle poll would spend a
 * third of loop_ctl on a synth that simply has no card in it. So the poll
 * doubles its own floor while attempts keep failing, and resets it the moment
 * one succeeds or a card that was working goes away — the case where it is
 * genuinely expected back. Demand paths (save, load, starting a streamed set)
 * ignore the floor entirely: the user is waiting, and a second is worth it. */
constexpr uint32_t kMountFloorMinMs = 2000;
constexpr uint32_t kMountFloorMaxMs = 30000;
uint32_t s_mount_floor_ms = kMountFloorMinMs;
uint32_t s_mount_next_ms = 0;
int s_mount_fails = 0; /* only the first failure of a run is worth logging */

uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

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

/* True when the errno says the card is not answering rather than that the
 * filesystem said no. Worth separating because every operation against a card
 * in this state costs a full sdspi timeout (~1.1 s, measured), and the paths
 * that hit it run on loop_ctl, which does not yield: a handful of them in a
 * row is a task watchdog reset. So the answer to a card that has stopped
 * talking is to stop talking to it, not to investigate. */
bool card_unresponsive(int e) {
    return e == EIO || e == ETIMEDOUT || e == ENODEV;
}

/* mkdir, checked. See loop_store.h for why this is not a bare mkdir call. */
bool ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        ESP_LOGE(TAG, "%s exists but is not a directory", path);
        return false;
    }
    /* An unresponsive card is not a missing directory, and the distinction is
     * the whole diagnosis: one is a card that has gone away mid-session, the
     * other a filesystem that answered. Checked here rather than after the
     * mkdir so a dead card costs one timeout instead of two.
     *
     * Reported, not acted on. Unmounting here would free the FATFS context
     * that the streamed looper's open track handles point into, from a call
     * that knows nothing about them; the idle poll is the one place that
     * unmounts, and it has the caller give those handles up first. A second
     * later at worst, and correct. */
    if (card_unresponsive(errno)) {
        ESP_LOGE(TAG, "%s unreadable (%s) — the card stopped answering", path,
                 strerror(errno));
        return false;
    }
    if (mkdir(path, 0775) == 0) return true;
    if (errno == EEXIST) return true; /* lost a race with another mount user */
    ESP_LOGE(TAG, "cannot create %s (%s)", path, strerror(errno));
    return false;
}

/* Power the SD rail (P4 boards whose socket hangs off the SDMMC slot-1 pads).
 * See the OSYNTH_SD_PWR_LDO_CHAN help: those pads and the card's VDD are fed
 * by an on-chip LDO that nothing turns on by itself, so the symptom of
 * skipping this is not a slow mount but a silent bus — CMD0 times out and the
 * log says "card inserted?" about a card that is.
 *
 * Non-adjustable and never released: the SD drum kits bring the same bus up
 * independently and need their own reference to the same channel, which the
 * regulator refcounts only while every owner agrees the voltage is fixed.
 * Called before spi_bus_initialize() so the pads have a supply before anything
 * drives them, and idempotent because loop_store_init() is not the only path
 * that reaches a cold bus. */
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
        ESP_LOGE(TAG,
                 "SD rail LDO%d refused (%s) — a card on the SDMMC pads will "
                 "not answer",
                 CONFIG_OSYNTH_SD_PWR_LDO_CHAN, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SD rail: LDO%d at %d mV", CONFIG_OSYNTH_SD_PWR_LDO_CHAN,
             CONFIG_OSYNTH_SD_PWR_LDO_MV);
#endif
}

bool ensure_mounted() {
    if (!s_bus_up) return false;
    if (s_card != nullptr) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = OSYNTH_SD_FREQ_KHZ;
#if OSYNTH_SD_SKIP_CRC
    host.flags |= SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC;
#endif
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = (gpio_num_t)CONFIG_OSYNTH_SD_CS_GPIO;
    dev.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    /* FILE slots for the whole mount, which is shared: the streamed looper
     * holds one per playing track plus the open take (LOOP_TRACKS + 1), the
     * slot save/load paths hold two, and the SD drum kits read through the
     * same mount. Two was enough when the only user was save/load, and became
     * an ENFILE the moment a streamed set played more than one track. */
    mount_cfg.max_files = LOOP_TRACKS + 4;
    mount_cfg.allocation_unit_size = 16 * 1024;
    esp_err_t err = esp_vfs_fat_sdspi_mount(kMount, &host, &dev, &mount_cfg,
                                            &s_card);
    if (err != ESP_OK) {
        s_card = nullptr;
        /* Only the first failure of a run: the idle poll retries forever, and
         * a synth with no card would otherwise print this until it is fed
         * one. A success resets the counter, so the next real problem still
         * announces itself. */
        if (s_mount_fails == 0) {
            if (err == ESP_ERR_INVALID_STATE) {
                /* Someone else registered /sd first — the SD drum kits mount
                 * the same card. They check for an existing mount and we do
                 * not, so this is the losing order, and the looper's SD
                 * backend stays dead until it is fixed rather than sharing
                 * what is there. */
                ESP_LOGE(TAG, "%s already mounted by another component — the "
                              "looper's SD backend is disabled", kMount);
            } else {
                ESP_LOGW(TAG, "SD mount failed (%s) — card inserted?",
                         esp_err_to_name(err));
            }
        }
        ++s_mount_fails;
        return false;
    }
    s_mount_fails = 0;
    ESP_LOGI(TAG, "SD mounted: %llu MB",
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >>
                 20);
    /* After the mount line, so a card that reads but cannot be written says
     * so in that order rather than looking like a mount failure. This is also
     * the first real traffic over the freshly mounted card, so it is where a
     * card that answered the init sequence and nothing since gets caught —
     * ensure_dir() unmounts in that case, and a mount with no card behind it
     * is not one we should report as good. */
    ensure_dir(kDir);
    return s_card != nullptr;
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
#if OSYNTH_SD_VERBOSE
    esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_init", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_sd", ESP_LOG_DEBUG);
    esp_log_level_set("sdspi_transaction", ESP_LOG_DEBUG);
    esp_log_level_set("sdspi_host", ESP_LOG_DEBUG);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_DEBUG);
#endif
    sd_power_up();
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

/* Shared with loop_stream.cpp so the card has exactly one mount. Callers are
 * loop_ctl and, once, looper_init() before the loop_ctl task can reach any
 * card path — a single logical thread, which is what keeps ensure_mounted()'s
 * lazy retry free of a lock. Do not call this from loop_io. */
extern "C" bool loop_store_mount(void) { return ensure_mounted(); }

extern "C" bool loop_store_ensure_dir(const char* path) {
    return path != nullptr && ensure_mounted() && ensure_dir(path);
}

extern "C" loop_store_card_t loop_store_poll_card(void) {
    if (!s_bus_up) return LOOP_STORE_CARD_NONE;
    if (s_card != nullptr) {
        /* CMD13. No data phase, so it costs a few hundred microseconds on a
         * card that is there and one command timeout on one that is not —
         * unlike a read, which would also drag FATFS through a cache miss. */
        if (sdmmc_get_status(s_card) == ESP_OK) return LOOP_STORE_CARD_OK;
        ESP_LOGW(TAG, "card stopped answering — treating it as removed");
        return LOOP_STORE_CARD_LOST; /* handles first, then card_gone() */
    }
    const uint32_t now = now_ms();
    if ((int32_t)(now - s_mount_next_ms) < 0) return LOOP_STORE_CARD_NONE;
    if (!ensure_mounted()) {
        s_mount_floor_ms = s_mount_floor_ms >= kMountFloorMaxMs / 2
                               ? kMountFloorMaxMs
                               : s_mount_floor_ms * 2;
        s_mount_next_ms = now + s_mount_floor_ms;
        return LOOP_STORE_CARD_NONE;
    }
    s_mount_floor_ms = kMountFloorMinMs;
    s_mount_next_ms = 0;
    return LOOP_STORE_CARD_OK;
}

extern "C" uint32_t loop_store_card_serial(void) {
    return s_card != nullptr ? (uint32_t)s_card->cid.serial : 0u;
}

extern "C" void loop_store_card_gone(void) {
    if (s_card == nullptr) return;
    drop_mount();
    /* A card that was working is the one case where it is genuinely expected
     * back, so start the backoff over rather than inheriting whatever the
     * last empty-slot run had wound it up to. */
    s_mount_floor_ms = kMountFloorMinMs;
    s_mount_next_ms = now_ms() + kMountFloorMinMs;
    s_mount_fails = 0;
}

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

extern "C" esp_err_t loop_store_slot_info(int slot, loop_store_info_t* out) {
    if (slot < 0 || slot >= LOOP_STORE_SLOTS_SD || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ensure_mounted()) return ESP_ERR_INVALID_STATE;
    char path[48];
    slot_path(path, sizeof(path), slot, false);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return ESP_ERR_NOT_FOUND;
    StoreHdr h;
    const bool ok = fread(&h, 1, kHdrSize, f) == kHdrSize;
    fclose(f);
    if (!ok || !hdr_valid(h)) return ESP_ERR_NOT_FOUND;
    info_from_hdr(h, out);
    return ESP_OK;
}

extern "C" esp_err_t loop_store_read_slot_bytes(int slot, int packed_idx,
                                                uint32_t offset, uint8_t* dst,
                                                uint32_t len) {
    if (slot < 0 || slot >= LOOP_STORE_SLOTS_SD || dst == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) return ESP_OK;
    if (!ensure_mounted()) return ESP_ERR_INVALID_STATE;
    char path[48];
    slot_path(path, sizeof(path), slot, false);
    /* Opened and closed per call. An export runs at BLE speed — a couple of
     * kilobytes per round trip — so the directory lookup is noise next to the
     * link, and a handle held across the whole transfer would be one more
     * thing the card-lost path had to know about (loop_store.h's LOST
     * contract: every open handle is dangling, and the caller must give them
     * up before the unmount). Nothing to give up is the simpler answer. */
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return ESP_ERR_NOT_FOUND;
    StoreHdr h;
    bool ok = fread(&h, 1, kHdrSize, f) == kHdrSize;
    if (!ok || !hdr_valid(h)) {
        fclose(f);
        return ok ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!export_range_ok(h, packed_idx, offset, len)) {
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t pos =
        kHdrSize + (uint32_t)packed_idx * h.track_bytes + offset;
    ok = fseek(f, (long)pos, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
    fclose(f);
    if (!ok) {
        drop_mount();
        return ESP_FAIL;
    }
    return ESP_OK;
}

#endif /* SYNTH_LOOP_STORE_SD */

#endif /* CONFIG_SPIRAM && SYNTH_ENABLE_LOOP_PERSIST */
