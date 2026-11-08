@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
set "PYTHONUTF8=1"
if exist ".console-venv\Scripts\python.exe" goto dependencies
py -3 -c "import sys; assert sys.version_info >= (3,10)" >nul 2>&1
if errorlevel 1 goto fallback
py -3 -m venv .console-venv
if errorlevel 1 goto failed
goto dependencies
:fallback
python -c "import sys; assert sys.version_info >= (3,10)" >nul 2>&1
if errorlevel 1 goto no_python
python -m venv .console-venv
if errorlevel 1 goto failed
:dependencies
".console-venv\Scripts\python.exe" -c "import serial" >nul 2>&1
if not errorlevel 1 goto run
".console-venv\Scripts\python.exe" -m pip install -r requirements-ground.txt
if errorlevel 1 goto failed
:run
".console-venv\Scripts\python.exe" -m tools.can_console --role ground %*
set "task_exit=%errorlevel%"
pause
exit /b %task_exit%
:no_python
echo Install Python 3.10+ from https://www.python.org/downloads/windows/
echo Then run this file again. Enable pip and the Python launcher when installing.
goto failed
:failed
echo Startup failed. See the error above.
pause
exit /b 1
