# SD card bring-up — decoding a failed mount

Notes from tracing an `esp_vfs_fat_sdspi_mount` failure on the S3 (IDF v6.0.1).
Kept because the log a failed mount prints is far less informative than it
looks, and the useful lines are compiled out by default.

## The init sequence

`sdmmc_card_init` (`components/sdmmc/sdmmc_init.c`) runs these steps in order,
aborting on the first failure:

1. **CMD0** `GO_IDLE_STATE` — sent twice in SPI mode; the second expects a
   valid R1.
2. **CMD8** `SEND_IF_COND` — identifies SDHC/SDXC. **A failure here is
   tolerated** (`sdmmc_sd.c:29`): timeout or "rejected" is logged at DEBUG and
   the card is treated as SD v1.x. The consequence is that `SD_OCR_SDHC_CAP`
   is never set, so ACMD41 goes out **without the HCS bit** — and any SDHC/SDXC
   card then stays in idle forever, surfacing as an ACMD41 timeout. A silent
   CMD8 failure looks exactly like an ACMD41 failure.
3. **CMD5** `IO_SEND_OP_COND` — the SDIO probe. A memory card answers
   "command not supported" (R1 = 0x04). **This line is normal and expected.**
4. **CMD59** — enable CRC16 for data transfers.
5. **ACMD41** (`CMD55` + `CMD41`) — poll until the card leaves idle.

## Reading the timing

`sdmmc_send_cmd_send_op_cond` (`sdmmc_cmd.c:141`) has two exit paths that both
return `ESP_ERR_TIMEOUT` (0x107), and the elapsed time tells them apart:

- **`nretries` exhausted** — 300 polls, `vTaskDelay(10 ms)` each: the card
  answers but never clears the idle bit. Takes **~3 s**.
- **`err_cnt` exhausted** — 3 consecutive command errors. `continue` skips the
  delay, so this takes **a few ms**.

So the gap between the `cmd=5` line and the `send_op_cond (1) returned 0x107`
line identifies the failure mode. ~12 ms means the card stopped answering, not
that it was slow to become ready.

## What is compiled out

`sdspi_transaction.c` has two R1 decoders. The SDIO one logs at **INFO** (that
is why the `cmd=5` line shows). The **memory-card** one — CRC error, R1 not
found, unexpected value — logs at **DEBUG**, as do all the per-command traces
in `sdmmc_common`/`sdmmc_sd`. With `CONFIG_LOG_MAXIMUM_LEVEL=3` (INFO, the
project default) those are not merely filtered, they are not in the binary.

To see them: menuconfig -> Component config -> Log -> Maximum log verbosity ->
Debug, then set `OSYNTH_SD_VERBOSE` to 1 in `components/looper/loop_store.cpp`
(it raises the six sdmmc/sdspi tags to DEBUG in `loop_store_init`). Revert both
once the card mounts.

## What the clock is doing

Card init always runs at `SDMMC_FREQ_PROBING` = **400 kHz**
(`sdspi_host.c:371` configures the device at that speed; the host only moves to
`max_freq_khz` in `sdmmc_init_host_frequency`, after init completes). So
"lower the SPI frequency" is not a remedy for an init-stage failure — it is
already as slow as it goes. Signal integrity can only be blamed if the wiring
is bad enough to corrupt 400 kHz.

## Observed trace on this board (2026-08-05)

With `OSYNTH_SD_VERBOSE` on:

```
D sdspi_host: poll_busy: timeout            <- x2, 40 ms each, before CMD0
D sdmmc_sd:   SDHC/SDXC card                <- CMD8 OK, R7 echo verified
I sdspi_transaction: cmd=5, R1: not supported   <- normal
D sdspi_host: data CRC set=1                <- CMD59 OK, CRC now enabled
D sdspi_host: sdspi_host_start_command: cmd=55 error=0x107   <- x3, then give up
```

Reading it:

- CMD8 returning "SDHC/SDXC card" means the 5-byte R7 came back **with the
  0xAA check pattern intact** (`sdmmc_send_cmd_send_if_cond` verifies it). The
  link works in both directions; wiring, pull-ups, pin assignment and power are
  not the problem.
