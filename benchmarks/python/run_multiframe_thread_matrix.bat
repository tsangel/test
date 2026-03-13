@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "BENCH_SCRIPT=%SCRIPT_DIR%\benchmark_multiframe_thread_matrix.py"

if "%PYTHON_BIN%"=="" set "PYTHON_BIN=python"
if "%WORKER_THREADS%"=="" set "WORKER_THREADS=1,2,4,8,all"
if "%CODEC_THREADS%"=="" set "CODEC_THREADS=1,2"
if "%WARMUP%"=="" set "WARMUP=1"
if "%REPEAT%"=="" set "REPEAT=4"
if "%TARGET_SAMPLE_MS%"=="" set "TARGET_SAMPLE_MS=150"
if "%MAX_INNER_LOOPS%"=="" set "MAX_INNER_LOOPS=128"
if "%HTJ2K_BACKEND%"=="" set "HTJ2K_BACKEND=openjph"

"%PYTHON_BIN%" --version >nul 2>nul
if errorlevel 1 (
  echo ERROR: Python executable is not available: %PYTHON_BIN%
  exit /b 1
)

if "%~1"=="" (
  echo Usage: %~nx0 INPUT1 [INPUT2 ...] [extra benchmark args]
  echo.
  echo Environment defaults:
  echo   PYTHON_BIN=%PYTHON_BIN%
  echo   WORKER_THREADS=%WORKER_THREADS%
  echo   CODEC_THREADS=%CODEC_THREADS%
  echo   WARMUP=%WARMUP%
  echo   REPEAT=%REPEAT%
  echo   TARGET_SAMPLE_MS=%TARGET_SAMPLE_MS%
  echo   MAX_INNER_LOOPS=%MAX_INNER_LOOPS%
  echo   HTJ2K_BACKEND=%HTJ2K_BACKEND%
  echo.
  echo Example:
  echo   %~nx0 ..\sample\multiframe\multiframe.dcm --transfer-syntax JPEG2000
  echo.
  "%PYTHON_BIN%" "%BENCH_SCRIPT%" --help
  exit /b 1
)

echo Using Python: %PYTHON_BIN%
echo WORKER_THREADS/CODEC_THREADS: %WORKER_THREADS% / %CODEC_THREADS%
echo WARMUP/REPEAT: %WARMUP% / %REPEAT%
echo HTJ2K_BACKEND: %HTJ2K_BACKEND%
echo.

"%PYTHON_BIN%" "%BENCH_SCRIPT%" ^
  --worker-threads "%WORKER_THREADS%" ^
  --codec-threads "%CODEC_THREADS%" ^
  --warmup %WARMUP% ^
  --repeat %REPEAT% ^
  --target-sample-ms %TARGET_SAMPLE_MS% ^
  --max-inner-loops %MAX_INNER_LOOPS% ^
  --htj2k-backend %HTJ2K_BACKEND% ^
  %*
exit /b %errorlevel%
