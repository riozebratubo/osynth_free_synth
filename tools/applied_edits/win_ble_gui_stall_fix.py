#!/usr/bin/env python3
"""Applied edits: the Windows/SimpleBLE "app hangs after discovery" fix.

Kept per the project's intermediary-artifacts policy. These are the nine
scripted string replacements that were actually run against
app_osyntho/src/bluetoothmanager2.{cpp,h}; they are recorded here as-is, in
order, so the change can be re-read as a sequence of edits rather than a diff.
They are NOT idempotent and will fail their asserts against an already-patched
tree -- that is deliberate, it is what makes them safe to re-run by accident.

What they fix (see the comments they install for the full reasoning):

  1-2  scanAndConnect() / connectToSelectedDevice() / startDeviceScan() no
       longer call into SimpleBLE from the GUI thread while a BLE worker is
       alive. On Windows every SimpleBLE entry point is a blocking hop onto
       one process-wide MTA thread (simpleble/.../windows/MtaManager.h), which
       also carries the worker's 2 s scan_for(), the connect + service
       discovery, and every CTRL write. The 2 s rescan timer therefore froze
       the UI for however long the worker was busy: seconds on a reconnect,
       ten on a write into a link that had stopped answering.
  3    Header: un-parks m_peripheralDropped and adds m_writeFailures.
  4,6  writeBlocking() counts consecutive failures and declares the link lost
       after two, instead of letting Windows take half a minute to notice
       while each stale frame costs a ten-second async_get timeout.
  7    startDeviceScan() uses the cached connection flag.
  8    drainWriteQueue() bails on m_peripheralDropped too, closing the 100 ms
       window between "link declared lost" and "keep-alive loop published it".
  9    Both flags are armed before the connection is announced, not after.

Verified afterwards with:
    python tools/check_mojibake.py       app_osyntho/src/bluetoothmanager2.cpp app_osyntho/src/bluetoothmanager2.h
    python tools/c_brace_check.py        app_osyntho/src/bluetoothmanager2.cpp app_osyntho/src/bluetoothmanager2.h
    python tools/check_comment_blocks.py app_osyntho/src/bluetoothmanager2.cpp app_osyntho/src/bluetoothmanager2.h
"""

import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.cpp"
s = io.open(p, encoding="utf-8", newline="").read()

old = """  if (not SimpleBLE::Adapter::bluetooth_enabled()) {
    qDebug() << "Bt | Scan | Will not scan and connect, bluetooth is not enabled!";
    return;
  }

  // A single worker is alive for its whole lifetime — scanning, connecting AND
  // while connected (the connectAndSubscribe keep-alive loop). scanAndConnect()
  // is only ever called on the main thread, so this makes spawning a second
  // concurrent worker structurally impossible.
  if (m_bluetoothThread.isRunning()) return;

  auto adapter = adapterHandle();
  if (adapter.initialized() and adapter.scan_is_active()) return;
  if (m_adapterIsConnecting) return;

  auto connectedPeripheral = peripheralHandle();
  if (connectedPeripheral.initialized() and connectedPeripheral.is_connected()) return;
"""

new = """  // Everything below this point that talks to SimpleBLE does so from the GUI
  // thread, and on Windows *every* SimpleBLE entry point — including the two
  // that look like plain getters, bluetooth_enabled() and is_connected() — is
  // a blocking hop onto one process-wide MTA thread (see MtaManager). That one
  // thread also carries the BLE worker's 2 s scan_for(), the whole connect +
  // service-discovery sequence, and every CTRL write, and it runs them
  // strictly one at a time. A GUI-thread call therefore does not cost a WinRT
  // round trip; it costs whatever the worker happens to be in the middle of —
  // which is seconds on a reconnect, and up to ten on a write into a link that
  // has stopped answering (async_get's timeout). The rescan timer fires this
  // function every 2 s for the whole session, so that was a guaranteed
  // multi-second UI freeze on every reconnect: the app "hangs after
  // discovery".
  //
  // So: the cheap cached tests first, and they are enough. A worker is alive
  // for its whole lifetime — scanning, connecting AND while connected (the
  // connectAndSubscribe keep-alive loop) — so while one is running there is
  // nothing here to do anyway, and the GUI thread makes no SimpleBLE call at
  // all. scanAndConnect() is only ever called on the main thread, so this also
  // makes spawning a second concurrent worker structurally impossible.
  if (m_bluetoothThread.isRunning()) return;
  if (m_adapterIsConnecting) return;
  // Replaces a peripheral.is_connected() probe that used to sit here. Same
  // answer from the cache the worker publishes, without the hop — and by this
  // line there is no worker running, so a live connection cannot exist.
  if (m_connectedCache.load()) return;

  if (not SimpleBLE::Adapter::bluetooth_enabled()) {
    qDebug() << "Bt | Scan | Will not scan and connect, bluetooth is not enabled!";
    return;
  }

  auto adapter = adapterHandle();
  if (adapter.initialized() and adapter.scan_is_active()) return;
"""

assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.cpp"
s = io.open(p, encoding="utf-8", newline="").read()

old = """  peripheral.set_callback_on_disconnected(
      []() { qDebug() << "Bt | Peripheral disconnected (callback)"; });

  // PARKED — the desktop counterpart of the Android link-loss fix
  // (bluetoothmanager.cpp: handleLinkLost). Only Android was reproduced and
  // retested, so this backend is left as it was rather than changed blind.
  //
  // The concern it addresses is real here too: the callback above only logs, so
  // the keep-alive loop below relies on peripheral.is_connected() alone — and
  // switching the adapter off mid-session can leave the Windows backend
  // answering a cached `true`. The worker then parks forever, the app never
  // publishes connectedChanged(false), and SynthController::setConnected(true)
  // on the way back early-returns on the unchanged value, skipping every reset
  // and reusing stale discovery state for the session.
  //
  // To enable: un-comment m_peripheralDropped in the header, arm it here
  // *before* installing the callback (so a disconnect in the gap is not
  // swallowed), set it from the callback, and un-comment the check in the loop.
  //
  // m_peripheralDropped.store(false);
  // peripheral.set_callback_on_disconnected(this]() {
  //   qDebug() << "Bt | Peripheral disconnected (callback)";
  //   m_peripheralDropped.store(true);
  // });

  // Keep-alive: the worker parks here while connected, polling for a disconnect
  // or a stop request (startDeviceScan/finish block on this poll).
  while (true) {
    bool stillConnected = false;
    try {
      stillConnected = peripheral.is_connected();
    } catch (...) {
      stillConnected = false;
    }

    // PARKED with the callback above — see that note to enable.
    // if (m_peripheralDropped.load()) stillConnected = false;

    if (not stillConnected or m_shouldStopBluetoothThread) {
      qDebug() << "Bt | Thread will quit";
      setPeripheralHandle(SimpleBLE::Peripheral{});
      try {
        peripheral.unsubscribe(serviceUuidStr, evtUuidStr);
      } catch (...) {
      }
      try {
        peripheral.disconnect();
      } catch (...) {
      }
      setAdapterHandle(SimpleBLE::Adapter{});

      publishConnectionState(false, QString(), QString(), 0);
      emitOnGuiThread([this]() {
        emit connectedChanged(false);
        emit deviceNameChanged();
        emit deviceAddressChanged();
      });
      return true;
    }

    QThread::msleep(100);
  }
}"""

# The parked block in the file uses a slightly different lambda text; rebuild from file.
if s.count(old) != 1:
    old = old.replace("set_callback_on_disconnected(this]()", "set_callback_on_disconnected([this]()")
assert s.count(old) == 1, ("marker not found", s.count(old))

