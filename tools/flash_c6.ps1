<#
.SYNOPSIS
    Build and flash the ESP32-C6 co-processor firmware on an osynth P4 board.

.DESCRIPTION
    End-to-end, from a fresh clone and a fresh ESP-IDF install:

      1. Locates and activates ESP-IDF (EIM-style or classic install).
      2. Bootstraps the main project so managed_components/ exists.
      3. Stages and builds the ESP-Hosted slave firmware for the C6.
      4. Stages the slave-OTA updater for the P4 and embeds the C6 image.
      5. Finds the P4's serial port.
      6. Flashes the updater, which pushes the image to the C6 over SDIO.
      7. Restores osynth to the P4.

    WHY THIS IS NEEDED: the P4+C6 board gives the C6 no USB of its own — every
    USB port on it leads to the P4 — so the co-processor cannot be flashed
    directly. The P4 carries the image and streams it across the SDIO link.

    Steps 5-7 touch the board. Everything before that is host-side only, so
    -SkipFlash gives you a dry run that still produces both binaries.

.PARAMETER Port
    Serial port of the P4 (e.g. COM10). Autodetected if omitted.

.PARAMETER IdfVersion
    Prefer a specific ESP-IDF version when several are installed, e.g. '6.0.2'.
    Defaults to the highest found.

.PARAMETER Recreate
    Delete and re-stage both tool projects before building. Use after bumping
    the esp_hosted component.

.PARAMETER SkipFlash
    Build everything, touch nothing. No serial port needed.

.PARAMETER NoRestore
    Leave the updater on the P4 instead of reflashing osynth at the end.

.PARAMETER Yes
    Skip confirmation prompts (for unattended runs).

.EXAMPLE
    .\tools\flash_c6.ps1
    .\tools\flash_c6.ps1 -Port COM10 -Yes
    .\tools\flash_c6.ps1 -SkipFlash
