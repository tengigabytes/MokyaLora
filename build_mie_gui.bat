@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1

set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set SRC=%~dp0firmware\mie
set BUILD=%~dp0build\mie-gui

echo [1/2] Configuring (first run downloads ImGui + SDL2 from GitHub)...
%CMAKE% -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMIE_BUILD_GUI=ON
if errorlevel 1 ( echo Configure FAILED & exit /b 1 )

echo [2/2] Building...
%CMAKE% --build "%BUILD%" --target mie_gui
if errorlevel 1 ( echo Build FAILED & exit /b 1 )

echo.
echo === Build successful ===
echo Run: build\mie-gui\mie_gui.exe
echo  Or: build\mie-gui\mie_gui.exe --dat firmware\mie\data\dict_dat.bin --val firmware\mie\data\dict_values.bin
