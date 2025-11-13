@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

if not defined BUILD_DIR set "BUILD_DIR=%ROOT_DIR%\build"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if not defined DICOM_BUILD_EXAMPLES set "DICOM_BUILD_EXAMPLES=ON"
if not defined BUILD_WHEEL set "BUILD_WHEEL=1"
if not defined WHEEL_DIR set "WHEEL_DIR=%ROOT_DIR%\dist"
if not defined PYTHON_BIN (
	where py >nul 2>&1
	if errorlevel 1 (
		set "PYTHON_BIN=python"
	) else (
		set "PYTHON_BIN=py -3"
	)
)

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
if errorlevel 1 exit /b %errorlevel%

if not "%RUN_TESTS%"=="0" (
	echo Running CTest suite (%BUILD_TYPE%)
	pushd "%BUILD_DIR%"
	ctest --output-on-failure -C %BUILD_TYPE%
	set "CTEST_ERROR=%errorlevel%"
	popd
	if not "%CTEST_ERROR%"=="0" exit /b %CTEST_ERROR%
)

if not "%BUILD_WHEEL%"=="0" (
	echo Building Python wheel into %WHEEL_DIR%
	if not exist "%WHEEL_DIR%" mkdir "%WHEEL_DIR%"
	%PYTHON_BIN% -m pip wheel "%ROOT_DIR%" --no-build-isolation --no-deps -w "%WHEEL_DIR%"
	if errorlevel 1 exit /b %errorlevel%
)

exit /b 0
