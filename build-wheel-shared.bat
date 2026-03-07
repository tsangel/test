@echo off
setlocal EnableExtensions

set "ROOT_DIR=%~dp0"

if not defined PYTHON_BIN set "PYTHON_BIN=python"
if not defined DICOMSDL_WINDOWS_TOOLCHAIN set "DICOMSDL_WINDOWS_TOOLCHAIN=msvc"
if not defined BUILD_DIR set "BUILD_DIR=%ROOT_DIR%build-wheel-shared"
if not defined WHEEL_DIR set "WHEEL_DIR=%ROOT_DIR%dist-shared"
if not defined CLEAN_BUILD_DIR set "CLEAN_BUILD_DIR=1"
if not defined RESET_CMAKE_CACHE set "RESET_CMAKE_CACHE=1"
if not defined BUILD_TESTING set "BUILD_TESTING=OFF"
if not defined DICOM_BUILD_EXAMPLES set "DICOM_BUILD_EXAMPLES=OFF"
if not defined RUN_TESTS set "RUN_TESTS=0"
if not defined BUILD_WHEEL set "BUILD_WHEEL=1"
if not defined WHEEL_ONLY set "WHEEL_ONLY=1"
if not defined PIP_WHEEL_VERBOSE set "PIP_WHEEL_VERBOSE=1"
if not defined DICOMSDL_CLEAN_BUILD set "DICOMSDL_CLEAN_BUILD=1"
if not defined FORCE_WHEEL_RELEASE set "FORCE_WHEEL_RELEASE=1"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if not defined DEBUG set "DEBUG=0"
if not defined DISTUTILS_DEBUG set "DISTUTILS_DEBUG=0"
if not defined STATIC_PRE_CLEAN_OUTPUTS set "STATIC_PRE_CLEAN_OUTPUTS=1"

set "BUILD_DIR=%BUILD_DIR:"=%"
set "WHEEL_DIR=%WHEEL_DIR:"=%"

if not defined DICOMSDL_PIXEL_DEFAULT_MODE set "DICOMSDL_PIXEL_DEFAULT_MODE=none"
if not defined DICOMSDL_PIXEL_JPEG_MODE set "DICOMSDL_PIXEL_JPEG_MODE=shared"
if not defined DICOMSDL_PIXEL_JPEGLS_MODE set "DICOMSDL_PIXEL_JPEGLS_MODE=shared"
if not defined DICOMSDL_PIXEL_JPEG2K_MODE set "DICOMSDL_PIXEL_JPEG2K_MODE=shared"
if not defined DICOMSDL_PIXEL_HTJ2K_MODE set "DICOMSDL_PIXEL_HTJ2K_MODE=shared"
if not defined DICOMSDL_PIXEL_JPEGXL_MODE set "DICOMSDL_PIXEL_JPEGXL_MODE=shared"

if not defined CMAKE_EXTRA_ARGS set "CMAKE_EXTRA_ARGS=-DDICOM_BUILD_PYTHON=OFF -DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=OFF"
if not defined DICOMSDL_CMAKE_ARGS set "DICOMSDL_CMAKE_ARGS=-DDICOMSDL_ENABLE_JPEGXL=ON -DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEG_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_OPENJPEG_PLUGIN=ON -DDICOMSDL_PIXEL_JPEG_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGLS_PLUGIN=ON -DDICOMSDL_PIXEL_HTJ2K_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGXL_PLUGIN=ON"

if not "%STATIC_PRE_CLEAN_OUTPUTS%"=="0" (
	call :preclean_outputs
	if errorlevel 1 (
		set "EXIT_CODE=%ERRORLEVEL%"
		goto finalize
	)
)

call "%ROOT_DIR%build.bat" %*
set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" goto finalize

:finalize
endlocal & exit /b %EXIT_CODE%

:preclean_outputs
call :assert_safe_remove_target "%BUILD_DIR%" "BUILD_DIR"
if exist "%BUILD_DIR%" (
	echo Removing existing build directory: %BUILD_DIR%
	rmdir /s /q "%BUILD_DIR%"
	if errorlevel 1 (
		echo Error: failed to remove build directory: %BUILD_DIR%.>&2
		exit /b 1
	)
)

for /d %%D in ("%ROOT_DIR%build\temp.*") do (
	if exist "%%~fD" (
		echo Removing wheel temp build directory: %%~fD
		rmdir /s /q "%%~fD"
		if errorlevel 1 (
			echo Error: failed to remove %%~fD.>&2
			exit /b 1
		)
	)
)
for /d %%D in ("%ROOT_DIR%build\lib.*") do (
	if exist "%%~fD" (
		echo Removing wheel lib build directory: %%~fD
		rmdir /s /q "%%~fD"
		if errorlevel 1 (
			echo Error: failed to remove %%~fD.>&2
			exit /b 1
		)
	)
)
for /d %%D in ("%ROOT_DIR%build\bdist.*") do (
	if exist "%%~fD" (
		echo Removing wheel bdist directory: %%~fD
		rmdir /s /q "%%~fD"
		if errorlevel 1 (
			echo Error: failed to remove %%~fD.>&2
			exit /b 1
		)
	)
)

if exist "%WHEEL_DIR%" (
	pushd "%WHEEL_DIR%" >nul 2>&1
	if errorlevel 1 (
		echo Error: WHEEL_DIR exists but is not a directory: %WHEEL_DIR%.>&2
		exit /b 1
	)
	popd
	echo Preserving existing wheel directory: %WHEEL_DIR%
	goto wheel_dir_ready
)
mkdir "%WHEEL_DIR%" >nul 2>&1
if errorlevel 1 (
	echo Error: failed to create wheel directory: %WHEEL_DIR%.>&2
	exit /b 1
)
:wheel_dir_ready

exit /b 0

:assert_safe_remove_target
if "%~1"=="" (
	echo Error: refusing to remove empty path for %~2.>&2
	exit /b 1
)
set "TARGET_TO_REMOVE=%~f1"
set "ROOT_DIR_NO_SLASH=%ROOT_DIR%"
if "%ROOT_DIR_NO_SLASH:~-1%"=="\" set "ROOT_DIR_NO_SLASH=%ROOT_DIR_NO_SLASH:~0,-1%"
if "%TARGET_TO_REMOVE%"=="" (
	echo Error: failed to resolve remove target for %~2.>&2
	exit /b 1
)
if /I "%TARGET_TO_REMOVE%"=="%ROOT_DIR_NO_SLASH%" (
	echo Error: refusing to remove ROOT_DIR for %~2: %TARGET_TO_REMOVE%.>&2
	exit /b 1
)
if /I "%TARGET_TO_REMOVE%"=="%~d1\" (
	echo Error: refusing to remove filesystem root for %~2: %TARGET_TO_REMOVE%.>&2
	exit /b 1
)
exit /b 0
