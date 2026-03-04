@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

if not defined BUILD_DIR set "BUILD_DIR=%ROOT_DIR%\build"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if not defined BUILD_TESTING set "BUILD_TESTING=ON"
if not defined DICOM_BUILD_EXAMPLES set "DICOM_BUILD_EXAMPLES=ON"
if not defined BUILD_WHEEL set "BUILD_WHEEL=1"
if not defined WHEEL_DIR set "WHEEL_DIR=%ROOT_DIR%\dist"
if not defined CTEST_LABEL set "CTEST_LABEL=dicomsdl"
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

if not defined DICOMSDL_CODEC_DEFAULT_MODE set "DICOMSDL_CODEC_DEFAULT_MODE=builtin"
if /I "%DICOMSDL_CODEC_DEFAULT_MODE%"=="builtin" (
	set "DICOMSDL_CODEC_DEFAULT_MODE=builtin"
) else (
	if /I "%DICOMSDL_CODEC_DEFAULT_MODE%"=="shared" (
		set "DICOMSDL_CODEC_DEFAULT_MODE=shared"
	) else (
		if /I "%DICOMSDL_CODEC_DEFAULT_MODE%"=="none" (
			set "DICOMSDL_CODEC_DEFAULT_MODE=none"
		) else (
			echo Error: DICOMSDL_CODEC_DEFAULT_MODE must be one of builtin^|shared^|none ^(got: %DICOMSDL_CODEC_DEFAULT_MODE%^).>&2
			exit /b 1
		)
	)
)

set "PIXEL_V2_CMAKE_ARGS=-DDICOMSDL_PIXEL_CORE=ON -DDICOMSDL_PIXEL_RUNTIME=ON -DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_JPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN=ON"

set "CODEC_CMAKE_ARGS="
call :resolve_codec_mode JPEG
if errorlevel 1 exit /b %errorlevel%
call :resolve_codec_mode JPEGLS
if errorlevel 1 exit /b %errorlevel%
call :resolve_codec_mode JPEG2K
if errorlevel 1 exit /b %errorlevel%
call :resolve_codec_mode HTJ2K
if errorlevel 1 exit /b %errorlevel%
call :resolve_codec_mode JPEGXL
if errorlevel 1 exit /b %errorlevel%

if not defined DICOMSDL_WINDOWS_TOOLCHAIN set "DICOMSDL_WINDOWS_TOOLCHAIN=auto"
set "SELECTED_TOOLCHAIN="
if /I "%DICOMSDL_WINDOWS_TOOLCHAIN%"=="auto" (
	where cl >nul 2>&1
	if errorlevel 1 (
		where clang-cl >nul 2>&1
		if not errorlevel 1 (
			set "SELECTED_TOOLCHAIN=clangcl"
		) else (
			where clang >nul 2>&1
			if errorlevel 1 (
				echo Error: neither cl.exe, clang-cl.exe, nor clang.exe was found on PATH.>&2
				echo Set DICOMSDL_WINDOWS_TOOLCHAIN=msvc^|clangcl^|clang64 and ensure compiler toolchain is initialized.>&2
				exit /b 1
			)
			where clang++ >nul 2>&1
			if errorlevel 1 (
				echo Error: clang++.exe not found on PATH.>&2
				exit /b 1
			)
			set "SELECTED_TOOLCHAIN=clang64"
		)
	) else (
		set "SELECTED_TOOLCHAIN=msvc"
	)
) else (
	if /I "%DICOMSDL_WINDOWS_TOOLCHAIN%"=="msvc" (
		set "SELECTED_TOOLCHAIN=msvc"
	) else (
		if /I "%DICOMSDL_WINDOWS_TOOLCHAIN%"=="clangcl" (
			set "SELECTED_TOOLCHAIN=clangcl"
		) else (
			if /I "%DICOMSDL_WINDOWS_TOOLCHAIN%"=="clang64" (
				set "SELECTED_TOOLCHAIN=clang64"
			) else (
				echo Error: unsupported DICOMSDL_WINDOWS_TOOLCHAIN=%DICOMSDL_WINDOWS_TOOLCHAIN%. Use auto^|msvc^|clangcl^|clang64.>&2
				exit /b 1
			)
		)
	)
)

