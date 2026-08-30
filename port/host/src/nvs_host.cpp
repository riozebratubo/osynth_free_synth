/*
 * osynth host port — the file-backed NVS. See nvs.h for the contract.
 *
 * One file per namespace under <data root>/nvs, holding length-prefixed
 * key/value records:
 *
 *     magic "OSNV" | u16 version | u16 count
 *     then count x { u8 key_len, key bytes, u32 value_len, value bytes }
 *
 * The format is this port's own and is never read by the firmware, so it is
 * shaped for being easy to verify rather than compact. What it does share with
 * NVS is the property persist.cpp depends on: a commit lands whole or not at
 * all, because it writes a temporary file and renames it over the old one.
 */
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_log.h"
#include "host_paths.h"
#include "synth_file.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

const char* TAG = "nvs_host";

constexpr uint32_t kMagic = 0x564E534Fu; /* "OSNV" little-endian */
constexpr uint16_t kVersion = 1;

/* A namespace is small by construction -- persist.cpp stores one blob of at
 * most a few hundred bytes -- so the whole thing is held in memory and
 * rewritten on commit. */
using Store = std::map<std::string, std::vector<uint8_t>>;

std::filesystem::path ns_path(const std::string& name) {
    char dir[1024];
    if (!osynth_host_subdir("nvs", dir, sizeof(dir))) return {};
    return std::filesystem::path(dir) / (name + ".nvs");
}

void rd(const std::vector<uint8_t>& b, size_t& at, void* out, size_t n,
        bool& ok) {
    if (!ok || at + n > b.size()) {
        ok = false;
        return;
    }
    std::memcpy(out, b.data() + at, n);
    at += n;
}

bool load(const std::filesystem::path& p, Store& out) {
    if (p.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return true; /* absent == empty */

    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (f == nullptr) return false;
    std::vector<uint8_t> buf;
    uint8_t chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        buf.insert(buf.end(), chunk, chunk + n);
    }
    std::fclose(f);

    size_t at = 0;
    bool ok = true;
    uint32_t magic = 0;
    uint16_t version = 0, count = 0;
    rd(buf, at, &magic, sizeof(magic), ok);
    rd(buf, at, &version, sizeof(version), ok);
    rd(buf, at, &count, sizeof(count), ok);
    if (!ok || magic != kMagic || version != kVersion) {
        ESP_LOGW(TAG, "%s: not a store this build wrote — ignoring it",
                 p.string().c_str());
        return false;
    }

    for (uint16_t i = 0; i < count && ok; ++i) {
        uint8_t klen = 0;
        rd(buf, at, &klen, sizeof(klen), ok);
        if (!ok) break;
        std::string key(klen, '\0');
        rd(buf, at, key.data(), klen, ok);
        uint32_t vlen = 0;
        rd(buf, at, &vlen, sizeof(vlen), ok);
        if (!ok) break;
        std::vector<uint8_t> val(vlen);
        if (vlen > 0) rd(buf, at, val.data(), vlen, ok);
        if (ok) out.emplace(std::move(key), std::move(val));
    }
    if (!ok) {
        ESP_LOGW(TAG, "%s: truncated — keeping what parsed",
                 p.string().c_str());
    }
    return true;
}

/* Temp file then rename: the atomicity NVS gives for free, and the reason
 * persist.cpp can treat a commit as all-or-nothing. */
bool store(const std::filesystem::path& p, const Store& s) {
    if (p.empty()) return false;
    std::vector<uint8_t> buf;
    auto put = [&buf](const void* d, size_t n) {
        const auto* b = static_cast<const uint8_t*>(d);
        buf.insert(buf.end(), b, b + n);
    };
    const uint16_t count = (uint16_t)s.size();
    put(&kMagic, sizeof(kMagic));
    put(&kVersion, sizeof(kVersion));
    put(&count, sizeof(count));
    for (const auto& kv : s) {
        const uint8_t klen = (uint8_t)kv.first.size();
        const uint32_t vlen = (uint32_t)kv.second.size();
        put(&klen, sizeof(klen));
        put(kv.first.data(), klen);
        put(&vlen, sizeof(vlen));
        if (vlen > 0) put(kv.second.data(), vlen);
    }

    const std::filesystem::path tmp = p.string() + ".tmp";
    FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (f == nullptr) return false;
    const bool wrote =
        buf.empty() || std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }

    /* The same replace the firmware's five writers use (synth_file.h). An
     * earlier version here removed the destination first and renamed into the
     * gap, which works and gives up the atomicity this function exists for. */
    if (synth_replace_file(tmp.string().c_str(), p.string().c_str()) != 0) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

struct nvs_handle_impl {
    std::string name;
    std::filesystem::path path;
    Store data;
    bool writable = false;
    bool dirty = false;
};

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
    if (name == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
    auto* h = new (std::nothrow) nvs_handle_impl();
    if (h == nullptr) return ESP_ERR_NO_MEM;

    h->name = name;
    h->path = ns_path(h->name);
    h->writable = (mode == NVS_READWRITE);
    if (h->path.empty()) { /* no writable data directory */
        delete h;
        return ESP_FAIL;
    }
    (void)load(h->path, h->data); /* absent or unreadable == empty */
    *out = h;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out,
                       size_t* len) {
    if (handle == nullptr || key == nullptr || len == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto it = handle->data.find(key);
    if (it == handle->data.end()) return ESP_ERR_NVS_NOT_FOUND;

    const size_t have = it->second.size();
    if (out == nullptr) { /* size query */
        *len = have;
        return ESP_OK;
    }
    if (*len < have) {
        *len = have;
        return ESP_ERR_INVALID_SIZE;
    }
    if (have > 0) std::memcpy(out, it->second.data(), have);
    *len = have;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value,
                       size_t len) {
    if (handle == nullptr || key == nullptr || (value == nullptr && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->writable) return ESP_ERR_INVALID_STATE;
    const auto* b = static_cast<const uint8_t*>(value);
    handle->data[key].assign(b, b + len);
    handle->dirty = true;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
    if (handle == nullptr || key == nullptr) return ESP_ERR_INVALID_ARG;
    if (!handle->writable) return ESP_ERR_INVALID_STATE;
    if (handle->data.erase(key) == 0) return ESP_ERR_NVS_NOT_FOUND;
    handle->dirty = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    if (handle == nullptr) return ESP_ERR_INVALID_ARG;
    if (!handle->dirty) return ESP_OK;
    if (!store(handle->path, handle->data)) {
        ESP_LOGW(TAG, "commit to %s failed", handle->path.string().c_str());
        return ESP_FAIL;
    }
    handle->dirty = false;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { delete handle; }

esp_err_t nvs_flash_init(void) { return ESP_OK; }

esp_err_t nvs_flash_erase(void) {
    char dir[1024];
    if (!osynth_host_subdir("nvs", dir, sizeof(dir))) return ESP_FAIL;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".nvs") {
            std::filesystem::remove(e.path(), ec);
        }
    }
    return ec ? ESP_FAIL : ESP_OK;
}
