@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0.."
for %%I in ("%ROOT_DIR%") do set "ROOT_DIR=%%~fI"

where git >nul 2>&1
if errorlevel 1 (
	echo Error: git is not installed or not on PATH.>&2
	exit /b 1
)

pushd "%ROOT_DIR%" >nul

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
	echo Error: this script must run inside a git repository.>&2
	popd >nul
	exit /b 1
)

if not exist ".gitmodules" (
	echo Error: .gitmodules not found.>&2
	popd >nul
	exit /b 1
)

git config -f .gitmodules --get submodule.extern/libjxl.path >nul 2>&1
if errorlevel 1 (
	echo Error: extern/libjxl is not declared in .gitmodules.>&2
	echo Run this first:>&2
	echo   git -c submodule.recurse=false submodule add --depth 1 https://github.com/libjxl/libjxl.git extern/libjxl>&2
	popd >nul
	exit /b 1
)

echo Initializing extern/libjxl (shallow^)...
git submodule update --init --depth 1 extern/libjxl
if errorlevel 1 (
	popd >nul
	exit /b %errorlevel%
)

for %%S in (
	testdata
	third_party/googletest
	third_party/lcms
	third_party/libjpeg-turbo
	third_party/libpng
	third_party/sjpeg
	third_party/zlib
) do (
	git -C extern/libjxl config submodule.%%S.update none
	if errorlevel 1 (
		popd >nul
		exit /b %errorlevel%
	)
)

echo Initializing required nested submodules only...
git -C extern/libjxl submodule update --init --depth 1 third_party/highway third_party/brotli third_party/skcms
if errorlevel 1 (
	popd >nul
	exit /b %errorlevel%
)

echo.
echo Current extern/libjxl nested submodule status:
git -C extern/libjxl submodule status
set "STATUS_ERR=%errorlevel%"

popd >nul
exit /b %STATUS_ERR%

