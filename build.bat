@echo off
rem ============================================================
rem  DOSTAB Build  -  Open Watcom 16-bit (Real Mode)
rem  Usage:  build.bat        -> compile
rem  Runtime testing is done manually on real 386 hardware
rem  (no DOSBox: it has its own TAB-completion that interferes).
rem ============================================================

rem --- Set up Open Watcom environment ---
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
call "%WATCOM%\owsetenv.bat" > nul 2>&1
set PATH=%WATCOM%\binnt64;%PATH%

rem  -s REQUIRED: disables Watcom stack-overflow checks (__STK). A TSR runs
rem  on a foreign stack (COMMAND.COM/DOS); __STK would falsely report overflow
rem  and hang. See CLAUDE.md.
wcc -bt=dos -ms -os -s -zq dostab.c
if errorlevel 1 (
    echo.
    echo COMPILE FAILED
    exit /b 1
)

rem  Link separately so we control segment order (dostab.lnk): the transient
rem  INIT_TEXT segment is placed above the stack so _dos_keep frees it.
wlink @dostab.lnk
if errorlevel 1 (
    echo.
    echo LINK FAILED
    exit /b 1
)
echo BUILD OK -^> dostab.exe
exit /b 0