set "TOOLCHAIN_CMAKE_ARGS="
set "EXPECTED_C_COMPILER_NAME="
if /I "%SELECTED_TOOLCHAIN%"=="msvc" (
	where cl >nul 2>&1
	if errorlevel 1 (
		echo Error: cl.exe not found. Run this from a Visual Studio Developer Command Prompt or call vcvarsall.bat first.>&2
		exit /b 1
	)
	set "EXPECTED_C_COMPILER_NAME=cl.exe"
	set "CC="
	set "CXX="
) else (
	if /I "%SELECTED_TOOLCHAIN%"=="clangcl" (
		set "CLANGCL_COMPILER_PATH="
		for /f "delims=" %%I in ('where clang-cl') do (
			if not defined CLANGCL_COMPILER_PATH set "CLANGCL_COMPILER_PATH=%%~fI"
		)
		if not defined CLANGCL_COMPILER_PATH (
			echo Error: clang-cl.exe not found on PATH for clangcl toolchain.>&2
			exit /b 1
		)
		set TOOLCHAIN_CMAKE_ARGS=-DCMAKE_C_COMPILER:FILEPATH="!CLANGCL_COMPILER_PATH!" -DCMAKE_CXX_COMPILER:FILEPATH="!CLANGCL_COMPILER_PATH!"
		where llvm-rc >nul 2>&1
		if not errorlevel 1 (
			set "LLVM_RC_COMPILER_PATH="
			for /f "delims=" %%I in ('where llvm-rc') do (
				if not defined LLVM_RC_COMPILER_PATH set "LLVM_RC_COMPILER_PATH=%%~fI"
			)
			if defined LLVM_RC_COMPILER_PATH set TOOLCHAIN_CMAKE_ARGS=!TOOLCHAIN_CMAKE_ARGS! -DCMAKE_RC_COMPILER:FILEPATH="!LLVM_RC_COMPILER_PATH!"
		)
		set "EXPECTED_C_COMPILER_NAME=clang-cl.exe"
		set "CC=!CLANGCL_COMPILER_PATH!"
		set "CXX=!CLANGCL_COMPILER_PATH!"
	) else (
		set "CLANG_COMPILER_PATH="
		for /f "delims=" %%I in ('where clang') do (
			if not defined CLANG_COMPILER_PATH set "CLANG_COMPILER_PATH=%%~fI"
		)
		if not defined CLANG_COMPILER_PATH (
			echo Error: clang.exe not found on PATH for clang64 toolchain.>&2
			exit /b 1
		)
		set "CLANGXX_COMPILER_PATH="
		for /f "delims=" %%I in ('where clang++') do (
			if not defined CLANGXX_COMPILER_PATH set "CLANGXX_COMPILER_PATH=%%~fI"
		)
		if not defined CLANGXX_COMPILER_PATH (
			echo Error: clang++.exe not found on PATH for clang64 toolchain.>&2
			exit /b 1
		)
		set TOOLCHAIN_CMAKE_ARGS=-DCMAKE_C_COMPILER:FILEPATH="!CLANG_COMPILER_PATH!" -DCMAKE_CXX_COMPILER:FILEPATH="!CLANGXX_COMPILER_PATH!"
		where llvm-rc >nul 2>&1
		if not errorlevel 1 (
			set "LLVM_RC_COMPILER_PATH="
			for /f "delims=" %%I in ('where llvm-rc') do (
				if not defined LLVM_RC_COMPILER_PATH set "LLVM_RC_COMPILER_PATH=%%~fI"
			)
			if defined LLVM_RC_COMPILER_PATH set TOOLCHAIN_CMAKE_ARGS=!TOOLCHAIN_CMAKE_ARGS! -DCMAKE_RC_COMPILER:FILEPATH="!LLVM_RC_COMPILER_PATH!"
		)
		set "EXPECTED_C_COMPILER_NAME=clang.exe"
		set "CC=!CLANG_COMPILER_PATH!"
		set "CXX=!CLANGXX_COMPILER_PATH!"
	)
)