new = """  // Armed before the callback is installed, so a disconnect landing in the gap
  // between the two is not swallowed.
  m_peripheralDropped.store(false);
  m_writeFailures.store(0);
  peripheral.set_callback_on_disconnected([this]() {
    qDebug() << "Bt | Peripheral disconnected (callback)";
    m_peripheralDropped.store(true);
  });

  // Keep-alive: the worker parks here while connected, polling for a disconnect
  // or a stop request (startDeviceScan/finish block on this poll).
  while (true) {
    bool stillConnected = false;

    // The flag first, and if it is set the poll is skipped entirely. Two
    // reasons, and the ordering matters for both.
    //
    // Correctness: peripheral.is_connected() is not reliable on every backend —
    // with the adapter switched off mid-session the Windows one can keep
    // reporting a cached `true`, so the loop would park forever, the app would
    // never publish connectedChanged(false), and SynthController::setConnected(true)
    // on the way back would early-return on the unchanged value, skipping every
    // reset and reusing stale discovery state for the session. This is the
    // desktop counterpart of the Android link-loss fix (bluetoothmanager.cpp:
    // handleLinkLost); the callback fires in that case, so it is authoritative.
    //
    // Cost: is_connected() is a blocking hop onto the one MTA thread, which is
    // also where writeBlocking() sits. A write into a link that has stopped
    // answering takes ten seconds to time out there (async_get), so a poll
    // queued behind one is a poll that learns nothing for ten seconds — on
    // exactly the link that has already been declared dead. Reading the flag
    // costs nothing and is not queued behind anything.
    if (m_peripheralDropped.load()) {
      stillConnected = false;
    } else {
      try {
        stillConnected = peripheral.is_connected();
      } catch (...) {
        stillConnected = false;
      }
    }

    if (not stillConnected or m_shouldStopBluetoothThread) {
      qDebug() << "Bt | Thread will quit";
      setPeripheralHandle(SimpleBLE::Peripheral{});

      // Published *before* the SimpleBLE teardown, not after. unsubscribe() and
      // disconnect() are two more MTA calls, and on a link that has stopped
      // answering they queue behind whatever writes are still draining — each
      // of which costs its own ten-second timeout. Announcing the disconnect
      // first makes drainWriteQueue() throw the queued frames away on its next
      // step (it bails on the cached state) and stops SynthController handing
      // it new ones, so the teardown below is not paid for one stale frame at a
      // time. The app's view of the link is already correct at this point: it
      // is gone, whether or not Windows has finished agreeing.
      publishConnectionState(false, QString(), QString(), 0);
      emitOnGuiThread([this]() {
        emit connectedChanged(false);
        emit deviceNameChanged();
        emit deviceAddressChanged();
      });

      try {
        peripheral.unsubscribe(serviceUuidStr, evtUuidStr);
      } catch (...) {
      }
      try {
        peripheral.disconnect();
      } catch (...) {
      }
      setAdapterHandle(SimpleBLE::Adapter{});
      return true;
    }

    QThread::msleep(100);
  }
}"""

s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.h"
s = io.open(p, encoding="utf-8", newline="").read()

old = """  // Cached connection state, so the QML accessors never call into SimpleBLE.
  // The strings ride m_handleMutex with the handles they describe.
  // PARKED \u2014 desktop/SimpleBLE counterpart of the Android link-loss fix in
  // bluetoothmanager.cpp (handleLinkLost). Not enabled because the bug was
  // only reproduced on Android and this backend was not retested; keep it
  // together with the two commented blocks in bluetoothmanager2.cpp if the
  // desktop build ever shows the same "still connected after the adapter is
  // switched off" symptom.
  //
  // Set by SimpleBLE's on_disconnected callback, cleared before each keep-alive
  // loop. The loop's own peripheral.is_connected() poll is not reliable on every
  // backend \u2014 with the adapter switched off mid-session the Windows one can keep
  // reporting a cached `true`, so the loop parks forever and the app never
  // publishes connectedChanged(false). The callback does fire in that case, so
  // it is the authoritative signal; it just cannot do the teardown itself
  // (wrong thread), hence a flag the loop reads.
  // std::atomic<bool> m_peripheralDropped{false};
  std::atomic<bool> m_connectedCache{false};"""

new = """  // "The link is gone" \u2014 the keep-alive loop's authoritative signal, read
  // before its own peripheral.is_connected() poll (see the loop for why that
  // poll is neither reliable nor free). Cleared before each loop; set by
  // SimpleBLE's on_disconnected callback, which fires on a SimpleBLE thread and
  // so cannot do the teardown itself, and by writeBlocking() once consecutive
  // write failures say the peer has stopped answering. Desktop counterpart of
  // the Android link-loss fix in bluetoothmanager.cpp (handleLinkLost).
  std::atomic<bool> m_peripheralDropped{false};

  // Consecutive writeBlocking() failures on the current connection, reset by
  // any write that goes through. Windows keeps accepting writes into a link the
  // peer has already stopped answering and only fails them on async_get's
  // ten-second timeout, so this is the app's own evidence that the link is
  // dead \u2014 usually well before ConnectionStatusChanged says so.
  std::atomic<int> m_writeFailures{0};

  // Cached connection state, so the QML accessors never call into SimpleBLE.
  // The strings ride m_handleMutex with the handles they describe.
  std::atomic<bool> m_connectedCache{false};"""

crlf = "\r\n" in s
if crlf:
    old = old.replace("\n", "\r\n")
    new = new.replace("\n", "\r\n")
assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok crlf=", crlf)
import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.cpp"
s = io.open(p, encoding="utf-8", newline="").read()

