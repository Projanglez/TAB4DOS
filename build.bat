@echo off
rem ============================================================
rem  TABTSR Build  -  Open Watcom 16-bit (Real Mode)
rem  Aufruf:  build.bat        -> nur kompilieren
rem           build.bat test   -> kompilieren + DOSBox starten
rem ============================================================

rem --- Open Watcom Environment einrichten ---
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
call "%WATCOM%\owsetenv.bat" > nul 2>&1
set PATH=%WATCOM%\binnt64;%PATH%

wcl -bt=dos -ms -os -zq -fe=tabtsr.exe tabtsr.c
if errorlevel 1 (
    echo.
    echo BUILD FEHLGESCHLAGEN
    exit /b 1
)
echo BUILD OK -^> tabtsr.exe

if /i "%1"=="test" (
    echo Starte DOSBox, lade TSR...
    "C:\dosgames\DOSBox.exe" -c "mount c %CD%" -c "c:" -c "tabtsr" -c "pause"
)
exit /b 0
