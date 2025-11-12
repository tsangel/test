@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

if not defined BUILD_DIR set "BUILD_DIR=%ROOT_DIR%\build"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if not defined DICOM_BUILD_EXAMPLES set "DICOM_BUILD_EXAMPLES=ON"

where cmake >nul 2>&1
if errorlevel 1 (
	echo Error: cmake is not installed or not on PATH.>&2
	exit /b 1
)

where cl >nul 2>&1
if errorlevel 1 (
	echo Error: cl.exe not found. Run this from a Visual Studio Developer Command Prompt or call vcvarsall.bat first.>&2
	exit /b 1
)

if defined CMAKE_GENERATOR (
	set "GENERATOR=%CMAKE_GENERATOR%"
) else (
	where ninja >nul 2>&1
	if errorlevel 1 (
		set "GENERATOR=NMake Makefiles"
	) else (
		set "GENERATOR=Ninja"
	)
)

echo Configuring dicomsdl (%BUILD_TYPE%) in %BUILD_DIR%
cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -DDICOM_BUILD_EXAMPLES=%DICOM_BUILD_EXAMPLES% -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -G "%GENERATOR%"
if errorlevel 1 exit /b %errorlevel%

set "PARALLEL_SWITCH=--parallel"
if defined BUILD_PARALLELISM set "PARALLEL_SWITCH=--parallel %BUILD_PARALLELISM%"

if "%~1"=="" goto build
set "TARGET_SWITCH=--target"
:target_loop
if "%~1"=="" goto build
set "TARGET_SWITCH=%TARGET_SWITCH% %1"
shift
goto target_loop

:build
echo Building dicomsdl (%BUILD_TYPE%)
cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% %PARALLEL_SWITCH% %TARGET_SWITCH%
exit /b %errorlevel%