#>
[CmdletBinding()]
param(
    [string]$Port,
    [string]$IdfVersion,
    [switch]$Recreate,
    [switch]$SkipFlash,
    [switch]$NoRestore,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------- paths ----

$RepoRoot    = Split-Path -Parent $PSScriptRoot
$ToolsDir    = Join-Path $RepoRoot 'tools'
$SlaveDir    = Join-Path $ToolsDir 'c6_slave'
$UpdaterDir  = Join-Path $ToolsDir 'c6_ota_updater'
$HostedDir   = Join-Path $RepoRoot 'managed_components\espressif__esp_hosted'

# ------------------------------------------------------------- plumbing ----

$script:StepNo = 0
function Write-Step([string]$Message) {
    $script:StepNo++
    Write-Host ''
    Write-Host "==> [$script:StepNo] $Message" -ForegroundColor Cyan
}
function Write-Note([string]$Message) { Write-Host "    $Message" -ForegroundColor DarkGray }
function Write-Warn([string]$Message) { Write-Host "    ! $Message" -ForegroundColor Yellow }

function Confirm-Action([string]$Message) {
    if ($Yes) { return $true }
    $answer = Read-Host "$Message [y/N]"
    return $answer -match '^(y|yes)$'
}

# idf.py is a PowerShell alias/function in EIM installs, a script elsewhere.
# Route through whichever exists and turn a non-zero exit into a terminating
# error, so a failed build stops the run instead of flashing a stale image.
# Arguments are passed as one explicit array rather than as remaining
# arguments: idf.py takes switches like -p, and PowerShell would try to bind
# a bare -p against this function's own parameters and fail.
function Invoke-Idf {
    param([Parameter(Mandatory = $true)][string[]]$IdfArgs)
    Write-Note "idf.py $($IdfArgs -join ' ')"
    if (Get-Command 'idf.py' -ErrorAction SilentlyContinue) {
        idf.py @IdfArgs
    } else {
        & python (Join-Path $env:IDF_PATH 'tools\idf.py') @IdfArgs
    }
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py $($IdfArgs -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Invoke-Esptool {
    param([Parameter(Mandatory = $true)][string[]]$EsptoolArgs)
    if (Get-Command 'esptool' -ErrorAction SilentlyContinue) {
        & esptool @EsptoolArgs 2>&1 | Out-String
    } else {
        & python -m esptool @EsptoolArgs 2>&1 | Out-String
    }
}

# --------------------------------------------------- ESP-IDF activation ----

# Returns the script to dot-source, or $null if IDF is already active.
# Dot-sourcing has to happen at script scope (the activation defines aliases
# and a venv), which is why this only *finds* the file.
function Find-IdfActivationScript {
    param([string]$Version)

    if ($env:IDF_PATH -and (Get-Command 'idf.py' -ErrorAction SilentlyContinue)) {
        return $null
    }

    # ESP-IDF Installation Manager (EIM) layout:
    #   <tools>\Microsoft.v<version>.PowerShell_profile.ps1
    # IDF_TOOLS_PATH is NOT ~/.espressif on these installs, which is exactly
    # why esp-idf's own export.ps1 fails on them — it assumes the default and
    # then looks for a python venv that was never created there.
    $toolRoots = @(
        $env:IDF_TOOLS_PATH
        'C:\Espressif\tools'
        (Join-Path $env:USERPROFILE '.espressif')
    ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

    $profiles = @()
    foreach ($root in $toolRoots) {
        $profiles += Get-ChildItem $root -Filter 'Microsoft.v*.PowerShell_profile.ps1' -ErrorAction SilentlyContinue
    }
    if ($Version) {
        $profiles = @($profiles | Where-Object { $_.Name -eq "Microsoft.v$Version.PowerShell_profile.ps1" })
    }
    if ($profiles.Count -gt 0) {
        return ($profiles | Sort-Object Name -Descending | Select-Object -First 1).FullName
    }

    # Classic install: <idf>\export.ps1
    $exportRoots = @(
        $env:IDF_PATH
        'C:\esp'
        (Join-Path $env:USERPROFILE 'esp')
    ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

    $exports = @()
    foreach ($root in $exportRoots) {
        $direct = Join-Path $root 'export.ps1'
        if (Test-Path $direct) { $exports += Get-Item $direct }
        $exports += Get-ChildItem $root -Filter 'export.ps1' -Depth 2 -Recurse -ErrorAction SilentlyContinue
    }
    if ($Version) {
        $filtered = @($exports | Where-Object { $_.FullName -like "*$Version*" })
        if ($filtered.Count -gt 0) { $exports = $filtered }
    }
    if ($exports.Count -gt 0) {
        return ($exports | Sort-Object FullName -Descending | Select-Object -First 1).FullName
    }

    throw @"
Could not find an ESP-IDF installation.
Looked for EIM profiles under: $($toolRoots -join ', ')
and export.ps1 under:          $($exportRoots -join ', ')
Open an ESP-IDF shell yourself and re-run this script, or pass -IdfVersion.
"@
}

# ------------------------------------------------------------- staging ----

# Copy a project out of managed_components. Building inside managed_components
# is not safe: the component manager records a hash per managed component and
# re-downloads any whose contents changed, which would delete the build tree.
function Copy-StagedProject {
    param([string]$Source, [string]$Destination, [string]$Label)

    if ((Test-Path $Destination) -and -not $Recreate) {
        Write-Note "$Label already staged (use -Recreate to refresh)"
        return
    }
    if (Test-Path $Destination) {
        Write-Note "removing existing $Label"
        Remove-Item -Recurse -Force $Destination
    }
    if (-not (Test-Path $Source)) { throw "Source project not found: $Source" }
    Write-Note "staging $Label from $Source"
    Copy-Item -Recurse -Force $Source $Destination
}

# Vendor every ${IDF_PATH}-relative dependency into the project's own
# components/ directory and drop it from idf_component.yml.
#
# On Windows this is not cosmetic. The component manager writes local
# components into dependencies.lock as paths relative to the project, and
# os.path.relpath cannot express a C: path relative to a D: one — the build
# dies in pydantic with "ValueError: path is on mount 'C:', start on mount
# 'D:'". Components under <project>/components are picked up automatically, so
# once vendored the dependency entry is not needed at all.
function Resolve-LocalPathDeps {
    param([string]$ProjectDir, [string]$Label)

    $yml = Join-Path $ProjectDir 'main\idf_component.yml'
    if (-not (Test-Path $yml)) { return }

    $text = Get-Content $yml -Raw
    $pattern = '(?m)^[ ]{2}(?<name>[A-Za-z0-9_.\-]+):[ ]*\r?\n[ ]{4}path:[ ]*\$\{IDF_PATH\}(?<rel>[^\r\n]*)\r?\n'
    $found = [regex]::Matches($text, $pattern)
    if ($found.Count -eq 0) { return }

    $componentsDir = Join-Path $ProjectDir 'components'
    New-Item -ItemType Directory -Force $componentsDir | Out-Null

    $vendored = @()
    foreach ($m in $found) {
        $name = $m.Groups['name'].Value
        $rel  = $m.Groups['rel'].Value.TrimStart('/') -replace '/', '\'
        $src  = Join-Path $env:IDF_PATH $rel
        if (-not (Test-Path $src)) { Write-Warn "cannot vendor '$name': $src not found"; continue }

        # Remove first: Copy-Item -Recurse into an existing directory nests it.
        $dest = Join-Path $componentsDir $name
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
        Copy-Item -Recurse -Force $src $dest
        $vendored += $name
    }

    if ($vendored.Count -gt 0) {
        $text = [regex]::Replace($text, $pattern, '')
        Set-Content -Path $yml -Value $text -NoNewline
        Write-Note "$Label - vendored local deps: $($vendored -join ', ')"
    }
}

# The four P4 settings osynth had to find the hard way, plus the OTA method.
# Same silicon, same PSRAM, same SDIO link as osynth: omit any of these and
# this project reproduces osynth's boot failures. See sdkconfig.defaults.esp32p4
# in the repo root for the full account of each.
$UpdaterSdkconfig = @'
# Generated by tools/flash_c6.ps1 — edit the script, not this file.

# 16 MB flash on this board. The example's partition table still fits inside
# 8 MB; this only makes the image header match the chip.
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"

CONFIG_ESP_WIFI_REMOTE_ENABLED=y
CONFIG_ESP_HOSTED_ENABLED=y
CONFIG_FREERTOS_HZ=1000

CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"

# OTA source: a partition on the host. The other two methods do not fit —
# HTTPS needs Wi-Fi, and Wi-Fi runs over the very link being updated; LittleFS
# needs the image pushed into a filesystem first.
CONFIG_OTA_METHOD_PARTITION=y
CONFIG_OTA_PARTITION_LABEL="slave_fw"

# 1. Chip revision and CPU clock. The CPLL is capped at 360 MHz below rev 3.0;
#    400 MHz configures cleanly then dies before app_main with
#    "assert failed: esp_clk_init clk.c:104 (res)".
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y

# 2. PSRAM — needed on its own, and a precondition for block 3
#    (ESP_HOSTED_MEMPOOL_PREFER_SPIRAM depends on SPIRAM).
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_IGNORE_NOTFOUND=y

# 3. ESP-Hosted transport buffers out of internal RAM, or the SDIO driver's
#    mempool cannot be allocated:
#    "assert failed: sdio_mempool_create sdio_drv.c:258 (buf_mp_g)".
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y

# 4. SDIO wiring to the C6, same as osynth:
#    CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17], slave reset [54].
#    This project drives no I2S, so nothing collides with those pins here.
CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y
'@

# ---------------------------------------------------- port autodetection ----

function Find-P4Port {
    # Only USB-backed COM ports. Motherboard and AMT ports would each cost a
    # multi-second esptool timeout for nothing.
    $candidates = @(
        Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.DeviceID -like 'USB\*' -and $_.Name -match '\(COM\d+\)' } |
            ForEach-Object { [regex]::Match($_.Name, '\((COM\d+)\)').Groups[1].Value } |
            Select-Object -Unique
    )
    if ($candidates.Count -eq 0) { return $null }
    Write-Note "probing: $($candidates -join ', ')"

    foreach ($candidate in $candidates) {
        foreach ($verb in 'chip-id', 'chip_id') {   # renamed in esptool v5
            $output = Invoke-Esptool -EsptoolArgs @('--port', $candidate, $verb)
            if ($output -match 'ESP32-P4') {
                Write-Note "$candidate is an ESP32-P4"
                return $candidate
            }
            if ($output -notmatch 'Unknown command|invalid choice') { break }
        }
    }
    return $null
}

# =================================================================== run ====

Write-Host ''
Write-Host 'osynth - ESP32-C6 co-processor firmware update' -ForegroundColor Green
Write-Host '----------------------------------------------'
Write-Note "repo: $RepoRoot"

Write-Step 'Locating ESP-IDF'
$activation = Find-IdfActivationScript -Version $IdfVersion
if ($activation) {
    Write-Note "activating: $activation"
    . $activation
} else {
    Write-Note "already active: $env:IDF_PATH"
}
if (-not $env:IDF_PATH) { throw 'ESP-IDF activation did not set IDF_PATH.' }
Write-Note "IDF_PATH = $env:IDF_PATH"

Write-Step 'Bootstrapping the main project (populates managed_components/)'
if (Test-Path $HostedDir) {
    Write-Note 'esp_hosted already present'
} else {
    # managed_components/, sdkconfig and dependencies.lock are all gitignored,
    # so a fresh clone has none of them. set-target regenerates sdkconfig from
    # the tracked sdkconfig.defaults*; reconfigure preserves an existing one.
    Push-Location $RepoRoot
    try {
        if (Test-Path (Join-Path $RepoRoot 'sdkconfig')) {
            Invoke-Idf -IdfArgs @('reconfigure')
        } else {
            Invoke-Idf -IdfArgs @('set-target', 'esp32p4')
        }
    } finally { Pop-Location }

    if (-not (Test-Path $HostedDir)) {
        throw "esp_hosted still missing after bootstrap: $HostedDir"
    }
}

Write-Step 'Staging and building the C6 slave firmware'
Copy-StagedProject -Source (Join-Path $HostedDir 'slave') -Destination $SlaveDir -Label 'c6_slave'
Resolve-LocalPathDeps -ProjectDir $SlaveDir -Label 'c6_slave'
Push-Location $SlaveDir
try {
    Invoke-Idf -IdfArgs @('set-target', 'esp32c6')
    Invoke-Idf -IdfArgs @('build')
} finally { Pop-Location }

$SlaveBin = Join-Path $SlaveDir 'build\network_adapter.bin'
if (-not (Test-Path $SlaveBin)) { throw "Slave build produced no binary at $SlaveBin" }
Write-Note ("built network_adapter.bin ({0:N0} bytes)" -f (Get-Item $SlaveBin).Length)

Write-Step 'Staging the OTA updater for the P4'
Copy-StagedProject -Source (Join-Path $HostedDir 'examples\host_performs_slave_ota') `
                   -Destination $UpdaterDir -Label 'c6_ota_updater'
Resolve-LocalPathDeps -ProjectDir $UpdaterDir -Label 'c6_ota_updater'

Set-Content -Path (Join-Path $UpdaterDir 'sdkconfig.defaults') -Value $UpdaterSdkconfig
Write-Note 'wrote sdkconfig.defaults'

# idf.py flash writes whatever .bin is here into the slave_fw partition.
$StageDir = Join-Path $UpdaterDir 'components\ota_partition\slave_fw_bin'
New-Item -ItemType Directory -Force $StageDir | Out-Null
Copy-Item -Force $SlaveBin (Join-Path $StageDir 'network_adapter.bin')
Write-Note "staged C6 image into $StageDir"

Write-Step 'Building the OTA updater'
Push-Location $UpdaterDir
try {
    Invoke-Idf -IdfArgs @('set-target', 'esp32p4')
    Invoke-Idf -IdfArgs @('build')
} finally { Pop-Location }

if ($SkipFlash) {
    Write-Host ''
    Write-Host 'Built both images. -SkipFlash was set, so nothing was written.' -ForegroundColor Green
    Write-Note "C6 firmware : $SlaveBin"
    Write-Note "P4 updater  : $(Join-Path $UpdaterDir 'build')"
    return
}

Write-Step 'Finding the P4'
if (-not $Port) { $Port = Find-P4Port }
if (-not $Port) {
    throw @'
No ESP32-P4 found on any USB serial port. Pass -Port COMxx explicitly.
Note that on this board every USB port leads to the P4 (a CH340 on UART0 and
the built-in USB-Serial/JTAG both report the same chip and MAC) - either will
do for flashing.
'@
}
Write-Note "using $Port"

Write-Step 'Flashing the updater to the P4'
Write-Warn 'This REPLACES osynth on the P4. It is put back at the end of this script.'
if (-not (Confirm-Action "Flash the C6 updater to $Port?")) {
    Write-Host 'Aborted; nothing was written.' -ForegroundColor Yellow
    return
}
Push-Location $UpdaterDir
try { Invoke-Idf -IdfArgs @('-p', $Port, 'flash') } finally { Pop-Location }

Write-Host ''
Write-Host 'Updater flashed. It runs the OTA automatically at boot.' -ForegroundColor Green
Write-Host 'Opening the monitor - watch for "OTA completed successfully!".'
Write-Host 'Press Ctrl-] to leave the monitor and continue.' -ForegroundColor DarkGray
Push-Location $UpdaterDir
try { Invoke-Idf -IdfArgs @('-p', $Port, 'monitor') } catch { Write-Warn "monitor exited: $_" } finally { Pop-Location }

if ($NoRestore) {
    Write-Host ''
    Write-Warn 'The updater is still on the P4 (-NoRestore). Reflash osynth with:'
    Write-Note "cd $RepoRoot; idf.py -p $Port flash monitor"
    return
}

Write-Step 'Restoring osynth to the P4'
if (-not (Confirm-Action "Reflash osynth to $Port now?")) {
    Write-Warn 'Skipped. The updater is still on the P4; reflash osynth with:'
    Write-Note "cd $RepoRoot; idf.py -p $Port flash monitor"
    return
}
Push-Location $RepoRoot
try {
    Invoke-Idf -IdfArgs @('build')
    Invoke-Idf -IdfArgs @('-p', $Port, 'flash')
} finally { Pop-Location }

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host 'Check the result on the next boot: ble_ctrl logs the co-processor'
Write-Host 'version, which should now be 2.12.x rather than 2.3.2. If it still'
Write-Host 'reads 2.3.2, the image was written but never activated - see the'
Write-Host 'activation note in tools/c6_ota_updater/OSYNTH_README.md.'