old = """void BluetoothManager::writeBlocking(const QByteArray& data, bool withResponse) {
  try {
    auto peripheral = peripheralHandle();
    // Deliberately no is_connected() probe: that is itself a blocking hop onto
    // the WinRT MTA thread, so it doubled the serialised work per frame on the
    // one path that must stay quick. drainWriteQueue() has already checked the
    // cached state, and a peripheral that drops mid-write throws \u2014 which is
    // what the catch below is for.
    if (peripheral.initialized()) {
      SimpleBLE::ByteArray ba(reinterpret_cast<const uint8_t*>(data.constData()),
                              size_t(data.size()));
      if (withResponse) {
        peripheral.write_request(serviceUuidStr, ctrlUuidStr, ba);
      } else {
        peripheral.write_command(serviceUuidStr, ctrlUuidStr, ba);
      }
    }
  } catch (...) {
    qDebug() << "Bt | Write error: peripheral disconnected during write";
  }
}"""

new = """void BluetoothManager::writeBlocking(const QByteArray& data, bool withResponse) {
  try {
    auto peripheral = peripheralHandle();
    // Deliberately no is_connected() probe: that is itself a blocking hop onto
    // the WinRT MTA thread, so it doubled the serialised work per frame on the
    // one path that must stay quick. drainWriteQueue() has already checked the
    // cached state, and a peripheral that drops mid-write throws \u2014 which is
    // what the catch below is for.
    if (peripheral.initialized()) {
      SimpleBLE::ByteArray ba(reinterpret_cast<const uint8_t*>(data.constData()),
                              size_t(data.size()));
      if (withResponse) {
        peripheral.write_request(serviceUuidStr, ctrlUuidStr, ba);
      } else {
        peripheral.write_command(serviceUuidStr, ctrlUuidStr, ba);
      }
      m_writeFailures.store(0);
    }
  } catch (...) {
    // Windows does not fail a write into a peer that has gone quiet; it accepts
    // it and lets SimpleBLE's async_get give up ten seconds later, so each of
    // these cost ten seconds of the one MTA thread the keep-alive poll, the
    // teardown and every other write also have to queue on. ConnectionStatusChanged
    // has been seen half a minute behind that, which is a queue of stale frames
    // metered out at ten seconds each while the app still believes it is
    // connected \u2014 and the UI freezing on every GUI-thread SimpleBLE call in the
    // meantime.
    //
    // Two in a row is the app's own answer to a question Windows is still
    // thinking about. One is not enough to act on \u2014 a single frame can fail on
    // a link that is fine \u2014 but a second consecutive failure means nothing has
    // reached the synth for at least ten seconds, on a link whose supervision
    // timeout is four. Hand it to the keep-alive loop, which owns the teardown.
    const int failures = m_writeFailures.fetch_add(1) + 1;
    qDebug() << "Bt | Write error: peripheral disconnected during write ("
             << failures << "in a row )";
    if (failures >= kWriteFailuresBeforeDrop && !m_peripheralDropped.exchange(true)) {
      qDebug() << "Bt | Link presumed lost after" << failures
               << "consecutive write failures; tearing down.";
    }
  }
}"""

assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)

# constant next to kMaxQueuedWrites
oldc = """constexpr int kMaxQueuedWrites = 256;"""
newc = """constexpr int kMaxQueuedWrites = 256;

// Consecutive failed CTRL writes before the link is declared lost without
// waiting for Windows to agree. See writeBlocking() for why one is not enough
// and why waiting for a third is expensive.
constexpr int kWriteFailuresBeforeDrop = 2;"""
assert s.count(oldc) == 1
s = s.replace(oldc, newc)

io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.cpp"
s = io.open(p, encoding="utf-8", newline="").read()

old = """  auto adapter = adapterHandle();
  auto peripheral = peripheralHandle();
  if (adapter.initialized() and adapter.scan_is_active()) {
    qDebug() << "Bt | connectToSelectedDevice | Stopping scan, will connect on the next one.";
    m_skipScanGoConnect = true;
  } else if (not m_adapterIsConnecting and
             not(peripheral.initialized() and peripheral.is_connected())) {
    scanAndConnect();
  }"""

