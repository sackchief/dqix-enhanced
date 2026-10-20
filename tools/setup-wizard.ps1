# Dragon Quest IX for PC - setup.
# A window that walks through everything: choose your ROM, where to install,
# shortcuts, then it builds the game and offers to play. Started by
# "Install Dragon Quest IX.bat" in the project folder. Runs on the PowerShell
# that comes with Windows 10 and 11 (5.1).
#
#   -Test  load the functions only (no window), for checking them
param([switch]$Test)

$ErrorActionPreference = 'Stop'
$Project = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)   # the folder with BUILD.bat
$AppName = 'Dragon Quest IX'
$FolderName = 'DQIX-Enhanced'
$UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\DQIX-Enhanced'

# ---------------------------------------------------------------- checks (no window needed)

# Which ROM is this? Returns @{ Ok; Warn; Text }. Any readable file can be
# tried: the USA and European versions work, the others get a warning.
function Test-Rom([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @{ Ok = $false; Warn = $false; Text = 'Choose your ROM file (it ends in .nds).' } }
    $code = ''
    try {
        $fs = [IO.File]::OpenRead($Path)
        try { $buf = New-Object byte[] 16; [void]$fs.Read($buf, 0, 16); $code = [Text.Encoding]::ASCII.GetString($buf, 12, 4) -replace '[^A-Za-z0-9]', '' } finally { $fs.Close() }
    } catch { return @{ Ok = $false; Warn = $false; Text = "This file can't be read: $($_.Exception.Message)" } }
    switch ($code) {
        'YDQE' { return @{ Ok = $true; Warn = $false; Text = 'Dragon Quest IX (USA).' } }
        'YDQP' { return @{ Ok = $true; Warn = $false; Text = 'Dragon Quest IX (Europe). English, French, German, Italian or Spanish: choose in the Language menu.' } }
        'YDQJ' { return @{ Ok = $true; Warn = $true; Text = "Dragon Quest IX (Japan). Only the USA and European versions are supported: this one may not build or may not run, but you can try." } }
        default { return @{ Ok = $true; Warn = $true; Text = "This doesn't look like a Dragon Quest IX ROM (game code '$code'). It will most likely not build, but you can try." } }
    }
}

function Get-DefaultFolder { return (Join-Path ([Environment]::GetFolderPath('MyDocuments')) $FolderName) }
function Get-LocalFolder { return (Join-Path $env:USERPROFILE $FolderName) }

