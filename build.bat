@echo off
setlocal
cd /d "%~dp0"

echo ============================================
echo  How to Fish Trainer - Otomatik Derleme
echo ============================================
echo.

REM Oyun aciksa DLL kilitli olur, uyar
tasklist /FI "IMAGENAME eq How to Fish.exe" 2>nul | find /I "How to Fish.exe" >nul
if %errorlevel%==0 (
    echo [!] UYARI: Oyun su an acik. DLL kilitli olabilir.
    echo     Oyunda END tusuna bas -eject- ya da oyunu kapat,
    echo     sonra bu pencereye donup bir tusa bas.
    echo.
    pause
)

echo [*] CMake configure ediliyor...
cmake -S . -B build -A x64
if %errorlevel% neq 0 (
    echo.
    echo [!] Configure BASARISIZ! CMake veya VS eksik olabilir.
    pause
    exit /b 1
)

echo.
echo [*] Derleniyor (Release x64)...
cmake --build build --config Release
if %errorlevel% neq 0 (
    echo.
    echo [!] Derleme BASARISIZ!
    echo     - LNK1104 hatasi goruyorsan: oyun hala acik, DLL kilitli.
    echo       Oyunu kapatip tekrar dene.
    pause
    exit /b 1
)

echo.
echo ============================================
echo  [+] BASARILI!
echo  EXE : build\Release\HowToFishTrainer.exe  ^<-- buna cift tikla
echo  DLL : build\Release\HowToFishInternal_v84.dll
echo ============================================
echo.
echo Kullanim (10 numara):
echo   1. Oyunu baslat, ana menuye gel.
echo   2. HowToFishTrainer.exe + DLL ayni klasorde olsun.
echo   3. HowToFishTrainer.exe'ye cift tikla (admin ister - evet de).
echo   4. Oyuna don -^> INSERT ile menu.
echo.
pause
