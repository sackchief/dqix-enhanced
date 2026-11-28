# Dragon Quest IX for PC: get the build tools (MSYS2) ready. Run by BUILD.bat.
#
#   msys2-setup.ps1 -Out <file>
#
# Finds an MSYS2 that is already on this PC (any drive), or installs one, then
# makes sure the tools the build needs are in it. On success it writes the
# MSYS2 folder to <file> and exits 0; on failure it says why and exits 1.
#
# In order, stopping as soon as the tools are there:
#   1. the folder in msys2-path.txt (next to BUILD.bat) or DQIX_MSYS2, if set
#   2. an MSYS2 Windows knows about (installed programs), or one in the usual
#      places (C:\msys64, D:\msys64..., Scoop, Chocolatey)
#   3. none: install one (winget, else the official installer from GitHub)
# and for the MSYS2 chosen:
#   - a lock left behind by an MSYS2 that was closed mid-update is removed
#     (only when no MSYS2 program is running)
#   - an old MSYS2 without the UCRT64 packages gets them added to its settings
#   - the tools are installed; if that fails, MSYS2 is updated and it tries again
#   - if it still fails, it asks before installing a fresh MSYS2, and keeps the
#     old one (renamed, never deleted)
#
# Runs on the PowerShell that comes with Windows 10 and 11 (5.1).
param([string]$Out, [switch]$Test)

$ErrorActionPreference = 'Stop'
$Project = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Packages = 'make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-python'
$Optional = 'mingw-w64-ucrt-x86_64-python-yaml mingw-w64-ucrt-x86_64-sdl3'      # nice to have: the build works without them (SDL3.dll: for launchers that look for it)
$InstallerUrl = 'https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-x86_64-latest.exe'

# Straight to the console or the log (not into a function's result).
function Say([string]$Text) { [Console]::Out.WriteLine($Text); [Console]::Out.Flush() }

function Test-Msys2([string]$Root) {
    if (-not $Root) { return $false }
    try { return (Test-Path -LiteralPath (Join-Path $Root 'usr\bin\bash.exe') -PathType Leaf) } catch { return $false }
}

# Every MSYS2 on this PC, the most likely first.
function Find-Msys2 {
    $found = New-Object System.Collections.ArrayList
    $add = { param($p) if ($p) { $p = $p.Trim().Trim('"').TrimEnd('\'); if ((Test-Msys2 $p) -and -not ($found -contains $p)) { [void]$found.Add($p) } } }
    $cfg = Join-Path $Project 'msys2-path.txt'
    if (Test-Path -LiteralPath $cfg) { & $add ((Get-Content -LiteralPath $cfg -TotalCount 1) -as [string]) }
    & $add $env:DQIX_MSYS2
    foreach ($key in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
                       'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
                       'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*')) {
        try {
            Get-ItemProperty -Path $key -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like 'MSYS2*' } | ForEach-Object {
                & $add $_.InstallLocation
                if ($_.UninstallString) { & $add (Split-Path -Parent ($_.UninstallString.Trim('"').Split('"')[0])) }
            }
        } catch { }
    }
    & $add 'C:\msys64'
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if ($d.DriveType -eq 'Fixed' -and $d.IsReady) { & $add (Join-Path $d.RootDirectory.FullName 'msys64') }
    }
    if ($env:USERPROFILE) { & $add (Join-Path $env:USERPROFILE 'scoop\apps\msys2\current') }
    & $add 'C:\tools\msys64'
    return ,$found
}

# A command in MSYS2's own shell. Its output goes to the log; returns the exit code.
function Invoke-Msys2([string]$Root, [string]$Command) {
    $bash = Join-Path $Root 'usr\bin\bash.exe'
    $env:MSYSTEM = 'UCRT64'; $env:CHERE_INVOKING = '1'
    $ErrorActionPreference = 'Continue'              # its messages on stderr are not errors here (PowerShell 5.1)
    & $bash -lc $Command 2>&1 | ForEach-Object { Say ([string]$_) }
    return $LASTEXITCODE
}