- The two `poll_busy: timeout` lines are before the card is in SPI mode (the
  74 init clocks are sent by `go_idle_clockout` *after* poll_busy) and are
  non-fatal by design — `sdspi_host_start_command` explicitly tolerates
  `ESP_ERR_TIMEOUT` from poll_busy (`sdspi_host.c:507`). Commonly seen on
  working setups.
- `cmd=55 error=0x107` originates in `shift_cmd_response` (`sdspi_host.c:694`):
  it scans up to 8 response bytes for one with bit 7 clear, finds none, and
  `start_command_default` turns `ESP_ERR_NOT_FOUND` into `ESP_ERR_TIMEOUT`.
  MISO stayed high for the whole window — **the card sent nothing at all**, as
  opposed to sending an R1 with an error bit (which would have logged
  "R1 response: ..." from `sdspi_transaction.c`).
- CMD55 is the first command issued after CMD59. A card that answers
  everything up to CRC-enable and then goes silent points at CMD59.

IDF has a supported escape hatch for this: `SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC`
(`sd_protocol_types.h:194`) skips CMD59 entirely
(`sdmmc_init.c`: `if (!ignore_data_crc) SDMMC_INIT_STEP(is_spi, sdmmc_init_spi_crc)`).
Exposed here as `OSYNTH_SD_SKIP_CRC` in `loop_store.cpp`.

## Filesystem

`FF_FS_EXFAT = 0` in IDF's `ffconf.h` — exFAT is not compiled in. FAT12/16/32
only, and `format_if_mount_failed = false` in `ensure_mounted()`, so the card
must be formatted on a PC. `FF_MIN_SS`/`FF_MAX_SS` resolve to 512/4096, so
`CONFIG_FATFS_SECTOR_4096` (set for the wear-levelled flash partition) does not
prevent 512-byte-sector SD cards from mounting.

## Second failure: the card mounts, then the first write fails

