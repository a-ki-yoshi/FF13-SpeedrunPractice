@echo off
rem FF13-SpeedrunPractice launcher -- practice tools OFF, for measured runs.
rem Parks dinput8.dll as dinput8.dll.off (so the mod cannot load) and starts
rem the game through Steam. Idempotent: declares a state, does not toggle.
rem ASCII only on purpose: cmd.exe parses this file in the OEM codepage.
setlocal
cd /d "%~dp0"

if not exist "dinput8.dll" goto launch
ren "dinput8.dll" "dinput8.dll.off"
if errorlevel 1 goto locked

:launch
echo Practice tools: OFF  (no dinput8.dll in the game folder)
start "" "steam://rungameid/292120"
timeout /t 3 >nul
exit /b 0

:locked
echo [!] Could not park dinput8.dll -- is the game still running?
echo     Close FINAL FANTASY XIII, then run this again.
pause
exit /b 1
