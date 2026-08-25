"""S44 app-side wiring: kit-info parsing, kit-edit invokables, pad release.

Kept per the intermediary-artifacts policy. Idempotent via a marker check.
"""
import io

H = 'app_osyntho/src/synthcontroller.h'
C = 'app_osyntho/src/synthcontroller.cpp'

h = io.open(H, encoding='utf-8').read()
c = io.open(C, encoding='utf-8').read()
if 'kitStorage' in h:
    raise SystemExit('already patched')

# ---------------------------------------------------------------- header ----
old = "  Q_PROPERTY(int currentKit READ currentKit NOTIFY kitChanged FINAL)"
new = """  Q_PROPERTY(int currentKit READ currentKit NOTIFY kitChanged FINAL)
  // Where the firmware is persisting recordable kits: "sd", "lfs" or "none"
  // (S44). The page shows it, and it is what decides whether a Save control is
  // offered at all -- a button that provably cannot work is worse than one
  // that is not there.
  Q_PROPERTY(QString kitStorage READ kitStorage NOTIFY kitChanged FINAL)"""
assert old in h, 'h: currentKit property'
h = h.replace(old, new, 1)

old = "  int currentKit() const { return m_currentKit; }"
new = """  int currentKit() const { return m_currentKit; }
  QString kitStorage() const { return m_kitStorage; }"""
assert old in h, 'h: currentKit getter'
h = h.replace(old, new, 1)

old = "  Q_INVOKABLE void triggerDrum(int slot, int velocity = 100);"
new = """  Q_INVOKABLE void triggerDrum(int slot, int velocity = 100);
  // Let go of a pad. Only gate and loop pads (S44) hold anything, so this is a
  // no-op on every other kind and the pad surfaces can send it on every
  // touch-up without first asking what the pad is.
  Q_INVOKABLE void releaseDrum(int slot);

  // ---- sample-kit editing (S44) ----
  //
  // These write kit data rather than parameters, so they do not go through
  // setParam's coalescing batch and they do not appear in a preset. `field`
  // is KitPadField in synthprotocol.h. Each is followed by a kit re-read,
  // because the firmware is the authority on what a pad ended up as -- it
  // clamps, and it refuses outright on the factory kit.
  Q_INVOKABLE void setPadField(int slot, int field, double value);
  Q_INVOKABLE void renameKit(int kit, const QString& name);
  Q_INVOKABLE void renamePad(int slot, const QString& name);"""
assert old in h, 'h: triggerDrum'
h = h.replace(old, new, 1)

old = "  int m_currentKit = 0;"
new = """  int m_currentKit = 0;
  QString m_kitStorage = QStringLiteral("none");"""
assert old in h, 'h: m_currentKit'
h = h.replace(old, new, 1)

io.open(H, 'w', encoding='utf-8').write(h)

# ------------------------------------------------------------------- cpp ----
old = """void SynthController::handleKitInfo(const QByteArray& payload, bool more) {
  Reader r(payload);
  const quint8 what = r.u8();
  if (what == 0) {
    m_currentKit = r.u8();
    r.u8();  // count; the records are authoritative
    while (r.remaining >= 1 + 24) {
      QVariantMap m;
      m["index"] = r.u8();
      QByteArray raw(reinterpret_cast<const char*>(r.p), 24);
      r.p += 24;
      r.remaining -= 24;
      const int nul = raw.indexOf('\\0');
      if (nul >= 0) raw.truncate(nul);
      m["name"] = QString::fromUtf8(raw);
      m_kitsAccum.append(m);
    }
    if (more) return;
    m_kits = m_kitsAccum;
    m_kitsAccum.clear();
    emit kitChanged();
    return;
  }

  r.u8();  // slot count; the records are authoritative
  r.u8();  // reserved
  while (r.remaining >= 2 + 12) {
    QVariantMap m;
    m["slot"] = r.u8();
    m["note"] = r.u8();
    QByteArray raw(reinterpret_cast<const char*>(r.p), 12);
    r.p += 12;
    r.remaining -= 12;
    const int nul = raw.indexOf('\\0');
    if (nul >= 0) raw.truncate(nul);
    m["name"] = QString::fromUtf8(raw);
    m_kitSlotsAccum.append(m);
  }
  if (more) return;
  m_kitSlots = m_kitSlotsAccum;
  m_kitSlotsAccum.clear();
  emit kitChanged();
}"""

