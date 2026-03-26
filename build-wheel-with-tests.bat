@echo off
setlocal EnableExtensions

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

if not defined PYTHON_BIN (
	where py >nul 2>&1
	if errorlevel 1 (
		set "PYTHON_BIN=python"
	) else (
		set "PYTHON_BIN=py -3"
	)
)

if not defined DICOMSDL_WHEEL_PROFILE set "DICOMSDL_WHEEL_PROFILE=static"
if not defined BUILD_TESTING set "BUILD_TESTING=ON"
if not defined DICOM_BUILD_EXAMPLES set "DICOM_BUILD_EXAMPLES=OFF"
if not defined RUN_TESTS set "RUN_TESTS=1"
if not defined BUILD_WHEEL set "BUILD_WHEEL=1"
if not defined WHEEL_ONLY set "WHEEL_ONLY=0"
if not defined PIP_WHEEL_VERBOSE set "PIP_WHEEL_VERBOSE=1"
if not defined PYTEST_ARGS set "PYTEST_ARGS="

set "DICOMSDL_WHEEL_PROFILE=%DICOMSDL_WHEEL_PROFILE:"=%"
if /I "%DICOMSDL_WHEEL_PROFILE%"=="static" (
	set "WRAPPER_SCRIPT=%ROOT_DIR%\build-wheel-static.bat"
	if not defined BUILD_DIR set "BUILD_DIR=%ROOT_DIR%\build-wheel-static-with-tests"
	if not defined WHEEL_DIR set "WHEEL_DIR=%ROOT_DIR%\dist-static-with-tests"
) else (
	if /I "%DICOMSDL_WHEEL_PROFILE%"=="shared" (
		set "WRAPPER_SCRIPT=%ROOT_DIR%\build-wheel-shared.bat"
		if not defined BUILD_DIR set "BUILD_DIR=%ROOT_DIR%\build-wheel-shared-with-tests"
		if not defined WHEEL_DIR set "WHEEL_DIR=%ROOT_DIR%\dist-shared-with-tests"
	) else (
		echo Error: DICOMSDL_WHEEL_PROFILE must be one of static^|shared ^(got: %DICOMSDL_WHEEL_PROFILE%^).>&2
		exit /b 1
	)
)

call :ensure_python_test_requirements
if errorlevel 1 exit /b %errorlevel%

echo Running wheel build with tests ^(profile=%DICOMSDL_WHEEL_PROFILE%^)
call "%WRAPPER_SCRIPT%" %*
if errorlevel 1 exit /b %errorlevel%

echo Running Python test suite
%PYTHON_BIN% -m pytest -q tests/python %PYTEST_ARGS%
if errorlevel 1 exit /b %errorlevel%

exit /b 0

:ensure_python_test_requirements
%PYTHON_BIN% -c "import importlib.util, sys; required={'pytest':'pytest','numpy':'numpy','Pillow':'PIL','pydicom':'pydicom','SimpleITK':'SimpleITK','vtk':'vtk'}; raise SystemExit(0 if all(importlib.util.find_spec(module) for module in required.values()) else 1)" >nul 2>&1
if not errorlevel 1 exit /b 0

echo Error: Python test dependencies are missing for wheel-with-tests validation with %PYTHON_BIN%.>&2
echo Run: %PYTHON_BIN% -m pip install --upgrade -r tests/python/requirements.txt>&2
exit /b 1
