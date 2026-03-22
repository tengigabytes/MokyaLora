@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1

set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set NINJA="C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set SRC=C:\Users\user\Documents\Workspace\MokyaLora\firmware\mie
set BUILD=C:\Users\user\Documents\Workspace\MokyaLora\build\mie-host

echo [1/2] Configuring...
%CMAKE% -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 ( echo Configure FAILED & exit /b 1 )

echo [2/2] Building...
%CMAKE% --build "%BUILD%"
if errorlevel 1 ( echo Build FAILED & exit /b 1 )

echo.
echo === Running tests ===
set CTEST="C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
%CTEST% --test-dir "%BUILD%" --output-on-failure -C Debug
