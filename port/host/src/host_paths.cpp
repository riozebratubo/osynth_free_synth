/*
 * osynth host port — the data root. See host_paths.h for the contract.
 */
#include "host_paths.h"

#include "esp_log.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace {

const char* TAG = "host_paths";

std::mutex g_mutex;
std::string g_override;
std::string g_resolved;

std::string env(const char* name) {
#if defined(_MSC_VER)
    /* getenv() is deprecated under MSVC's secure-CRT warnings, and
     * _dupenv_s is the sanctioned replacement. */
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return {};
    std::string v(buf);
    std::free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

std::string platform_default() {
#if defined(_WIN32)
    std::string base = env("LOCALAPPDATA");
    if (base.empty()) base = env("APPDATA");
#elif defined(__ANDROID__)
    /* There is no default worth guessing here, and pretending otherwise is
     * worse than saying so.
     *
     * An Android app may only write inside the directory the system assigns
     * it, and that path is not in the environment: it comes from the Java
     * side, which is why osynth_host_config_t has a data_dir field and why
     * EmbeddedManager fills it from QStandardPaths. Without it the branch
     * below would find no XDG_DATA_HOME and no HOME, fall through to a
     * relative path, and try to create it under the process's working
     * directory -- which is "/" and is not writable. The result is storage
     * that silently does nothing, reported as a permissions error somewhere
     * far from the cause.
     *
     * Empty, so the caller is told plainly instead. */
    ESP_LOGE(TAG,
             "no data directory set: on Android the app must pass one "
             "(osynth_host_config_t::data_dir) -- storage is disabled");
    /* Declared even though this branch returns before using it: the shared
     * tail below the #endif reads `base`, so every branch has to leave one in
     * scope for that code to compile at all. */
    std::string base;
    return {};
#elif defined(__APPLE__)
    /* $HOME is the app's own sandbox on iOS and the user's home on macOS, and
     * "Library/Application Support" under it is right for both. */
    std::string base = env("HOME");
    if (!base.empty()) base += "/Library/Application Support";
#else
    std::string base = env("XDG_DATA_HOME");
    if (base.empty()) {
        const std::string home = env("HOME");
        if (!home.empty()) base = home + "/.local/share";
    }
#endif
    /* Last resort: the working directory. Not a good place for user data, but
     * better than failing -- and it is what a build running in a sandbox with
     * no environment at all will get. */
    if (base.empty()) return "osynth";
    /* Appended through std::filesystem rather than concatenated with a '/', so
     * the result uses one separator throughout. Concatenating produced a path
     * mixing both -- which every API here accepts, and which looks like a bug
     * in every log line it appears in. */
    return (std::filesystem::path(base) / "osynth").string();
}

const std::string& resolve() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_resolved.empty()) {
        g_resolved = g_override.empty() ? platform_default() : g_override;
    }
    return g_resolved;
}

}  // namespace

void osynth_host_set_data_dir(const char* path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_override = (path != nullptr) ? path : "";
    g_resolved.clear(); /* re-resolved on next use */
}

const char* osynth_host_data_dir(void) { return resolve().c_str(); }

bool osynth_host_subdir(const char* name, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return false;
    out[0] = '\0';

    std::filesystem::path p(resolve());
    if (name != nullptr && name[0] != '\0') p /= name;

    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    /* create_directories reports "already existed" as no error with ec unset
     * but a false return, so the directory's existence is what is checked --
     * not the return value. */
    if (!std::filesystem::is_directory(p, ec)) {
        ESP_LOGW(TAG, "cannot create %s (%s)", p.string().c_str(),
                 ec ? ec.message().c_str() : "unknown");
        return false;
    }

    const std::string s = p.string();
    if (s.size() + 1 > out_len) {
        ESP_LOGW(TAG, "path too long for caller's buffer: %s", s.c_str());
        return false;
    }
    std::memcpy(out, s.c_str(), s.size() + 1);
    return true;
}
