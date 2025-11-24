@echo off
rem Build C++ benchmark (read_all_dcm) in RelWithDebInfo with debug info.
setlocal enabledelayedexpansion

set ROOT_DIR=%~dp0
rem strip trailing backslash if present
if "%ROOT_DIR:~-1%"=="\" set ROOT_DIR=%ROOT_DIR:~0,-1%
set BUILD_DIR=%ROOT_DIR%\build
set CONFIG=RelWithDebInfo

cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -A x64 -DCMAKE_BUILD_TYPE=%CONFIG% -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build "%BUILD_DIR%" --config %CONFIG%

set LIB_PATH=
for %%L in ("%BUILD_DIR%\%CONFIG%\dicomsdl.lib" "%BUILD_DIR%\dicomsdl.lib") do (
    if exist %%L set LIB_PATH=%%L
)

if "%LIB_PATH%"=="" (
    echo libdicomsdl.lib not found under %BUILD_DIR%
    exit /b 1
)

set OUT_DIR=%ROOT_DIR%\build.bench
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
set OUT_BIN=%OUT_DIR%\read_all_dcm.exe
echo Using lib: %LIB_PATH%
echo Building %OUT_BIN%

cl /std:c++20 /utf-8 /O2 /Zi /MT /EHsc /I"%ROOT_DIR%\include" /I"%BUILD_DIR%" /I"%BUILD_DIR%\generated\include" "%ROOT_DIR%\benchmarks\read_all_dcm.cpp" "%LIB_PATH%" /Fe:"%OUT_BIN%"

echo Done ^> %OUT_BIN%

endlocal
