@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if not errorlevel 1 (
    py -3 converter_gui.pyw
    if errorlevel 1 pause
    exit /b
)
where python >nul 2>nul
if not errorlevel 1 (
    python converter_gui.pyw
    if errorlevel 1 pause
    exit /b
)
if exist "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" (
    "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" converter_gui.pyw
    if errorlevel 1 pause
    exit /b
)
echo Python 3.10+ is required. Install Python from https://www.python.org/downloads/
echo Include Tcl/Tk for the graphical interface. The CLI does not require Tcl/Tk.
pause
