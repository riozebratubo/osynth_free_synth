# c6_ota_updater — reflash the ESP32-C6 co-processor over SDIO

A **one-shot maintenance tool**, not part of osynth. It is Espressif's
`host_performs_slave_ota` example (from `managed_components/espressif__esp_hosted/`)
copied out and configured for this board. Flashing it **replaces osynth on the
P4** for as long as it takes to update the C6; you then flash osynth back.

## Why this exists

The P4+C6 board gives the C6 no USB of its own. All three USB ports lead to the
P4 — two enumerate as serial (a CH340 on UART0, and the P4's built-in
USB-Serial/JTAG) and both report the same chip and MAC. So there is no port to
point `idf.py flash` at for the co-processor.

What there *is* is a working SDIO link between the two chips. This tool uses it:
the P4 carries the C6 image in its own flash and streams it across via
`esp_hosted_slave_ota_begin/write/end`.

The C6 shipped running ESP-Hosted **2.3.2** while osynth's host component is
**2.12.12** (`dependencies.lock`). With that gap the C6 answers
`ESP_ERR_NOT_SUPPORTED` to the BT-controller init RPC, so `ble_ctrl_init()`
degrades to "no BLE" and the synth runs without remote control.

## Procedure

**The scripted route.** `tools/flash_c6.ps1` does everything below — finds and
activates ESP-IDF, bootstraps `managed_components/`, stages and builds both
projects, detects the P4's port, flashes, and puts osynth back. It works from a
bare clone on a machine that only has ESP-IDF installed:

```powershell
.\tools\flash_c6.ps1              # full run, prompts before writing anything
.\tools\flash_c6.ps1 -SkipFlash   # build both images, touch no hardware
.\tools\flash_c6.ps1 -Recreate    # re-stage both projects after an esp_hosted bump
```

It regenerates this project's `sdkconfig.defaults`, so change the script rather
than that file. The manual steps below are what it automates.

### Manual steps

The C6 image is already staged at
`components/ota_partition/slave_fw_bin/network_adapter.bin`, built from
`tools/c6_slave`. To rebuild it after changing the slave:

```powershell
Set-Location D:\dev\osynth_free_synth\tools\c6_slave
idf.py build
Copy-Item -Force .\build\network_adapter.bin ..\c6_ota_updater\components\ota_partition\slave_fw_bin\
```

Then, in a shell with the IDF environment loaded
(`. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1`):

```powershell
Set-Location D:\dev\osynth_free_synth\tools\c6_ota_updater
idf.py set-target esp32p4
idf.py build
idf.py -p <P4 port> flash monitor
```

`idf.py flash` writes the staged `.bin` into the `slave_fw` partition
(`partitions.csv`: 0x5F0000, 2 MB — the image is ~1.21 MB) as well as flashing
the app. The app then runs the update on its own at boot; there is no console
command to type. Watch for `OTA completed successfully!`.

**Then put osynth back:**

```powershell
Set-Location D:\dev\osynth_free_synth
idf.py -p <P4 port> flash monitor
```

Confirm it took: `ble_ctrl` logs the co-processor version at boot, and it should
now read 2.12.12 rather than 2.3.2.

## Two things to watch

**Activation on a 2.3.2 slave.** `activate_and_restart()` in `main/main.c:203`
only calls `esp_hosted_slave_ota_activate()` when the running slave is newer
than 2.5, so against 2.3.2 it will log *"Activate API not supported (requires
v2.6.0+)"* and simply restart the host. In ESP-Hosted releases before the
activate call was split out, `ota_end` set the boot partition and rebooted the
slave itself, so the update should still take — but that is behaviour of the
2.3.2 firmware, which is not in this tree and was not verified. **The check that
matters is the version `ble_ctrl` prints afterwards.** If it still says 2.3.2,
the image was written but never activated, and the C6 needs a wired route
(its UART pins plus an external USB-serial adapter) instead.

**The board settings are load-bearing.** `sdkconfig.defaults` carries the four
P4 fixes osynth needed — chip revision + 360 MHz CPU, PSRAM, ESP-Hosted mempools
in PSRAM, and the SDIO board wiring. Drop any of them and this project
reproduces osynth's boot failures on the same silicon. Each is commented in
place; the full account is in `sdkconfig.defaults.esp32p4` at the repo root.

## Local modifications

- `main/idf_component.yml` — `cmd_nvs` and `cmd_system` were local-path
  dependencies on `${IDF_PATH}` (C:), which the component manager cannot express
  relative to a project on D:. Both are vendored under `components/` instead.
  Same fix as `tools/c6_slave`.
- `sdkconfig.defaults` — 16 MB flash, `CONFIG_OTA_METHOD_PARTITION`, and the P4
  block described above.