Signature (the S31 streamed looper's first take):

```
E loop_stream: cannot open /sd/osynth/live/rec.tmp (No such file or directory)
W looper: track 1: sd take could not be opened — rec rejected
W looper: armed take could not start — back to stop
```

Reading this backwards is what matters. `fopen(..., "wb")` maps to
`FA_WRITE | FA_CREATE_ALWAYS`, so FatFs cannot answer `FR_NO_FILE` — a create
always creates. The only other `FRESULT` that reaches errno as `ENOENT`
(`vfs_fat.c: fresult_to_errno`) is **`FR_NO_PATH`**, returned by `follow_path()`
*before any write is attempted*. So the message means exactly one thing: the
directory `/sd/osynth/live` did not exist. It says nothing about `rec.tmp`.

Both `mkdir()` calls that should have created it were unchecked — one in
`loop_store.cpp: ensure_mounted()` (for `/sd/osynth`, run once per mount) and
two in `loop_stream.cpp: loop_stream_begin_set()`. A failing `mkdir` therefore
threw away the real errno and surfaced as this bare `ENOENT` one call later.
Both now go through `loop_store_ensure_dir()`, which stats, creates, and logs
`strerror(errno)` on failure. **The errno on the `cannot create` line is the
actual diagnosis:**

| errno | `FRESULT` | Meaning |
| --- | --- | --- |
| `EIO` | `FR_DISK_ERR` | The card will not take a write. Bus integrity — see below. |
| `EACCES` | `FR_DENIED` / `FR_WRITE_PROTECTED` | Volume full, or mounted read-only. |
| `ENOENT` | `FR_NO_PATH` | The *parent* is missing (only possible for `/sd/osynth`, whose parent is the root — i.e. the volume is not what we think it is). |
| `ENOMEM` | `FR_NOT_ENOUGH_CORE` | `CONFIG_FATFS_LFN_HEAP` could not get its 512 B working buffer (allocated from PSRAM, `CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y`). |
| `ETIMEDOUT` | `FR_TIMEOUT` | Volume mutex contention (`CONFIG_FATFS_TIMEOUT_MS=10000`). |

`EIO` is the likely one on a breadboard, and it is the asymmetry worth
remembering: **reads tolerate a marginal SPI bus that writes do not.** Mount and
capacity reporting are read-only, so a card can report `SD mounted: 3724 MB`
and still fail the first `mkdir`. Directory creation is the first write the
firmware ever attempts. Knob: `OSYNTH_SD_FREQ_KHZ` in `loop_store.cpp`
(20000 default, try 10000 then 4000; init is always 400 kHz regardless).

## Open file descriptors

`max_files` in `esp_vfs_fat_sdmmc_mount_config_t` is the FIL table for the
whole mount, and the mount is shared (looper slots, streamed tracks, SD drum
kits). A streamed set holds `LOOP_TRACKS + 1` at once — one per playing track
plus the open take. It was 2, which was correct while save/load was the only
user; exceeding it gives `E vfs_fat: open: no free file descriptors` and
`ENFILE`, not `ENOENT`. Now `LOOP_TRACKS + 4`.

## Two mounters, one card

`loop_store.cpp` and `drum_kit.cpp` both bring up SPI2 and both mount `/sd`.
`drum_kit`'s `ensure_mounted()` opens with `stat("/sd")` and treats an existing
mount as success; `loop_store`'s does not, so if the drum kits ever mount
first, `esp_vfs_fat_sdspi_mount` returns `ESP_ERR_INVALID_STATE` and the
looper's SD backend stays dead for the whole session. That order does not
happen today (the looper initialises first); the mount failure branch now names
this case explicitly instead of asking "card inserted?".

### Resolution: `mkdir` is the odd one out

The checked version reported:

```
E loop_store: cannot create /sd/osynth/live (No such file or directory)
```

`ENOENT` out of `mkdir` is `FR_NO_PATH`, which `follow_path()` returns when a
**non-final** path segment is missing or is not a directory — here, `/sd/osynth`.
But that directory demonstrably exists and is writable: the card already held a
loop saved to a slot (`/sd/osynth/loop0.olp`) from an earlier session. So the
card takes file creation, writes, renames and deletes at arbitrary depth, and
refuses only `mkdir`. That rules out every hypothesis above — bus integrity,
free space, write protection, sector size, heap — none of which could be
selective in that way. Root cause not established.

What it does establish is that `mkdir` is not worth depending on. The streamed
looper now keeps its scratch in `/sd/osynth` itself, as `live0.olt`…`live7.olt`
and `live.tmp` (all valid 8.3, so long-filename support is not a dependency
either). The isolation from the save slots that the subdirectory provided is
now a property of the names: nothing in `loop_stream.cpp` opens, renames or
removes a path it did not spell out itself, so `loopN.olp` cannot be touched
whatever happens mid-take.

`/sd/osynth` is still created by `ensure_dir()` on the mount, which is the only
remaining `mkdir` in the looper — and on a card where that fails, creating the
directory on a PC is enough to make both the slots and the streamed looper
work.

The `mkdir` failure did not survive a power cycle of the module, and the
section below is why: it was almost certainly the same bad link, not a card
that refuses directories. The flat layout stays regardless — it depends on
strictly less, and nothing about it was worth undoing once written.

## Third failure: the card stops answering mid-session

```
E sdmmc_cmd: sdmmc_read_sectors_dma: sdmmc_send_cmd returned 0x107, status 0x0
E sdmmc_cmd: sdmmc_read_sectors: error 0x107 reading blocks 63238+[0..0]
E diskio_sdmmc: sdmmc_read_blocks failed (0x107)
E loop_store: cannot create /sd/osynth (I/O error)
...
E task_wdt: Task watchdog got triggered ... CPU 0: loop_ctl
```

`0x107` is `ESP_ERR_TIMEOUT` out of `poll_data_token` — the card accepted the
read command and never returned the data token. **Reads**, not writes, and the
same card mounted cleanly at boot ~165 s earlier. Nothing in the firmware makes
a card do that; this is the link going bad under load (the alive line at the
time shows BLE connected, USB running at 40–47 % with drops, and audio
underruns). Suspects, in order: supply droop or noise on the card's 3.3 V rail
(SD cards pull >100 mA in bursts), flying-lead length at 20 MHz, grounding.
Knob: `OSYNTH_SD_FREQ_KHZ`.

It also reframes the `mkdir` `ENOENT` above: an unreliable link explains a root
directory that reads as empty (hence "no path" for a directory that exists)
just as well as it explains a hard timeout. Treat the two as one fault.

A power cycle of the module brought the card back, with none of the mitigations
applied — so nothing here is a firmware fault. What the episode did expose is
two firmware faults of its own, both now fixed:

- *Do not investigate a card that has stopped answering.* Every operation
  against one burns a full ~1.1 s timeout. A forensic dump added here (stat
  each path, list the card root) cost ~6 s inside `loop_ctl`, which does not
  yield — that, not the card, is what tripped the IDLE0 watchdog. The dump is
  gone; `card_unresponsive()` (`EIO`/`ETIMEDOUT`/`ENODEV`) is what remains of
  it, and its job is to stop the traffic rather than add more.
- *An I/O error must drop the mount.* `ensure_mounted()` short-circuits on
  `s_card != nullptr`, so once the card is gone every later call kept using a
  dead handle and paid the timeout again. `ensure_dir()` now calls
  `drop_mount()` on `EIO`/`ETIMEDOUT`/`ENODEV`, and `ensure_mounted()` returns
  `s_card != nullptr` at the end so a mount whose first real access failed is
  not reported as good. The next attempt re-runs the card init from scratch,
  which is what recovery has to look like.

Worst case on the record path is now one failed mount attempt (~1–2 s) on
`loop_ctl` per press of record, then the PSRAM fallback. Loud, bounded, and
under the 5 s watchdog — but it is a stall, so a card known to be bad is worth
switching off at the app rather than leaving armed.

## Card removal and re-insertion

There is no card-detect pin on the breakout (six pins: VCC, GND and the four
SPI lines), so presence cannot be an interrupt — it has to be asked about.

`loop_ctl` asks once a second while the transport is stopped
(`kCardPollMs`), via `loop_store_poll_card()`. It answers with three states,
not two, and that distinction is load-bearing: **NONE ("nothing is mounted
right now") is not LOST ("the card just left")**. They call for opposite
responses — one waits, the other tears down — and collapsing them into a
boolean, as the first version did, meant a mount merely between retries
suspended a perfectly good set.

Relatedly, `ensure_dir()` *reports* an unresponsive card but no longer
unmounts one. Unmounting frees the FATFS context that the streamed looper's
open track handles point into, and a directory check knows nothing about
those. The poll is the single place that unmounts, and it has the caller give
up its handles first.

What the states mean:

- **Mounted**: a CMD13 status read (`sdmmc_get_status`). No data phase, so it
  costs a few hundred microseconds on a card that is there. Failure means
  removed — and the mount is deliberately *left in place*, because
  `esp_vfs_fat_sdcard_unmount()` frees the FATFS context that every open FILE
  points into. `ctl_card_lost()` gives up the handles first
  (`loop_stream_abandon_set()`, which closes without touching the card — FatFs
  only syncs a file it wrote to, so closing read handles is free), then
  `loop_store_card_gone()` unmounts.
- **Not mounted**: a mount attempt, which is what picks up a re-insertion.

The backoff matters more than it looks. A mount attempt against an empty slot
costs ~1 s of `loop_ctl` inside sdmmc's ACMD41 retry loop, so an unconditional
1 Hz retry would spend a third of the control task on a synth that simply has
no card. `loop_store` doubles its own floor from 2 s to 30 s while attempts
keep failing, resets it when one succeeds, and resets it again when a *working*
card disappears — the one case where it is genuinely expected back. Only the
first failure of a run is logged. Demand paths (save, load, starting a streamed
set) ignore the floor: the user is waiting and a second is worth it.

**A streamed set survives removal.** Its tracks are files, and a card sitting
on the desk still has them, so removal *suspends* the set instead of clearing
it: length, format and filled mask all stand, every track is muted through
`g_hold` (held, not starved — no underrun counting, no re-prime requests), and
the loop plays silence until the card is back. Putting it back re-opens the
files and the audio returns.

"Back" is checked, not assumed. `loop_stream_resume_set()` compares the card's
CID serial against the one recorded when the set was opened, because a
different card can easily carry a stale `live*.olt` from someone else's power
cut and it would play as though it were this set. A serial mismatch, or a
missing track file, clears the set — at that point `loop.filled` is a lie and
leaving it silent forever is worse than losing it. A PSRAM set is untouched
throughout; it never needed the card.

Resume is idle-only for the same reason the poll is: priming a window is a
ring reset, and doing that under a live reader is what
`loop_stream_add_track()`'s resync handshake exists to avoid. Re-inserting
mid-playback therefore does nothing until the transport stops.

Idle-only is deliberate: a running transport is either reading the card or
about to, and a status probe in the middle of that says nothing its own I/O
errors would not.
