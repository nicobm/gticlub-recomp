@echo off
rem Racing Club+ : asks for the game's .pkg files and builds the game from the user's own copy (see LEEME_INSTALADOR.md)
setlocal
cd /d "%~dp0"
echo ==============================================================
echo  Racing Club+ para PC - instalador
echo.
echo  Necesitas los .pkg de TU copia del juego:
echo    - el juego, version EUROPEA  NPEB00060, con su licencia .rap al lado
echo    - la actualizacion 1.02
echo    - (opcional) el pack de coches
echo  No hace falta ningun emulador ni el firmware de PS3.
echo  Otras regiones o versiones NO sirven.
echo ==============================================================
echo.
rem Python: the one bundled with the installer (toolchain\python, with capstone) or, without it, the system's
set "PY=%~dp0toolchain\python\python.exe"
if exist "%PY%" goto have_python
set "PY=python"
where python >nul 2>nul
if errorlevel 1 (
  echo Falta Python 3. Instalalo desde https://www.python.org/downloads/ marcando "Add python.exe to PATH" y vuelve a ejecutar este fichero.
  pause
  exit /b 1
)
:have_python
"%PY%" -c "import capstone" >nul 2>nul
if errorlevel 1 (
  echo Instalando el modulo capstone de Python...
  "%PY%" -m pip install capstone==5.0.7
  if errorlevel 1 (
    echo No se ha podido instalar capstone. Hace falta conexion a internet.
    pause
    exit /b 1
  )
)
set /p G=Arrastra aqui el .pkg del JUEGO y pulsa Intro:
set /p U=Arrastra aqui el .pkg de la ACTUALIZACION 1.02 y pulsa Intro:
set "D="
set /p D=Arrastra aqui el .pkg del DLC (o pulsa Intro si no lo tienes):
"%PY%" instalar.py --pkg %G% %U% %D% --out "%~dp0Racing Club+"
echo.
pause