if defined CMAKE_GENERATOR (
	set "GENERATOR=%CMAKE_GENERATOR%"
) else (
	if /I "%SELECTED_TOOLCHAIN%"=="msvc" (
		where ninja >nul 2>&1
		if errorlevel 1 (
			set "GENERATOR=NMake Makefiles"
		) else (
			set "GENERATOR=Ninja"
		)
	) else (
		if /I "%SELECTED_TOOLCHAIN%"=="clangcl" (
			where ninja >nul 2>&1
			if errorlevel 1 (
				echo Error: ninja is required for clangcl builds. Install Ninja or add it to PATH.>&2
				exit /b 1
			)
			set "GENERATOR=Ninja"
		) else (
			where ninja >nul 2>&1
			if errorlevel 1 (
				echo Error: ninja is required for clang64 builds. Install it in MSYS2 clang64 environment.>&2
				exit /b 1
			)
			set "GENERATOR=Ninja"
		)
	)
)

set "CMAKE_CACHE_FILE=%BUILD_DIR%\CMakeCache.txt"
if exist "%CMAKE_CACHE_FILE%" (
	set "EXISTING_GENERATOR="
	set "EXISTING_C_COMPILER="
	set "EXISTING_C_COMPILER_NAME="
	set "MISMATCH_REASON="
	for /f "tokens=1,* delims==" %%A in ('findstr /B /C:"CMAKE_GENERATOR:INTERNAL=" "%CMAKE_CACHE_FILE%"') do (
		set "EXISTING_GENERATOR=%%B"
	)
	for /f "tokens=1,* delims==" %%A in ('findstr /B /C:"CMAKE_C_COMPILER:" "%CMAKE_CACHE_FILE%"') do (
		set "EXISTING_C_COMPILER=%%B"
	)
	if defined EXISTING_C_COMPILER (
		for %%I in ("!EXISTING_C_COMPILER!") do (
			set "EXISTING_C_COMPILER_NAME=%%~nxI"
		)
	)
	if defined EXISTING_GENERATOR (
		if /I not "!EXISTING_GENERATOR!"=="%GENERATOR%" (
			set "MISMATCH_REASON=generator"
		)
	)
	if not defined MISMATCH_REASON (
		if defined EXISTING_C_COMPILER_NAME (
			if /I not "!EXISTING_C_COMPILER_NAME!"=="!EXPECTED_C_COMPILER_NAME!" (
				set "MISMATCH_REASON=compiler"
			)
		)
	)
	if defined MISMATCH_REASON (
		if "%RESET_CMAKE_CACHE%"=="1" (
			if /I "!MISMATCH_REASON!"=="generator" (
				echo Resetting CMake cache in %BUILD_DIR% to switch generator ^(!EXISTING_GENERATOR! -^> %GENERATOR%^)
			) else (
				echo Resetting CMake cache in %BUILD_DIR% to switch compiler ^(!EXISTING_C_COMPILER_NAME! -^> !EXPECTED_C_COMPILER_NAME!^)
			)
			if exist "%BUILD_DIR%\CMakeCache.txt" del /f /q "%BUILD_DIR%\CMakeCache.txt"
			if exist "%BUILD_DIR%\CMakeFiles" rmdir /s /q "%BUILD_DIR%\CMakeFiles"
		) else (
			if /I "!MISMATCH_REASON!"=="generator" (
				echo Error: %BUILD_DIR% was configured with generator '!EXISTING_GENERATOR!', but requested '%GENERATOR%'.>&2
			) else (
				echo Error: %BUILD_DIR% was configured with C compiler '!EXISTING_C_COMPILER_NAME!', but requested '!EXPECTED_C_COMPILER_NAME!'.>&2
			)
			echo        Set RESET_CMAKE_CACHE=1 to remove CMakeCache.txt/CMakeFiles automatically.>&2
			exit /b 1
		)
	)
)

