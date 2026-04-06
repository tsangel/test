@echo off
setlocal

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

if "%~1"=="" (
    set "TARGET=%REPO%\tmp\ohif_public_demo2_downloads\study_b563f8c11f83\series_b7d18d62d331"
) else (
    set "TARGET=%~1"
)

set "PYD=%REPO%\build-msvccheck\_deps\dicomsdl_openjpeg-build\bin\_dicomsdl.cp314-win_amd64.pyd"
if not exist "%PYD%" (
    echo Missing built extension:
    echo   %PYD%
    echo.
    echo Build it first with:
    echo   cmake --build build-msvccheck --config Release --target _dicomsdl
    exit /b 1
)

python -c "import importlib.util, pathlib, sys; repo = pathlib.Path(r'%REPO%'); target = r'%TARGET%'; sys.path.insert(0, str(repo / 'bindings' / 'python')); pyd = repo / 'build-msvccheck' / '_deps' / 'dicomsdl_openjpeg-build' / 'bin' / '_dicomsdl.cp314-win_amd64.pyd'; spec = importlib.util.spec_from_file_location('dicomsdl._dicomsdl', pyd); module = importlib.util.module_from_spec(spec); sys.modules['dicomsdl._dicomsdl'] = module; spec.loader.exec_module(module); from dicomsdl.dicomview import main; raise SystemExit(main([target]))"
exit /b %ERRORLEVEL%
