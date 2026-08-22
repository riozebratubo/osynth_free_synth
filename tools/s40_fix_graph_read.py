p = 'components/presets/presets.cpp'
s = open(p, encoding='utf-8').read()

old = """        s_graph_len = 0;
        if (h.graph_len > 0) {
#if SYNTH_ENABLE_MODULAR
            s_graph_len = fread(s_graph_blob, 1, h.graph_len, fp);
            if (s_graph_len != h.graph_len) {
                fclose(fp);
                ESP_LOGW(TAG, "state: truncated graph, ignoring");
                break;
            }
#else
            /* A file written by a build with the modular engine, read by one
             * without it: step over the blob rather than refuse the state. */
            if (fseek(fp, (long)h.graph_len, SEEK_CUR) != 0) {
                fclose(fp);
                ESP_LOGW(TAG, "state: truncated graph, ignoring");
                break;
            }
#endif
        }
"""
new = """        /* The blob is taken only when this build can hold it: a firmware
         * without the modular engine, or one with fewer node slots than the
         * firmware that wrote the file, steps over it and restores everything
         * else. Refusing the whole state because one section is for a bigger
         * build would throw away the patch and the sequencer with it — the
         * same rule the oversized-pattern branch below follows. */
        s_graph_len = 0;
        bool take_graph = false;
#if SYNTH_ENABLE_MODULAR
        take_graph = h.graph_len > 0 && h.graph_len <= sizeof(s_graph_blob);
#endif
        if (take_graph) {
#if SYNTH_ENABLE_MODULAR
            s_graph_len = fread(s_graph_blob, 1, h.graph_len, fp);
            if (s_graph_len != h.graph_len) {
                fclose(fp);
                ESP_LOGW(TAG, "state: truncated graph, ignoring");
                break;
            }
#endif
        } else if (h.graph_len > 0) {
            if (fseek(fp, (long)h.graph_len, SEEK_CUR) != 0) {
                fclose(fp);
                ESP_LOGW(TAG, "state: truncated graph, ignoring");
                break;
            }
        }
"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