echo Selected Windows toolchain: %SELECTED_TOOLCHAIN%
echo Codec modes: jpeg=%CODEC_MODE_JPEG% jpegls=%CODEC_MODE_JPEGLS% jpeg2k=%CODEC_MODE_JPEG2K% htj2k=%CODEC_MODE_HTJ2K% jpegxl=%CODEC_MODE_JPEGXL%
echo Pixel runtime mode: v2 ^(default^)
echo Configuring dicomsdl (%BUILD_TYPE%) in %BUILD_DIR%
echo Toolchain CMake args: %TOOLCHAIN_CMAKE_ARGS%
cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -DBUILD_TESTING=%BUILD_TESTING% -DDICOM_BUILD_EXAMPLES=%DICOM_BUILD_EXAMPLES% -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -G "%GENERATOR%" %TOOLCHAIN_CMAKE_ARGS% %CODEC_CMAKE_ARGS% %PIXEL_V2_CMAKE_ARGS% %CMAKE_EXTRA_ARGS%
if errorlevel 1 exit /b %errorlevel%

set "CONFIGURED_C_COMPILER="
set "CONFIGURED_C_COMPILER_NAME="
for /f "tokens=1,* delims==" %%A in ('findstr /B /C:"CMAKE_C_COMPILER:" "%CMAKE_CACHE_FILE%"') do (
	set "CONFIGURED_C_COMPILER=%%B"
)
if defined CONFIGURED_C_COMPILER (
	for %%I in ("!CONFIGURED_C_COMPILER!") do (
		set "CONFIGURED_C_COMPILER_NAME=%%~nxI"
	)
	if /I not "!CONFIGURED_C_COMPILER_NAME!"=="!EXPECTED_C_COMPILER_NAME!" (
		echo Error: expected %SELECTED_TOOLCHAIN% compiler '!EXPECTED_C_COMPILER_NAME!' but CMake configured '!CONFIGURED_C_COMPILER!'.>&2
		echo        Verify PATH order and clear %BUILD_DIR% ^(or set RESET_CMAKE_CACHE=1^).>&2
		exit /b 1
	)
)

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
	if defined CTEST_LABEL (
		ctest --output-on-failure -C %BUILD_TYPE% -L "%CTEST_LABEL%"
	) else (
		ctest --output-on-failure -C %BUILD_TYPE%
	)
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

:resolve_codec_mode
set "CODEC_NAME=%~1"
set "MODE_VAR_NAME=DICOMSDL_CODEC_%CODEC_NAME%_MODE"
call set "MODE_VALUE=%%%MODE_VAR_NAME%%%"
if not defined MODE_VALUE set "MODE_VALUE=%DICOMSDL_CODEC_DEFAULT_MODE%"

if /I "%MODE_VALUE%"=="builtin" (
	set "MODE_VALUE=builtin"
	set "CODEC_CMAKE_ARGS=%CODEC_CMAKE_ARGS% -DDICOMSDL_CODEC_%CODEC_NAME%_BUILTIN=ON -DDICOMSDL_CODEC_%CODEC_NAME%_SHARED=OFF"
) else (
	if /I "%MODE_VALUE%"=="shared" (
		set "MODE_VALUE=shared"
		set "CODEC_CMAKE_ARGS=%CODEC_CMAKE_ARGS% -DDICOMSDL_CODEC_%CODEC_NAME%_BUILTIN=OFF -DDICOMSDL_CODEC_%CODEC_NAME%_SHARED=ON"
	) else (
		if /I "%MODE_VALUE%"=="none" (
			set "MODE_VALUE=none"
			set "CODEC_CMAKE_ARGS=%CODEC_CMAKE_ARGS% -DDICOMSDL_CODEC_%CODEC_NAME%_BUILTIN=OFF -DDICOMSDL_CODEC_%CODEC_NAME%_SHARED=OFF"
		) else (
			echo Error: %MODE_VAR_NAME% must be one of builtin^|shared^|none ^(got: %MODE_VALUE%^).>&2
			exit /b 1
		)
	)
)

set "CODEC_MODE_%CODEC_NAME%=%MODE_VALUE%"
exit /b 0
