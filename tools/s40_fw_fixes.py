p = 'components/presets/presets.cpp'
s = open(p, encoding='utf-8').read()

# do_state_save -> bool, so a failed write leaves the state dirty
old = """/* `why` only labels the log line. Preset task only — it owns s_pairs, s_ids
 * and the graph staging buffer, and this walks all three. */
void do_state_save(const char* why) {
    /* Never before the restore. A save that ran first would overwrite the
     * stored state with the defaults the synth boots at, which is the one
     * failure this whole mechanism must not have. */
    if (!s_state_ready.load(std::memory_order_relaxed) || !s_fs_ok) return;
"""
new = """/* `why` only labels the log line. Preset task only — it owns s_pairs, s_ids
 * and the graph staging buffer, and this walks all three.
 *
 * False means "wanted to write and could not", which is what puts the dirty
 * flag back: losing an edit to a full filesystem and never trying again would
 * be the same silent data loss the whole file exists to prevent. "Nothing to
 * write" is true — there is nothing to retry. */
bool do_state_save(const char* why) {
    /* Never before the restore. A save that ran first would overwrite the
     * stored state with the defaults the synth boots at, which is the one
     * failure this whole mechanism must not have. */
    if (!s_state_ready.load(std::memory_order_relaxed) || !s_fs_ok) return true;
"""
assert old in s
s = s.replace(old, new, 1)

old = """    if (buf == nullptr) {
        ESP_LOGW(TAG, "state save: no memory for a %u B pattern",
                 (unsigned)cap);
        return;
    }
"""
new = """    if (buf == nullptr) {
        ESP_LOGW(TAG, "state save: no memory for a %u B pattern",
                 (unsigned)cap);
        return false;
    }
"""
assert old in s
s = s.replace(old, new, 1)

old = """    if (!build_state(probe, buf, cap)) {
        ESP_LOGW(TAG, "state save: could not serialise the sequencer");
        free(buf);
        return;
    }
    if (s_state_hash_valid && probe.hash == s_state_hash) {
        free(buf);
        return;
    }
"""
new = """    if (!build_state(probe, buf, cap)) {
        ESP_LOGW(TAG, "state save: could not serialise the sequencer");
        free(buf);
        return false;
    }
    if (s_state_hash_valid && probe.hash == s_state_hash) {
        free(buf);
        return true; /* nothing actually moved */
    }
"""
assert old in s
s = s.replace(old, new, 1)

old = """    FILE* fp = fopen(tmp, "wb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "cannot create %s", tmp);
        free(buf);
        return;
    }
    StateSink w = {fp, kHashSeed, 0, true};"""
new = """    FILE* fp = fopen(tmp, "wb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "cannot create %s", tmp);
        free(buf);
        return false;
    }
    StateSink w = {fp, kHashSeed, 0, true};"""
assert old in s
s = s.replace(old, new, 1)

old = """    if (!built || !w.ok || rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "state save failed (filesystem full?)");
        remove(tmp);
        return;
    }"""
new = """    if (!built || !w.ok || rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "state save failed (filesystem full?)");
        remove(tmp);
        return false;
    }"""
assert old in s
s = s.replace(old, new, 1)

old = """    ESP_LOGI(TAG, "state saved: %u B [%s]", (unsigned)w.bytes, why);
}"""
new = """    ESP_LOGI(TAG, "state saved: %u B [%s]", (unsigned)w.bytes, why);
    return true;
}"""
assert old in s
s = s.replace(old, new, 1)

old = """    /* Cleared before the write, so a change that lands during it leaves the
     * state dirty again rather than being swallowed. */
    s_state_dirty.store(false, std::memory_order_relaxed);
    do_state_save(quiet ? "quiet" : "overdue");
    settled_ms = 0;
    waiting_ms = 0;
}"""
new = """    /* Cleared before the write, so a change that lands during it leaves the
     * state dirty again rather than being swallowed — and put back when the
     * write itself failed, so a transient full filesystem costs a retry rather
     * than the session. */
    s_state_dirty.store(false, std::memory_order_relaxed);
    if (!do_state_save(quiet ? "quiet" : "overdue")) {
        s_state_dirty.store(true, std::memory_order_relaxed);
    }
    settled_ms = 0;
    waiting_ms = 0;
}"""
assert old in s
s = s.replace(old, new, 1)

old = """            case OP_STATE_SAVE:
                do_state_save("forced");
                if (s_state_done != nullptr) xSemaphoreGive(s_state_done);
                break;"""
new = """            case OP_STATE_SAVE:
                (void)do_state_save("forced");
                if (s_state_done != nullptr) xSemaphoreGive(s_state_done);
                break;"""
assert old in s
s = s.replace(old, new, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)

# ---- main.cpp: never fail to boot for the restore ------------------------
p = 'main/main.cpp'
s = open(p, encoding='utf-8').read()
old = """    ESP_ERROR_CHECK(presets_state_restore());"""
new = """    /* Not ESP_ERROR_CHECKed. The only way this fails is a full request queue,
     * and a synth that refuses to boot because it could not arrange to restore
     * a patch is worse than one that boots at its defaults — the same
     * sink-fallback rule persist_init() follows. */
    const esp_err_t rerr = presets_state_restore();
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "working state not restored: %s (starting at defaults)",
                 esp_err_to_name(rerr));
    }"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
