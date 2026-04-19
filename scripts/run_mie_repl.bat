@echo off
rem run_mie_repl.bat — double-click launcher for the MIE REPL on Windows.
rem Opens cmd.exe sized for the REPL layout, chcp to UTF-8, then launches
rem mie_repl.exe with both Chinese and English dictionaries loaded.
rem SPDX-License-Identifier: MIT

setlocal
cd /d "%~dp0.."
chcp 65001 > nul
mode con cols=60 lines=32

set "EXE=build\mie-host\Debug\mie_repl.exe"
set "ZH_DAT=firmware\mie\data\dict_dat.bin"
set "ZH_VAL=firmware\mie\data\dict_values.bin"
set "EN_DAT=firmware\mie\data\en_dat.bin"
set "EN_VAL=firmware\mie\data\en_values.bin"

set MISSING=
for %%F in ("%EXE%" "%ZH_DAT%" "%ZH_VAL%" "%EN_DAT%" "%EN_VAL%") do (
    if not exist %%F (
        echo missing: %%~F
        set MISSING=1
    )
)
if defined MISSING (
    echo.
    echo Run the /build-mie skill to build the REPL, and /gen-data to
    echo regenerate the dictionary assets, then double-click this again.
    echo.
    pause
    exit /b 1
)

"%EXE%" --dat "%ZH_DAT%" --val "%ZH_VAL%" --en-dat "%EN_DAT%" --en-val "%EN_VAL%"

if errorlevel 1 (
    echo.
    echo REPL exited with errorlevel %errorlevel%.
    pause
)

endlocal