function Test-Under([string]$Path, [string]$Root) {
    if (-not $Root) { return $false }
    $p = $Path.TrimEnd('\') + '\'; $r = $Root.TrimEnd('\') + '\'
    return $p.StartsWith($r, [StringComparison]::OrdinalIgnoreCase)
}

# Can the game live here? Returns @{ Ok; Text; OneDrive; Existing }.
function Test-Folder([string]$Path) {
    $r = @{ Ok = $true; Text = ''; OneDrive = $false; Existing = $false }
    if (-not $Path) { $r.Ok = $false; $r.Text = 'Choose a folder.'; return $r }
    try { $full = [IO.Path]::GetFullPath($Path) } catch { $r.Ok = $false; $r.Text = 'That is not a folder path.'; return $r }
    if ($full.TrimEnd('\').Length -le 3) { $r.Ok = $false; $r.Text = 'Choose a folder, not a whole drive.'; return $r }
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432, $env:windir, $env:ProgramData)) {
        if (Test-Under $full $root) {
            $r.Ok = $false
            $r.Text = "Windows protects this folder: the game couldn't save there, or update itself. Choose another one, such as your Documents."
            return $r
        }
    }
    foreach ($od in @($env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial)) {
        if (Test-Under $full $od) { $r.OneDrive = $true }
    }
    if (Test-Path -LiteralPath (Join-Path $full 'BUILD.bat')) { $r.Existing = $true }
    try {
        $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($full))
        $freeGB = [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
        if ($drive.AvailableFreeSpace -lt 4GB -and -not $r.Existing) {
            $r.Ok = $false; $r.Text = "This drive has only $freeGB GB free; the game needs about 4 GB while it's built. Free some space or choose another drive."
            return $r
        }
    } catch { }
    $notes = @()
    if ($r.Existing) { $notes += 'The game is already installed here: it will be built again, and your saves and settings are kept.' }
    if ($r.OneDrive) { $notes += 'This folder is synced to OneDrive, which would upload about 3 GB of build files. A folder that is not synced is better.' }
    $r.Text = ($notes -join ' ')
    return $r
}

# Where the game ends up when a folder is picked with "Change...": inside it,
# in a folder of its own (unless it already is one).
function Resolve-PickedFolder([string]$Picked) {
    if (Test-Path -LiteralPath (Join-Path $Picked 'BUILD.bat')) { return $Picked }
    if ((Split-Path -Leaf $Picked) -ieq $FolderName) { return $Picked }
    return (Join-Path $Picked $FolderName)
}

# ---------------------------------------------------------------- doing it

function Copy-Project([string]$Dest) {
    $src = [IO.Path]::GetFullPath($Project).TrimEnd('\')
    $dst = [IO.Path]::GetFullPath($Dest).TrimEnd('\')
    if ($src -ieq $dst) { return }
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    & robocopy.exe $src $dst /E /XD .git build game update /XF last-rom.txt /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying the files to $dst failed (robocopy $LASTEXITCODE)." }
}

function Copy-Rom([string]$Rom, [string]$Dest) {
    $romDir = Join-Path $Dest 'rom'
    New-Item -ItemType Directory -Force -Path $romDir | Out-Null
    Get-ChildItem -LiteralPath $romDir -Filter *.nds -ErrorAction SilentlyContinue | Where-Object { $_.FullName -ne $Rom } | Remove-Item -Force
    $target = Join-Path $romDir 'dqix_usa.nds'
    if ([IO.Path]::GetFullPath($Rom) -ne [IO.Path]::GetFullPath($target)) { Copy-Item -LiteralPath $Rom -Destination $target -Force }
}

function Unblock-Everything([string]$Dest) {
    # files from a downloaded zip carry "came from the internet" marks: remove them, so Windows doesn't ask about each later
    Get-ChildItem -LiteralPath $Dest -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue
}

function New-Shortcut([string]$Lnk, [string]$Target, [string]$Arguments, [string]$WorkDir, [string]$Icon, [string]$Description) {
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($Lnk)
    $s.TargetPath = $Target
    if ($Arguments) { $s.Arguments = $Arguments }
    if ($WorkDir) { $s.WorkingDirectory = $WorkDir }
    if ($Icon) { $s.IconLocation = $Icon }
    if ($Description) { $s.Description = $Description }
    $s.Save()
}

function Get-StartMenuFolder { return (Join-Path ([Environment]::GetFolderPath('Programs')) $AppName) }

function Add-Shortcuts([string]$Dest, [bool]$Desktop, [bool]$StartMenu) {
    $game = Join-Path $Dest 'game'
    $exe = Join-Path $game 'dqix.exe'
    $icon = "$exe,0"
    if ($Desktop) {
        New-Shortcut (Join-Path ([Environment]::GetFolderPath('Desktop')) "$AppName.lnk") $exe '' $game $icon 'Play Dragon Quest IX'
    }
    if ($StartMenu) {
        $sm = Get-StartMenuFolder
        New-Item -ItemType Directory -Force -Path $sm | Out-Null
        New-Shortcut (Join-Path $sm "$AppName.lnk") $exe '' $game $icon 'Play Dragon Quest IX'
        New-Shortcut (Join-Path $sm 'Save files.lnk') $game '' $game '' 'The folder with your save file, settings and bug reports'
        $ps = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
        New-Shortcut (Join-Path $sm "Uninstall $AppName.lnk") $ps "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $Dest 'tools\uninstall.ps1')`"" $Dest '' "Remove $AppName from this PC"
    }
}

function Register-Uninstall([string]$Dest) {
    $ps = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
    New-Item -Force -Path $UninstallKey | Out-Null
    $v = @{
        DisplayName = "$AppName (DQIX Enhanced)"
        DisplayIcon = (Join-Path $Dest 'game\dqix.exe')
        InstallLocation = $Dest
        Publisher = 'DQIX Enhanced'
        UninstallString = "`"$ps`" -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $Dest 'tools\uninstall.ps1')`""
    }
    foreach ($k in $v.Keys) { Set-ItemProperty -Path $UninstallKey -Name $k -Value $v[$k] }
    Set-ItemProperty -Path $UninstallKey -Name NoModify -Value 1 -Type DWord
    Set-ItemProperty -Path $UninstallKey -Name NoRepair -Value 1 -Type DWord
    try {
        $kb = [int]((Get-ChildItem -LiteralPath $Dest -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1KB)
        Set-ItemProperty -Path $UninstallKey -Name EstimatedSize -Value $kb -Type DWord
    } catch { }
}

# The build's progress from its output: @{ Step (0-4); Percent; Line }.
# Steps: 0 ROM, 1 build tools, 2 recompiling, 3 compiling, 4 finishing.
$CompileEstimate = 140          # compile commands in a first build (the game and its library)
function Read-Progress([string]$Log) {
    $p = @{ Step = 0; Percent = 0; Line = ''; Compiled = 0; Done = $false }
    foreach ($line in ($Log -split "`r?`n")) {
        if ($line -match '^ROM: ') { $p.Step = 1 }
        elseif ($line -match 'Installing MSYS2|Checking the build tools') { $p.Step = 1 }
        elseif ($line -match '^== Checking the ROM|^== Extracting|^== Recompiling') { $p.Step = 2 }
        elseif ($line -match '^== Compiling') { $p.Step = 3 }
        elseif ($line -match '^== Putting the game folder together') { $p.Step = 4 }
        elseif ($line -match '^== Done') { $p.Step = 4; $p.Done = $true }
        if ($p.Step -eq 3 -and $line -match '(gcc|clang|cc)(\.exe)? .* -c ') { $p.Compiled++ }
        # what to show under the bar: the build's own headings, not the compilers' chatter
        if ($line -match '^(== |ROM: |Installing |Checking |   [A-Z])') { $p.Line = $line.Trim().TrimStart('=', ' ') }
    }
    $p.Percent = [math]::Min(99, [int](100 * $p.Compiled / $CompileEstimate))
    if ($p.Step -eq 3 -and $p.Compiled) { $p.Line = "Compiled $($p.Compiled) of about $CompileEstimate parts" }
    return $p
}

if ($Test) { return }

# ---------------------------------------------------------------- the window
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
try { Add-Type -Name Dpi -Namespace Native -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'; [void][Native.Dpi]::SetProcessDPIAware() } catch { }
[System.Windows.Forms.Application]::EnableVisualStyles()
trap {
    [void][System.Windows.Forms.MessageBox]::Show("Setup hit a problem: $($_.Exception.Message)`n`nRun BUILD.bat instead, or send this message with a bug report.", 'Dragon Quest IX - Setup', 'OK', 'Error')
    exit 1
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "$AppName - Setup"
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$g = $form.CreateGraphics(); $scale = $g.DpiX / 96.0; $g.Dispose()
function S([double]$n) { return [int]($n * $scale) }
$form.ClientSize = New-Object System.Drawing.Size((S 640), (S 470))
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
try { $form.Icon = [System.Drawing.SystemIcons]::Application } catch { }

$header = New-Object System.Windows.Forms.Panel
$header.SetBounds(0, 0, (S 640), (S 70)); $header.BackColor = [System.Drawing.Color]::FromArgb(24, 48, 96)
$form.Controls.Add($header)
$title = New-Object System.Windows.Forms.Label
$title.SetBounds((S 20), (S 12), (S 600), (S 30)); $title.ForeColor = 'White'
$title.Font = New-Object System.Drawing.Font('Segoe UI', 15, [System.Drawing.FontStyle]::Bold)
$header.Controls.Add($title)
$subtitle = New-Object System.Windows.Forms.Label
$subtitle.SetBounds((S 22), (S 42), (S 600), (S 22)); $subtitle.ForeColor = [System.Drawing.Color]::FromArgb(200, 215, 240)
$header.Controls.Add($subtitle)

$body = New-Object System.Windows.Forms.Panel
$body.SetBounds((S 20), (S 85), (S 600), (S 320))
$form.Controls.Add($body)

$btnBack = New-Object System.Windows.Forms.Button; $btnBack.Text = '< Back'; $btnBack.SetBounds((S 330), (S 425), (S 90), (S 30))
$btnNext = New-Object System.Windows.Forms.Button; $btnNext.Text = 'Next >'; $btnNext.SetBounds((S 425), (S 425), (S 100), (S 30))
$btnCancel = New-Object System.Windows.Forms.Button; $btnCancel.Text = 'Cancel'; $btnCancel.SetBounds((S 530), (S 425), (S 90), (S 30))
$form.Controls.AddRange(@($btnBack, $btnNext, $btnCancel))
$form.AcceptButton = $btnNext

function New-Label([string]$Text, [int]$Y, [int]$H, [switch]$Bold) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $Text; $l.SetBounds(0, $Y, (S 600), $H); $l.AutoSize = $false
    if ($Bold) { $l.Font = New-Object System.Drawing.Font('Segoe UI', 10, [System.Drawing.FontStyle]::Bold) }
    $body.Controls.Add($l); return $l
}

$state = @{ Page = 0; Rom = ''; RomOk = $false; Folder = (Get-DefaultFolder); Desktop = $true; StartMenu = $true; Proc = $null; LogPath = ''; Finished = $false }

# ---- pages
function Show-Welcome {
    $title.Text = "Welcome to $AppName for PC"
    $subtitle.Text = 'This setup turns your own copy of the game into a PC game.'
    [void](New-Label ("You'll need:`n" +
        "  - a copy (ROM, a .nds file) of your own Dragon Quest IX cartridge (the USA or European version)`n" +
        "  - an internet connection, and about 4 GB of free space`n" +
        "  - 10 to 20 minutes the first time (the build tools are installed once)") 0 (S 100))
    [void](New-Label ("What happens: you choose your ROM and where to put the game, then setup downloads free build " +
        "tools (MSYS2) and builds the game from your ROM, right here on your PC. Nothing from the game is downloaded: " +
        "everything comes from your own cartridge.") (S 110) (S 80))
    [void](New-Label "Windows may ask for permission to install the build tools: choose Yes." (S 200) (S 40))
    $btnBack.Enabled = $false; $btnNext.Text = 'Next >'; $btnNext.Enabled = $true
}

function Show-Rom {
    $title.Text = 'Your Dragon Quest IX ROM'
    $subtitle.Text = 'Choose the .nds file made from your cartridge, or drag it into this window.'
    [void](New-Label 'ROM file:' 0 (S 24) -Bold)
    $box = New-Object System.Windows.Forms.TextBox; $box.SetBounds(0, (S 28), (S 480), (S 26)); $box.ReadOnly = $true; $box.Text = $state.Rom
    $body.Controls.Add($box)
    $browse = New-Object System.Windows.Forms.Button; $browse.Text = 'Browse...'; $browse.SetBounds((S 490), (S 26), (S 110), (S 30))
    $body.Controls.Add($browse)
    $status = New-Label '' (S 70) (S 90)
    $check = {
        param($path)
        $state.Rom = $path; $box.Text = $path
        $status.ForeColor = 'Black'; $status.Text = 'Checking the file...'; $form.Refresh()
        $r = Test-Rom $path
        $state.RomOk = $r.Ok
        $status.Text = $r.Text
        if (-not $r.Ok) { $status.ForeColor = [System.Drawing.Color]::DarkRed } elseif ($r.Warn) { $status.ForeColor = [System.Drawing.Color]::DarkOrange } else { $status.ForeColor = [System.Drawing.Color]::DarkGreen }
        $btnNext.Enabled = $r.Ok
    }.GetNewClosure()
    $browse.Add_Click({
        $d = New-Object System.Windows.Forms.OpenFileDialog
        $d.Filter = 'DS ROM (*.nds)|*.nds|All files (*.*)|*.*'; $d.Title = 'Choose your Dragon Quest IX ROM'
        if ($d.ShowDialog($form) -eq 'OK') { & $check $d.FileName }
    }.GetNewClosure())
    $script:dropCheck = $check
    [void](New-Label "Only a ROM you made from your own cartridge. It's copied into the game's folder, so updates can rebuild the game later." (S 170) (S 50))
    $btnBack.Enabled = $true; $btnNext.Text = 'Next >'; $btnNext.Enabled = $state.RomOk
    if ($state.Rom) { & $check $state.Rom }
}

function Show-Folder {
    $title.Text = 'Where to put the game'
    $subtitle.Text = 'The game, its saves and settings all live in this folder.'
    [void](New-Label 'Folder:' 0 (S 24) -Bold)
    $box = New-Object System.Windows.Forms.TextBox; $box.SetBounds(0, (S 28), (S 480), (S 26)); $box.Text = $state.Folder
    $body.Controls.Add($box)
    $change = New-Object System.Windows.Forms.Button; $change.Text = 'Change...'; $change.SetBounds((S 490), (S 26), (S 110), (S 30))
    $body.Controls.Add($change)
    $status = New-Label '' (S 70) (S 90)
    $alt = New-Object System.Windows.Forms.Button; $alt.SetBounds(0, (S 165), (S 400), (S 30)); $alt.Visible = $false
    $body.Controls.Add($alt)
    $validate = {
        $state.Folder = $box.Text.Trim()
        $r = Test-Folder $state.Folder
        $status.Text = $r.Text
        if (-not $r.Ok) { $status.ForeColor = [System.Drawing.Color]::DarkRed } elseif ($r.OneDrive) { $status.ForeColor = [System.Drawing.Color]::DarkOrange } else { $status.ForeColor = 'Black' }
        $alt.Visible = $r.Ok -and $r.OneDrive
        $alt.Text = "Use $(Get-LocalFolder) instead"
        $btnNext.Enabled = $r.Ok
    }.GetNewClosure()
    $box.Add_TextChanged($validate)
    $change.Add_Click({
        $d = New-Object System.Windows.Forms.FolderBrowserDialog
        $d.Description = 'Choose where to put the game (a folder called DQIX-Enhanced is made inside it).'
        $d.ShowNewFolderButton = $true
        if ($d.ShowDialog($form) -eq 'OK') { $box.Text = Resolve-PickedFolder $d.SelectedPath }
    }.GetNewClosure())
    $alt.Add_Click({ $box.Text = Get-LocalFolder }.GetNewClosure())
    $btnBack.Enabled = $true; $btnNext.Text = 'Next >'
    & $validate
}

function Show-Shortcuts {
    $title.Text = 'Shortcuts'
    $subtitle.Text = 'How you start the game.'
    $c1 = New-Object System.Windows.Forms.CheckBox; $c1.Text = 'A shortcut on the desktop'; $c1.SetBounds(0, 0, (S 600), (S 28)); $c1.Checked = $state.Desktop
    $c2 = New-Object System.Windows.Forms.CheckBox; $c2.Text = 'In the Start menu (with your save folder and Uninstall)'; $c2.SetBounds(0, (S 32), (S 600), (S 28)); $c2.Checked = $state.StartMenu
    $c1.Add_CheckedChanged({ $state.Desktop = $c1.Checked }.GetNewClosure())
    $c2.Add_CheckedChanged({ $state.StartMenu = $c2.Checked }.GetNewClosure())
    $body.Controls.AddRange(@($c1, $c2))
    [void](New-Label ("It also appears in Settings > Apps, where it can be uninstalled like any program.`n`n" +
        "Ready? Setup will now copy the files to:`n  $($state.Folder)`nand build the game. The first time takes 10 to 20 minutes.") (S 80) (S 120))
    $btnBack.Enabled = $true; $btnNext.Text = 'Install'; $btnNext.Enabled = $true
}

$steps = @('Checking your ROM', 'Build tools (installed once)', 'Recompiling the game', 'Compiling', 'Finishing')
function Show-Install {
    $title.Text = 'Installing'
    $subtitle.Text = 'This takes a while the first time. You can leave it running.'
    $script:stepLabels = @()
    for ($i = 0; $i -lt $steps.Count; $i++) {
        $l = New-Label ("     " + $steps[$i]) ((S 30) * $i) (S 26)
        $l.ForeColor = 'Gray'
        $script:stepLabels += $l
    }
    $script:bar = New-Object System.Windows.Forms.ProgressBar; $bar.SetBounds(0, (S 160), (S 600), (S 22)); $bar.Style = 'Marquee'
    $body.Controls.Add($bar)
    $script:lastLine = New-Label '' (S 188) (S 24)
    $lastLine.ForeColor = 'Gray'
    $script:details = New-Object System.Windows.Forms.TextBox
    $details.Multiline = $true; $details.ScrollBars = 'Vertical'; $details.ReadOnly = $true; $details.SetBounds(0, (S 215), (S 600), (S 105)); $details.Visible = $false
    $details.Font = New-Object System.Drawing.Font('Consolas', 8)
    $body.Controls.Add($details)
    $show = New-Object System.Windows.Forms.LinkLabel; $show.Text = 'Show details'; $show.SetBounds((S 480), (S 188), (S 120), (S 22)); $show.TextAlign = 'TopRight'
    $show.Add_LinkClicked({ $details.Visible = -not $details.Visible }.GetNewClosure())
    $body.Controls.Add($show)
    $btnBack.Enabled = $false; $btnNext.Enabled = $false; $btnNext.Text = 'Next >'
    $form.Refresh()
    Start-Install
}

function Set-Step([int]$Step, [switch]$Failed) {
    for ($i = 0; $i -lt $stepLabels.Count; $i++) {
        $mark = '     '
        if ($i -lt $Step) { $mark = '  ' + [char]0x2714 + '  '; $stepLabels[$i].ForeColor = [System.Drawing.Color]::DarkGreen }
        elseif ($i -eq $Step) { $mark = '  ' + [char]0x25B6 + '  '; $stepLabels[$i].ForeColor = 'Black'; if ($Failed) { $mark = '  ' + [char]0x2716 + '  '; $stepLabels[$i].ForeColor = [System.Drawing.Color]::DarkRed } }
        else { $stepLabels[$i].ForeColor = 'Gray' }
        $stepLabels[$i].Text = $mark + $steps[$i]
    }
}

function Start-Install {
    Set-Step 0
    $dest = $state.Folder
    try {
        Copy-Project $dest
        Copy-Rom $state.Rom $dest
        Unblock-Everything $dest
    } catch {
        Set-Step 0 -Failed
        Show-Failed "Setup couldn't copy the files: $($_.Exception.Message)"
        return
    }
    $state.LogPath = Join-Path $dest 'setup-log.txt'
    Set-Content -LiteralPath $state.LogPath -Value '' -Encoding ASCII
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:windir 'System32\cmd.exe'
    $psi.Arguments = "/c `"`"$(Join-Path $dest 'BUILD.bat')`" > `"$($state.LogPath)`" 2>&1`""
    $psi.WorkingDirectory = $dest
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables['DQIX_NOPAUSE'] = '1'
    $state.Proc = [System.Diagnostics.Process]::Start($psi)
    $script:timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 500
    $timer.Add_Tick({ Update-Install })
    $timer.Start()
}

function Read-Log {
    try {
        $fs = New-Object IO.FileStream($state.LogPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try { $sr = New-Object IO.StreamReader($fs); return $sr.ReadToEnd() } finally { $fs.Close() }
    } catch { return '' }
}

function Update-Install {
    $log = Read-Log
    $p = Read-Progress $log
    Set-Step $p.Step
    if ($p.Step -eq 3) { $bar.Style = 'Continuous'; $bar.Value = $p.Percent } else { $bar.Style = 'Marquee' }
    if ($p.Line) { $lastLine.Text = $p.Line.Substring(0, [math]::Min(90, $p.Line.Length)) }
    if ($details.Visible) {
        $tail = ($log -split "`r?`n" | Select-Object -Last 200) -join "`r`n"
        if ($details.Text -ne $tail) { $details.Text = $tail; $details.SelectionStart = $details.TextLength; $details.ScrollToCaret() }
    }
    if ($state.Proc -and $state.Proc.HasExited) {
        $timer.Stop()
        if ($state.Proc.ExitCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $state.Folder 'game\dqix.exe'))) {
            try {
                Add-Shortcuts $state.Folder $state.Desktop $state.StartMenu
                Register-Uninstall $state.Folder
            } catch { }
            $state.Finished = $true
            Set-Step 5
            $bar.Style = 'Continuous'; $bar.Value = 100
            Show-Page 5
        } else {
            Set-Step $p.Step -Failed
            Show-Failed "The build stopped. The details below say why; the whole log is in $($state.LogPath)."
        }
    }
}

function Show-Failed([string]$Message) {
    $subtitle.Text = 'Something went wrong.'
    $lastLine.Text = $Message; $lastLine.ForeColor = [System.Drawing.Color]::DarkRed
    $lastLine.SetBounds(0, (S 188), (S 600), (S 44))
    $details.Visible = $true; $details.SetBounds(0, (S 235), (S 600), (S 85))
    $details.Text = ((Read-Log) -split "`r?`n" | Select-Object -Last 200) -join "`r`n"
    $details.SelectionStart = $details.TextLength; $details.ScrollToCaret()
    $btnNext.Text = 'Try again'; $btnNext.Enabled = $true
    $btnCancel.Text = 'Close'
    $state.Failed = $true
}

function Show-Done {
    $title.Text = "$AppName is ready!"
    $subtitle.Text = 'Have fun on your adventure.'
    $where = @()
    if ($state.Desktop) { $where += 'the desktop' }
    if ($state.StartMenu) { $where += 'the Start menu' }
    $start = 'Start it with the button below'
    if ($where.Count) { $start += ', or later from ' + ($where -join ' or ') }
    [void](New-Label "$start." 0 (S 30))
    [void](New-Label 'Tip: to pin it to the taskbar or to Start, right-click the Dragon Quest IX shortcut and choose "Pin to taskbar" or "Pin to Start". (Windows only lets you do that yourself.)' (S 40) (S 60))
    [void](New-Label "Your saves and settings are in $(Join-Path $state.Folder 'game'). Updates come from inside the game: Help > Check for updates." (S 110) (S 60))
    [void](New-Label 'If you downloaded a zip to get this setup, you can delete it and the folder you unpacked: the game is installed now.' (S 180) (S 50))
    $btnBack.Enabled = $false
    $btnNext.Text = 'Play now'; $btnNext.Enabled = $true
    $btnCancel.Text = 'Close'
}

function Show-Page([int]$Page) {
    $state.Page = $Page
    $body.Controls.Clear()
    switch ($Page) { 0 { Show-Welcome } 1 { Show-Rom } 2 { Show-Folder } 3 { Show-Shortcuts } 4 { Show-Install } 5 { Show-Done } }
}

$btnBack.Add_Click({ if ($state.Page -gt 0 -and $state.Page -le 3) { Show-Page ($state.Page - 1) } })
$btnNext.Add_Click({
    if ($state.Page -eq 5) {
        $game = Join-Path $state.Folder 'game'
        Start-Process -FilePath (Join-Path $game 'dqix.exe') -WorkingDirectory $game
        $form.Close(); return
    }
    if ($state.Page -eq 4 -and $state.Failed) { $state.Failed = $false; $btnCancel.Text = 'Cancel'; Show-Page 4; return }
    if ($state.Page -lt 4) { Show-Page ($state.Page + 1) }
})
$btnCancel.Add_Click({ $form.Close() })
$form.Add_FormClosing({
    param($s, $e)
    if ($state.Proc -and -not $state.Proc.HasExited) {
        $a = [System.Windows.Forms.MessageBox]::Show($form, 'Stop setup? The game is not finished yet; you can run setup again later.', $AppName, 'YesNo', 'Question')
        if ($a -ne 'Yes') { $e.Cancel = $true; return }
        try { & taskkill.exe /T /F /PID $state.Proc.Id | Out-Null } catch { }
    }
})
$form.AllowDrop = $true
$form.Add_DragEnter({ param($s, $e) if ($state.Page -eq 1 -and $e.Data.GetDataPresent([Windows.Forms.DataFormats]::FileDrop)) { $e.Effect = 'Copy' } })
$form.Add_DragDrop({ param($s, $e) $f = $e.Data.GetData([Windows.Forms.DataFormats]::FileDrop); if ($f -and $script:dropCheck) { & $script:dropCheck $f[0] } })

Show-Page 0
[void]$form.ShowDialog()