new = """  // Cached state only, like scanAndConnect(): this runs on the GUI thread, and
  // a peripheral.is_connected() here would block the UI for however long the
  // MTA thread is busy \u2014 which, on the Connect button of a synth that has just
  // stopped answering, is the worst possible moment for it.
  auto adapter = adapterHandle();
  if (adapter.initialized() and adapter.scan_is_active()) {
    qDebug() << "Bt | connectToSelectedDevice | Stopping scan, will connect on the next one.";
    m_skipScanGoConnect = true;
  } else if (not m_adapterIsConnecting and not m_connectedCache.load()) {
    scanAndConnect();
  }"""

assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
old = """    const int failures = m_writeFailures.fetch_add(1) + 1;
    qDebug() << "Bt | Write error: peripheral disconnected during write ("
             << failures << "in a row )";
    if (failures >= kWriteFailuresBeforeDrop && !m_peripheralDropped.exchange(true)) {
      qDebug() << "Bt | Link presumed lost after" << failures
               << "consecutive write failures; tearing down.";
    }"""
new = """    const int failures = m_writeFailures.fetch_add(1) + 1;
    qDebug() << "Bt | Write error: peripheral disconnected during write;"
             << failures << "in a row";
    if (failures >= kWriteFailuresBeforeDrop && !m_peripheralDropped.exchange(true)) {
      qDebug() << "Bt | Link presumed lost after" << failures
               << "consecutive write failures; tearing down.";
    }"""
assert s.count(old) == 1
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
old = """  // Disconnect any connected synth so the user starts from a clean scan \u2014 but
  // WITHOUT blocking the GUI thread. The old waitForFinished() froze the UI for
  // the whole disconnect (~1-2s), which is the "app hangs then the screen comes"
  // when opening the selector while connected. Instead ask the worker to stop
  // and resume scanning once it has finished, via a QFutureWatcher.
  auto peripheral = peripheralHandle();
  if (peripheral.initialized() and peripheral.is_connected()) {"""
new = """  // Disconnect any connected synth so the user starts from a clean scan \u2014 but
  // WITHOUT blocking the GUI thread. The old waitForFinished() froze the UI for
  // the whole disconnect (~1-2s), which is the "app hangs then the screen comes"
  // when opening the selector while connected. Instead ask the worker to stop
  // and resume scanning once it has finished, via a QFutureWatcher.
  //
  // The test is the cached flag for the same reason: peripheral.is_connected()
  // is a blocking hop onto the shared MTA thread, so asking it here reintroduced
  // exactly the freeze this function was rewritten to remove \u2014 worst on a link
  // that has stopped answering, which is when the user reaches for the selector.
  if (m_connectedCache.load()) {"""
assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
old = """    // Nothing queued for a link that is down: the firmware runs
    // voice_manager_all_notes_off() on disconnect, and the listings would be
    // answered into a closed connection.
    if (!m_connectedCache.load()) {"""
new = """    // Nothing queued for a link that is down: the firmware runs
    // voice_manager_all_notes_off() on disconnect, and the listings would be
    // answered into a closed connection.
    //
    // m_peripheralDropped as well as the cached flag, because it is set first:
    // the keep-alive loop needs up to its 100 ms poll period to notice and
    // publish, and every frame written into that window costs its own
    // ten-second timeout on the MTA thread the teardown itself has to queue on.
    if (!m_connectedCache.load() || m_peripheralDropped.load()) {"""
assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\bluetoothmanager2.cpp"
s = io.open(p, encoding="utf-8", newline="").read()

block = """
  // Armed before the callback is installed, so a disconnect landing in the gap
  // between the two is not swallowed.
  m_peripheralDropped.store(false);
  m_writeFailures.store(0);
  peripheral.set_callback_on_disconnected([this]() {
    qDebug() << "Bt | Peripheral disconnected (callback)";
    m_peripheralDropped.store(true);
  });
"""
assert s.count(block) == 1
s = s.replace(block, "")

anchor = """  publishConnectionState(true, name, address, negotiatedMtu);
"""
newblock = """  // Link-loss detection, armed before the connection is announced: both flags
  // still carry the *previous* session's verdict, and drainWriteQueue() reads
  // them against the cached state publishConnectionState() is about to set. The
  // callback goes in ahead of the announcement too, so a disconnect between the
  // two is not swallowed.
  m_peripheralDropped.store(false);
  m_writeFailures.store(0);
  peripheral.set_callback_on_disconnected([this]() {
    qDebug() << "Bt | Peripheral disconnected (callback)";
    m_peripheralDropped.store(true);
  });

  publishConnectionState(true, name, address, negotiatedMtu);
"""
assert s.count(anchor) == 1
s = s.replace(anchor, newblock)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
