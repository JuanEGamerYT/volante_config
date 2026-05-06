@echo off
cd /d "%~dp0"
where python >nul 2>nul
if %errorlevel% equ 0 (
  python wheel_config_app.py
  goto check_error
)

where py >nul 2>nul
if %errorlevel% equ 0 (
  py -3 wheel_config_app.py
  goto check_error
)

echo No encontre Python.
echo Instala Python o ejecuta install_windows.bat primero.
pause
exit /b 1

:check_error
if errorlevel 1 pause
