@echo off
setlocal

cd /d "%~dp0"
title Volante Config - Instalador

echo.
echo ==========================================
echo  Volante Config - Instalador Windows
echo ==========================================
echo.

where python >nul 2>nul
if %errorlevel% neq 0 (
  echo Python no encontrado.
  where winget >nul 2>nul
  if %errorlevel% neq 0 (
    echo.
    echo No encontre winget para instalar Python automaticamente.
    echo Instala Python manualmente desde:
    echo https://www.python.org/downloads/
    echo.
    pause
    exit /b 1
  )

  echo Instalando Python con winget...
  winget install --id Python.Python.3.13 --source winget --accept-package-agreements --accept-source-agreements
  if %errorlevel% neq 0 (
    echo.
    echo Error instalando Python con winget.
    pause
    exit /b 1
  )
)

echo.
echo Instalando/actualizando dependencias...
python -m pip install --upgrade pip
if %errorlevel% neq 0 (
  echo Error actualizando pip.
  pause
  exit /b 1
)

python -m pip install -r requirements.txt
if %errorlevel% neq 0 (
  echo Error instalando requirements.txt.
  pause
  exit /b 1
)

echo.
echo Creando acceso directo en escritorio...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$root=(Resolve-Path '.').Path; $desktop=[Environment]::GetFolderPath('Desktop'); $lnk=Join-Path $desktop 'Volante Config.lnk'; $target=Join-Path $root 'run_wheel_config_app.bat'; $ws=New-Object -ComObject WScript.Shell; $s=$ws.CreateShortcut($lnk); $s.TargetPath=$target; $s.WorkingDirectory=$root; $s.Save()"

echo.
echo Instalacion lista.
echo.
choice /C SN /N /M "Abrir Volante Config ahora? [S/N] "
if errorlevel 2 goto end
start "" "%~dp0run_wheel_config_app.bat"

:end
endlocal
