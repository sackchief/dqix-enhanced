@echo off
rem Dragon Quest IX for PC: turns your own DQIX ROM into the PC game (the USA version is the tested one).
rem Double-click this. The first time it installs the free MSYS2 build tools
rem (a few minutes, needs the internet); after that it only rebuilds.
rem The result goes in the "game" folder next to this file.
setlocal
cd /d "%~dp0"
title Dragon Quest IX PC - build

rem ---- RecompLauncher (recomplauncher.json "prepare"): BUILD.bat --prepare "<your ROM>"
rem builds from that ROM without asking anything or pausing, and if it fails its
rem last line says why (the launcher shows only that line)
set "PREPARE="
set "ROM="
if /i not "%~1"=="--prepare" goto pick_rom
set "PREPARE=1"
set "DQIX_NOPAUSE=1"
set "ROM=%~f2"
if "%~2"=="" goto prepare_no_rom
if exist "%ROM%" goto check_zip
:prepare_no_rom
echo BUILD FAILED: the ROM wasn't found ("%~2"). Choose your own Dragon Quest IX .nds file, or a .zip with it inside.
exit /b 1

:pick_rom
rem ---- the ROM: the first .nds in the "rom" folder (or a .zip with one in it), or ask for it
for %%f in ("%~dp0rom\*.nds") do if not defined ROM set "ROM=%%~ff"
if defined ROM goto have_rom
for %%f in ("%~dp0rom\*.zip") do if not defined ROM set "ROM=%%~ff"
if defined ROM goto check_zip
rem the ROM chosen last time (updates build without asking again)
if exist "%~dp0rom\last-rom.txt" set /p ROM=<"%~dp0rom\last-rom.txt"
if defined ROM if exist "%ROM%" goto check_zip
set "ROM="
echo Choose your Dragon Quest IX ROM in the window that opens...
for /f "usebackq delims=" %%i in (`powershell -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Filter = 'DS ROM, or a zip with one in it (*.nds;*.zip)|*.nds;*.zip|All files (*.*)|*.*'; $d.Title = 'Choose your Dragon Quest IX ROM'; if ($d.ShowDialog() -eq 'OK') { $d.FileName }"`) do set "ROM=%%i"
if defined ROM goto check_zip
echo No ROM chosen. Put your ROM in the "rom" folder (or choose it) and run this again.
if not defined DQIX_NOPAUSE pause
exit /b 1

:check_zip
rem a ROM in a zip: take it out into the "rom" folder
if /i not "%ROM:~-4%"==".zip" goto have_rom
set "ZIPRES=%TEMP%\dqix-rom-%RANDOM%.txt"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\rom-unzip.ps1" -Zip "%ROM%" -OutDir "%~dp0rom" -Result "%ZIPRES%"
if errorlevel 1 goto failed_rom
set "ROM="
set /p ROM=<"%ZIPRES%"
del "%ZIPRES%" >nul 2>&1
if not defined ROM goto failed_rom

:have_rom
echo ROM: "%ROM%"
if not exist "%~dp0rom" mkdir "%~dp0rom"
>"%~dp0rom\last-rom.txt" echo %ROM%

rem ---- the build tools (MSYS2): tools\msys2-setup.ps1 finds the one on this PC (any
rem drive; or the folder in msys2-path.txt next to this file), repairs an old or
rem broken one, or installs it, and says which it is
echo Checking the build tools (the first time downloads them)...
set "MSYSROOT="
set "MSYSRES=%TEMP%\dqix-msys2-%RANDOM%.txt"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\msys2-setup.ps1" -Out "%MSYSRES%"
if errorlevel 1 goto failed_tools
set /p MSYSROOT=<"%MSYSRES%"
del "%MSYSRES%" >nul 2>&1
if not defined MSYSROOT goto failed_tools

rem ---- build
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
set "DQIX_ROM=%ROM%"
"%MSYSROOT%\usr\bin\bash.exe" -lc "./tools/build-game.sh \"$(cygpath -u \"$DQIX_ROM\")\""
if errorlevel 1 goto failed
echo.
echo Finished. Play with game\dqix.exe (you can make a shortcut to it).
if not defined DQIX_NOPAUSE pause
exit /b 0

:failed
echo.
echo BUILD FAILED. The text above says why.
if defined PREPARE if exist "%~dp0build\last-error.txt" (
    set /p LASTERR=<"%~dp0build\last-error.txt"
    call echo BUILD FAILED while %%LASTERR%%
)
if not defined DQIX_NOPAUSE pause
exit /b 1

:failed_tools
echo.
echo BUILD FAILED: the build tools (MSYS2) aren't ready. The text above says why.
echo Nothing was built; your game, saves and settings are untouched.
if defined PREPARE echo BUILD FAILED: the build tools (MSYS2) couldn't be installed or repaired. Nothing was changed; check the internet connection and try again.
if not defined DQIX_NOPAUSE pause
exit /b 1

:failed_rom
echo.
echo BUILD FAILED: no ROM could be taken out of "%ROM%".
echo Choose the .nds file itself, or a .zip with the .nds in it.
if defined PREPARE echo BUILD FAILED: no Dragon Quest IX ROM was found in the zip. Choose the .nds file itself, or a .zip with the .nds in it.
if not defined DQIX_NOPAUSE pause
exit /b 1