function Test-Tools([string]$Root) {
    foreach ($f in @('ucrt64\bin\gcc.exe', 'ucrt64\bin\python.exe', 'ucrt64\bin\SDL2.dll', 'usr\bin\make.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $f))) { return $false }
    }
    return $true
}

# An MSYS2 program still running from this folder (a terminal, an update)?
function Get-Msys2Processes([string]$Root) {
    $r = $Root.TrimEnd('\') + '\'
    try { return @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($r, [StringComparison]::OrdinalIgnoreCase) }) } catch { return @() }
}

# pacman's lock: left behind when an update was interrupted ("unable to lock database").
function Clear-StaleLock([string]$Root) {
    $lock = Join-Path $Root 'var\lib\pacman\db.lck'
    if (-not (Test-Path -LiteralPath $lock)) { return }
    if ((Get-Msys2Processes $Root).Count -gt 0) {
        Say '   MSYS2 is busy (a window of it is open, or it is updating): close it, then run this again.'
        return
    }
    Remove-Item -LiteralPath $lock -Force -ErrorAction SilentlyContinue
    Say '   Removed a lock left behind by an interrupted MSYS2 update.'
}

# MSYS2 from before 2021 doesn't know the UCRT64 packages ("target not found").
function Add-Ucrt64Repo([string]$Root) {
    $conf = Join-Path $Root 'etc\pacman.conf'
    if (-not (Test-Path -LiteralPath $conf)) { return }
    $text = [IO.File]::ReadAllText($conf)
    if ($text -match '(?m)^\[ucrt64\]') { return }
    Say '   This MSYS2 is older than its UCRT64 packages: adding them to its settings.'
    $list = Join-Path $Root 'etc\pacman.d\mirrorlist.ucrt64'
    if (-not (Test-Path -LiteralPath $list)) {
        [IO.File]::WriteAllText($list, "Server = https://mirror.msys2.org/mingw/ucrt64/`nServer = https://repo.msys2.org/mingw/ucrt64/`n")
    }
    Copy-Item -LiteralPath $conf -Destination "$conf.before-dqix" -Force
    $block = "`n[ucrt64]`nInclude = /etc/pacman.d/mirrorlist.ucrt64`n"
    # before the first other package list, or at the end
    $i = $text.IndexOf("`n[mingw32]"); if ($i -lt 0) { $i = $text.IndexOf("`n[mingw64]") }; if ($i -lt 0) { $i = $text.IndexOf("`n[msys]") }
    if ($i -ge 0) { $text = $text.Insert($i, $block) } else { $text = $text + $block }
    [IO.File]::WriteAllText($conf, $text)
}

function Install-Tools([string]$Root) {
    Clear-StaleLock $Root
    Add-Ucrt64Repo $Root
    Say '   Installing the tools the build needs (the first time downloads them)...'
    [void](Invoke-Msys2 $Root "pacman -Sy --noconfirm --needed $Packages")
    if (Test-Tools $Root) { [void](Invoke-Msys2 $Root "pacman -S --noconfirm --needed $Optional"); return $true }
    Say '   That did not work: updating MSYS2 itself, then trying again (a few minutes)...'
    [void](Invoke-Msys2 $Root 'pacman-key --init >/dev/null 2>&1; pacman-key --populate msys2 >/dev/null 2>&1; true')
    for ($k = 0; $k -lt 2; $k++) {                    # its core updates first, so twice
        Clear-StaleLock $Root
        [void](Invoke-Msys2 $Root 'pacman -Syuu --noconfirm')
    }
    Clear-StaleLock $Root
    Add-Ucrt64Repo $Root
    [void](Invoke-Msys2 $Root "pacman -Sy --noconfirm --needed $Packages")
    if (Test-Tools $Root) { [void](Invoke-Msys2 $Root "pacman -S --noconfirm --needed $Optional"); return $true }
    return $false
}

function Test-Winget { return [bool](Get-Command winget.exe -ErrorAction SilentlyContinue) }

