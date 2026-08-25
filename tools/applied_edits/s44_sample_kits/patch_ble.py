"""Splice the S44 sample-kit opcode into components/ble_ctrl/ble_ctrl.cpp.

Kept per the project's intermediary-artifacts policy. Idempotent: it refuses
to run twice by checking for OP_KIT_EDIT first.
"""
import io

NUL = "'" + chr(92) + "0'"
P = 'components/ble_ctrl/ble_ctrl.cpp'

s = io.open(P, encoding='utf-8').read()
if 'OP_KIT_EDIT' in s:
    raise SystemExit('already patched')

# ---- 1. the opcode ------------------------------------------------------
old = """    OP_CHORD_SET = 0x3E,
    OP_PING = 0x7F,"""
new = """    OP_CHORD_SET = 0x3E,
    /* Sample-kit editing (S44). Everything the *recorder* does rides on
     * ordinary parameters (smp.arm, smp.rec, smp.erase, smp.undo), because a
     * float is all any of it needs and the app already has plumbing for those.
     * What could not go there is the per-pad performance data - play mode,
     * reverse, start offset, choke group, note, name - which is kit data
     * rather than patch data: it has to follow a kit switch, and a parameter
     * does not. There is also no room for it, with 448 parameter slots and
     * ~418 in use. So it travels here, the same reasoning the sequencer's
     * pattern data and the graph's structure both took. */
    OP_KIT_EDIT = 0x3F,
    OP_PING = 0x7F,"""
assert old in s
s = s.replace(old, new, 1)

