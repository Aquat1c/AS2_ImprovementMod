@echo off
setlocal EnableDelayedExpansion

echo === Alice Senki 2 Improvement Mod Build (CMake) ===
echo.

REM Check for cmake
where cmake >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: cmake not found!
    echo Please install CMake and add it to your PATH.
    pause
    exit /b 1
)

REM Create build directory
if not exist "build" mkdir build

REM Configure
echo [1/3] Configuring project...
cd build
cmake .. -A Win32
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed!
    cd ..
    pause
    exit /b 1
)

REM Build
echo.
echo [2/3] Building project...
cmake --build . --config Release
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed!
    cd ..
    pause
    exit /b 1
)

REM Copy artifacts
echo.
echo [3/3] Copying artifacts...
cd ..
if not exist "bin" mkdir bin

REM Copy d3d9.dll (Proxy)
if exist "build\Release\d3d9.dll" (
    copy /Y "build\Release\d3d9.dll" "bin\d3d9.dll" >nul
    echo Copied d3d9.dll to bin\
) else (
    echo WARNING: d3d9.dll not found in build output!
)

REM Copy as2_rollback.dll (Mod)
if exist "build\Release\as2_rollback.dll" (
    copy /Y "build\Release\as2_rollback.dll" "bin\as2_rollback.dll" >nul
    echo Copied as2_rollback.dll to bin\
) else (
    echo WARNING: as2_rollback.dll not found in build output!
)

echo.
echo Build complete! Artifacts are in the 'bin' directory.
echo.