# A new MSYS2 in $Root: winget, else the official installer.
function Install-Msys2([string]$Root) {
    $default = $Root -ieq 'C:\msys64'
    $ErrorActionPreference = 'Continue'
    if ($default -and (Test-Winget)) {
        Say 'Installing MSYS2 with winget (one time, a few minutes)...'
        & winget.exe install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements 2>&1 | ForEach-Object { Say ([string]$_) }
        if (Test-Msys2 $Root) { return $true }
        # winget thinks it is installed, but the folder is gone: do it again for real
        & winget.exe install --id MSYS2.MSYS2 -e --force --accept-source-agreements --accept-package-agreements 2>&1 | ForEach-Object { Say ([string]$_) }
        if (Test-Msys2 $Root) { return $true }
        Say 'winget did not install it: downloading the installer from MSYS2 itself instead.'
    }
    $exe = Join-Path $env:TEMP 'msys2-x86_64-latest.exe'
    Say "Downloading MSYS2 (about 100 MB) from $InstallerUrl ..."
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $InstallerUrl -OutFile $exe -UseBasicParsing
        $ProgressPreference = $old
    } catch {
        Say "The download failed: $($_.Exception.Message)"
        return $false
    }
    Say "Installing MSYS2 in $Root ..."
    $p = Start-Process -FilePath $exe -ArgumentList @('in', '--confirm-command', '--accept-messages', '--root', ($Root -replace '\\', '/')) -Wait -PassThru
    Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue
    if (-not (Test-Msys2 $Root)) { Say "The MSYS2 installer stopped (code $($p.ExitCode))."; return $false }
    [void](Invoke-Msys2 $Root 'true')                # its first start sets it up
    return $true
}

function Ask-YesNo([string]$Text) {
    if ($env:DQIX_MSYS2_YES) { return $true }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $r = [System.Windows.Forms.MessageBox]::Show($Text, 'Dragon Quest IX - build tools', 'YesNo', 'Question')
        return $r -eq 'Yes'
    } catch { return $false }
}

# Put the old MSYS2 aside (never deleted) and install a fresh one where it was.
function Replace-Msys2([string]$Root) {
    $stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
    $aside = "$Root-old-$stamp"
    $busy = Get-Msys2Processes $Root
    if ($busy.Count -gt 0) {
        Say "MSYS2 is open (close these first: $(($busy | ForEach-Object { $_.ProcessName }) -join ', '))."
        return $false
    }
    try { Rename-Item -LiteralPath $Root -NewName (Split-Path -Leaf $aside) } catch {
        Say "Couldn't rename $Root (is something in it open?): $($_.Exception.Message)"
        return $false
    }
    Say "Your old MSYS2 is kept as $aside (delete it yourself once you don't need it)."
    if (-not (Install-Msys2 $Root)) { return $false }
    return (Install-Tools $Root)
}

function Done([string]$Root) {
    if ($Out) { [IO.File]::WriteAllText($Out, $Root) }
    Say "Build tools: $Root"
    exit 0
}

function Fail([string]$Text) {
    Say ''
    Say $Text
    exit 1
}

if ($Test) { return }

$all = Find-Msys2
if ($all.Count -gt 0) {
    if ($all.Count -gt 1) { Say "MSYS2 found in: $($all -join ', ')" }
    foreach ($r in $all) { if (Test-Tools $r) { Clear-StaleLock $r; Done $r } }   # one that already has everything
    $root = $all[0]
    Say "Using the MSYS2 in $root"
} else {
    $root = 'C:\msys64'
    if (Test-Path -LiteralPath $root) {
        Fail ("There is a C:\msys64 folder, but it isn't a working MSYS2. Rename or remove it, then run this again " +
              "(or put the path of your MSYS2 in msys2-path.txt next to BUILD.bat).")
    }
    if (-not (Install-Msys2 $root)) {
        Fail ("MSYS2 (the free build tools) couldn't be installed automatically. Install it from https://www.msys2.org " +
              "(the default folder, C:\msys64), then run this again.")
    }
}

if (Install-Tools $root) { Done $root }

$msg = "The build tools (MSYS2 in $root) are too old or damaged to update.`n`n" +
       "Install a fresh copy of MSYS2 there? Your current one is kept: it is renamed to $root-old-<date>, nothing is deleted.`n`n" +
       "(No: nothing changes, and the build stops.)"
if (-not (Ask-YesNo $msg)) {
    Fail ("The build tools in $root couldn't be updated, and nothing was changed. Update MSYS2 yourself " +
          "(in its UCRT64 terminal: pacman -Syuu), or run this again and let it install a fresh copy.")
}
if (Replace-Msys2 $root) { Done $root }
Fail ("A fresh MSYS2 couldn't be set up (see above). Install it from https://www.msys2.org, then run this again. " +
      "Your old MSYS2 is in $root-old-* if it was moved.")