# ---- 2. handle_kit_info, replaced whole (spliced by line, because the
#         original carries a U+2026 that is easy to get wrong by hand) ------
body = """/* Wire width of one `what = 1` slot record. Sent in the prefix rather than
 * assumed by the app, which is the lesson S44 taught this opcode: the record
 * grew from 14 bytes to 22, and an app with the old width hard-coded would
 * have read the whole listing shifted rather than noticing. A reader that
 * takes the width from the prefix survives the next addition too. */
constexpr uint8_t kKitSlotRecBytes = 6 + 4 + DRUM_SLOT_NAME_MAX;

void handle_kit_info(uint8_t seq, const uint8_t* p, uint16_t plen) {
    const uint8_t what = plen >= 1 ? p[0] : 1;
    if (what == 0) { /* the selectable kits */
        const uint8_t prefix[3] = {0, (uint8_t)ParamStore::instance().get(
                                          DRUM_PID_KIT),
                                   (uint8_t)drums_kit_count()};
        s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false,
                        /*paced=*/true);
        for (int i = 0; i < drums_kit_count(); ++i) {
            uint8_t rec[2 + DRUM_KIT_NAME_MAX] = {};
            rec[0] = (uint8_t)i;
            /* Bit 0 says the kit can be recorded into and saved; the app draws
             * a very different page for one that cannot. */
            rec[1] = (uint8_t)(drums_kit_is_user(i) ? 0x01u : 0x00u);
            /* rec is zero-initialised, so the name stays NUL-padded to its
             * fixed wire width; strlcpy always terminates. */
            strlcpy((char*)rec + 2, drums_kit_name_at(i), DRUM_KIT_NAME_MAX);
            s_chunker.append(rec, sizeof(rec));
        }
        s_chunker.finish();
        return;
    }
    /* The current kit's slots: what the app labels the drum lanes and the
     * mixer strips with, since the parameters themselves are named
     * generically (drum1.level and friends) and outlive any one kit.
     *
     * Since S44 it also carries what a *pad editor* needs - whether the pad
     * has anything in it, how long that is, and the four performance settings
     * that live in the kit rather than in parameter space. All of it in the
     * listing the app already fetches on every kit change, rather than a
     * per-pad round trip sixteen times over. */
    const uint8_t prefix[3] = {1, (uint8_t)drums_slot_count(),
                               kKitSlotRecBytes};
    s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false,
                    /*paced=*/true);
    for (int i = 0; i < drums_slot_count() && i < DRUM_SLOTS; ++i) {
        uint8_t rec[kKitSlotRecBytes] = {};
        drums_pad_t pad;
        const bool filled = drums_pad_get(i, &pad);
        rec[0] = (uint8_t)i;
        const int note = drums_slot_note(i);
        rec[1] = (uint8_t)(note >= 0 ? note : (36 + i));
        rec[2] = (uint8_t)((filled ? 0x01u : 0x00u) |
                           (filled && pad.reverse ? 0x02u : 0x00u));
        rec[3] = filled ? pad.play_mode : (uint8_t)DRUM_PLAY_ONESHOT;
        rec[4] = filled ? pad.choke_group : 0u;
        /* start_ofs as a byte: the app draws it on a slider a couple of
         * hundred pixels long, so 1/255 is already finer than anyone can aim
         * at, and it keeps the record byte-aligned. */
        rec[5] = filled ? (uint8_t)(pad.start_ofs * 255.0f + 0.5f) : 0u;
        const uint32_t frames = filled ? pad.frames : 0u;
        memcpy(rec + 6, &frames, 4);
        strlcpy((char*)rec + 10, drums_slot_name(i), DRUM_SLOT_NAME_MAX);
        s_chunker.append(rec, sizeof(rec));
    }
    s_chunker.finish();
}

/* ---- sample-kit editing (S44) ------------------------------------------
 *
 * One opcode, a sub-op byte, and a status reply. Deliberately not chunked:
 * each of these is a single small write, and the app re-reads KIT_INFO
 * afterwards to see the result rather than being told it twice.
 *
 *   0 PAD FIELD  [u8 kit][u8 slot][u8 field][f32 value]
 *       `kit` 0xFF means the bound one; `field` is drums_pad_field_t.
 *   1 RENAME KIT [u8 kit][char name[DRUM_KIT_NAME_MAX]]
 *   2 RENAME PAD [u8 kit][u8 slot][char name[DRUM_SLOT_NAME_MAX]]
 */
void handle_kit_edit(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_KIT_EDIT, seq, ST_MALFORMED);
        return;
    }
    const uint8_t sub = p[0];
    if (sub == 0) {
        if (plen < 8) {
            send_status(OP_KIT_EDIT, seq, ST_MALFORMED);
            return;
        }
        const int kit = (p[1] == 0xFF) ? -1 : (int)p[1];
        float value;
        memcpy(&value, p + 4, 4);
        const esp_err_t err = drums_pad_set_field(
            kit, (int)p[2], (drums_pad_field_t)p[3], value);
        send_status(OP_KIT_EDIT, seq,
                    err == ESP_OK
                        ? ST_OK
                        : (err == ESP_ERR_NOT_SUPPORTED ? ST_UNSUPPORTED
                                                        : ST_BAD_ARG));
        return;
    }
    if (sub == 1) {
        if (plen < 3) {
            send_status(OP_KIT_EDIT, seq, ST_MALFORMED);
            return;
        }
        char name[DRUM_KIT_NAME_MAX];
        const uint16_t avail = (uint16_t)(plen - 2);
        const uint16_t n =
            avail < DRUM_KIT_NAME_MAX - 1 ? avail : DRUM_KIT_NAME_MAX - 1;
        memcpy(name, p + 2, n);
        name[n] = NULCHAR;
        const esp_err_t err = drums_kit_rename((int)p[1], name);
        send_status(OP_KIT_EDIT, seq, err == ESP_OK ? ST_OK : ST_UNSUPPORTED);
        return;
    }
    if (sub == 2) {
        if (plen < 4) {
            send_status(OP_KIT_EDIT, seq, ST_MALFORMED);
            return;
        }
        char name[DRUM_SLOT_NAME_MAX];
        const uint16_t avail = (uint16_t)(plen - 3);
        const uint16_t n =
            avail < DRUM_SLOT_NAME_MAX - 1 ? avail : DRUM_SLOT_NAME_MAX - 1;
        memcpy(name, p + 3, n);
        name[n] = NULCHAR;
        const esp_err_t err =
            drums_pad_rename((p[1] == 0xFF) ? -1 : (int)p[1], (int)p[2], name);
        send_status(OP_KIT_EDIT, seq, err == ESP_OK ? ST_OK : ST_BAD_ARG);
        return;
    }
    send_status(OP_KIT_EDIT, seq, ST_BAD_ARG);
}""".replace('NULCHAR', NUL)

lines = s.split('\n')
i0 = next(i for i, l in enumerate(lines) if l.startswith('void handle_kit_info'))
i1 = next(i for i in range(i0, len(lines)) if lines[i] == '}')
s = '\n'.join(lines[:i0]) + '\n' + body + '\n' + '\n'.join(lines[i1 + 1:])

# ---- 3. dispatch --------------------------------------------------------
old = """        case OP_KIT_INFO:
            handle_kit_info(seq, p, plen);"""
new = """        case OP_KIT_EDIT:
            handle_kit_edit(seq, p, plen);
            break;
        case OP_KIT_INFO:
            handle_kit_info(seq, p, plen);"""
assert old in s
s = s.replace(old, new, 1)

io.open(P, 'w', encoding='utf-8').write(s)
print('ok')