new = '''// A fixed-width NUL-padded name field, which is how every string on this link
// travels.
static QString readName(Reader& r, int width) {
  if (r.remaining < width) return QString();
  QByteArray raw(reinterpret_cast<const char*>(r.p), width);
  r.p += width;
  r.remaining -= width;
  const int nul = raw.indexOf('\\0');
  if (nul >= 0) raw.truncate(nul);
  return QString::fromUtf8(raw);
}

void SynthController::handleKitInfo(const QByteArray& payload, bool more) {
  Reader r(payload);
  const quint8 what = r.u8();
  if (what == 0) {
    m_currentKit = r.u8();
    r.u8();  // count; the records are authoritative

    // S44 widened this listing: a fourth prefix byte for the storage backend,
    // and a flags byte on every record. Rather than assume, work out which
    // shape arrived -- the two sizes are distinguishable and a mismatched
    // firmware is a real thing to be handed. Old: N * 25 bytes exactly. New:
    // 1 + N * 26. Nothing satisfies both for any N that fits a frame.
    const bool wide = r.remaining >= 1 && ((r.remaining - 1) % 26) == 0;
    if (wide) {
      const quint8 backend = r.u8();
      m_kitStorage = backend == 1 ? QStringLiteral("sd")
                                  : (backend == 2 ? QStringLiteral("lfs")
                                                  : QStringLiteral("none"));
    } else {
      m_kitStorage = QStringLiteral("none");
    }
    while (r.remaining >= (wide ? 26 : 25)) {
      QVariantMap m;
      m["index"] = r.u8();
      // Bit 0: the kit can be recorded into and saved. The factory kit cannot,
      // and the page it gets is a different page.
      m["user"] = wide ? ((r.u8() & 0x01) != 0) : false;
      m["name"] = readName(r, 24);
      m_kitsAccum.append(m);
    }
    if (more) return;
    m_kits = m_kitsAccum;
    m_kitsAccum.clear();
    emit kitChanged();
    return;
  }

  r.u8();  // slot count; the records are authoritative
  // Record width, sent by the firmware since S44 so this loop does not have to
  // know it. 0 is a pre-S44 firmware, whose records were 14 bytes.
  const int stride = r.u8();
  const int width = stride > 0 ? stride : 14;
  while (r.remaining >= width) {
    const int before = r.remaining;
    QVariantMap m;
    m["slot"] = r.u8();
    m["note"] = r.u8();
    if (width >= 22) {
      const quint8 flags = r.u8();
      m["filled"] = (flags & 0x01) != 0;
      m["reverse"] = (flags & 0x02) != 0;
      m["mode"] = int(r.u8());
      m["choke"] = int(r.u8());
      m["start"] = double(r.u8()) / 255.0;
      const quint32 frames = r.u32();
      m["frames"] = int(frames);
      m["seconds"] = double(frames) / 48000.0;
    } else {
      // Pre-S44: a named slot is a filled one, which was the only signal the
      // old listing carried.
      m["filled"] = false;
      m["reverse"] = false;
      m["mode"] = 0;
      m["choke"] = 0;
      m["start"] = 0.0;
      m["frames"] = 0;
      m["seconds"] = 0.0;
    }
    m["name"] = readName(r, 12);
    if (width < 22) m["filled"] = !m["name"].toString().isEmpty();
    // Skip any bytes a newer firmware appended that this build does not know
    // about, so one unknown field cannot shift the whole listing.
    const int used = before - r.remaining;
    if (used < width) {
      const int skip = qMin(width - used, r.remaining);
      r.p += skip;
      r.remaining -= skip;
    }
    m_kitSlotsAccum.append(m);
  }
  if (more) return;
  m_kitSlots = m_kitSlotsAccum;
  m_kitSlotsAccum.clear();
  emit kitChanged();
}

/* ------------------------------------------------- sample-kit editing (S44) */

void SynthController::releaseDrum(int slot) {
  if (slot < 0 || !m_connected) return;
  // Only meaningful on a firmware with the three-byte trigger form; an older
  // one answers ST_MALFORMED, which is harmless and happens once.
  if (m_drumTrigOpcode) send(OP_DRUM_TRIG, payloadDrumRelease(slot), false);
}

void SynthController::setPadField(int slot, int field, double value) {
  if (slot < 0 || !m_connected) return;
  send(OP_KIT_EDIT, payloadKitPadField(-1, slot, field, float(value)), true);
  refreshKit();
}

void SynthController::renameKit(int kit, const QString& name) {
  if (kit < 0 || !m_connected) return;
  send(OP_KIT_EDIT, payloadKitRename(kit, name), true);
  refreshKit();
}

void SynthController::renamePad(int slot, const QString& name) {
  if (slot < 0 || !m_connected) return;
  send(OP_KIT_EDIT, payloadKitPadRename(-1, slot, name), true);
  refreshKit();
}'''
assert old in c, 'c: handleKitInfo'
c = c.replace(old, new, 1)

# The reset path has to clear the new field too.
old = "  m_kitSlots.clear();\n  m_kitSlotsAccum.clear();"
new = "  m_kitSlots.clear();\n  m_kitSlotsAccum.clear();\n  m_kitStorage = QStringLiteral(\"none\");"
assert old in c, 'c: reset'
c = c.replace(old, new, 1)

io.open(C, 'w', encoding='utf-8').write(c)
print('ok')
