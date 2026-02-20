@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..\..") do set "ROOT_DIR=%%~fI"

set "BENCH_SCRIPT=%SCRIPT_DIR%\benchmark_wg04_pixel_decode.py"
set "PRINT_SCRIPT=%SCRIPT_DIR%\print_wg04_tables.py"

if "%WARMUP%"=="" set "WARMUP=1"
if "%REPEAT%"=="" set "REPEAT=3"
if "%USE_INSTALLED_DICOMSDL%"=="" set "USE_INSTALLED_DICOMSDL=0"

if "%MAIN_JSON%"=="" set "MAIN_JSON=%ROOT_DIR%\build\wg04_pixel_decode_compare_r3_htj2k.json"
if "%HTJ2K_OPENJPEG_JSON%"=="" set "HTJ2K_OPENJPEG_JSON=%ROOT_DIR%\build\wg04_htj2k_openjpeg_r3_toarray_postopt.json"
if "%HTJ2K_OPENJPH_JSON%"=="" set "HTJ2K_OPENJPH_JSON=%ROOT_DIR%\build\wg04_htj2k_openjph_r3_toarray_postopt.json"

if "%PYTHON_BIN%"=="" set "PYTHON_BIN=python"

"%PYTHON_BIN%" --version >nul 2>nul
if errorlevel 1 (
  echo ERROR: Python executable is not available: %PYTHON_BIN%
  exit /b 1
)

if "%USE_INSTALLED_DICOMSDL%"=="1" (
  set "DICOMSDL_IMPORT_SOURCE=installed"
) else (
  if defined PYTHONPATH (
    set "PYTHONPATH=%ROOT_DIR%\bindings\python;%PYTHONPATH%"
  ) else (
    set "PYTHONPATH=%ROOT_DIR%\bindings\python"
  )
  set "DICOMSDL_IMPORT_SOURCE=workspace"
)

if defined WG04_ROOT (
  set "ROOT_ARG=%WG04_ROOT%"
) else (
  set "ROOT_ARG="
)

echo Using Python: %PYTHON_BIN%
echo DICOMSDL source: %DICOMSDL_IMPORT_SOURCE%
echo WARMUP/REPEAT: %WARMUP%/%REPEAT%
if defined WG04_ROOT echo WG04_ROOT: %WG04_ROOT%

echo.
echo [1/4] Full WG04 benchmark ^(dicomsdl + pydicom^)
if defined WG04_ROOT (
  "%PYTHON_BIN%" "%BENCH_SCRIPT%" "%ROOT_ARG%" --backend both --warmup %WARMUP% --repeat %REPEAT% --json "%MAIN_JSON%"
) else (
  "%PYTHON_BIN%" "%BENCH_SCRIPT%" --backend both --warmup %WARMUP% --repeat %REPEAT% --json "%MAIN_JSON%"
)
if errorlevel 1 exit /b 1

echo.
echo [2/4] HTJ2K benchmark ^(openjpeg^)
if defined WG04_ROOT (
  "%PYTHON_BIN%" "%BENCH_SCRIPT%" "%ROOT_ARG%" --backend dicomsdl --codec htj2kll --codec htj2kly --dicomsdl-mode to_array --dicomsdl-htj2k-decoder openjpeg --warmup %WARMUP% --repeat %REPEAT% --json "%HTJ2K_OPENJPEG_JSON%"
) else (
  "%PYTHON_BIN%" "%BENCH_SCRIPT%" --backend dicomsdl --codec htj2kll --codec htj2kly --dicomsdl-mode to_array --dicomsdl-htj2k-decoder openjpeg --warmup %WARMUP% --repeat %REPEAT% --json "%HTJ2K_OPENJPEG_JSON%"
)
if errorlevel 1 exit /b 1

echo.
echo [3/4] HTJ2K benchmark ^(openjph^)
if defined WG04_ROOT (
  "%PYTHON_BIN%" "%BENCH_SCRIPT%" "%ROOT_ARG%" --backend dicomsdl --codec htj2kll --codec htj2kly --dicomsdl-mode to_array --dicomsdl-htj2k-decoder openjph --warmup %WARMUP% --repeat %REPEAT% --json "%HTJ2K_OPENJPH_JSON%"
) else (
  "%PYTHON_BIN%" "%BENCH_SCRIPT%" --backend dicomsdl --codec htj2kll --codec htj2kly --dicomsdl-mode to_array --dicomsdl-htj2k-decoder openjph --warmup %WARMUP% --repeat %REPEAT% --json "%HTJ2K_OPENJPH_JSON%"
)
if errorlevel 1 exit /b 1

echo.
echo [4/4] Markdown tables
"%PYTHON_BIN%" "%PRINT_SCRIPT%" --main-json "%MAIN_JSON%" --htj2k-openjpeg-json "%HTJ2K_OPENJPEG_JSON%" --htj2k-openjph-json "%HTJ2K_OPENJPH_JSON%"
if errorlevel 1 exit /b 1

echo.
echo Done.
echo   main JSON: %MAIN_JSON%
echo   HTJ2K openjpeg JSON: %HTJ2K_OPENJPEG_JSON%
echo   HTJ2K openjph JSON: %HTJ2K_OPENJPH_JSON%

endlocal
