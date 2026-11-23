@echo off
rem FF13-SpeedrunPractice launcher -- practice tools OFF, for measured runs.
rem Parks dinput8.dll as dinput8.dll.off (so the mod cannot load) and starts
rem the game through Steam. Idempotent: declares a state, does not toggle.
rem ASCII only on purpose: cmd.exe parses this file in the OEM codepage.
setlocal
cd /d "%~dp0"

tasklist /fi "imagename eq ffXiiiimg.exe" 2>nul | find /i "ffXiiiimg.exe" >nul && goto running

if not exist "dinput8.dll" goto launch
rem Only the mod's own dll is parked: another dinput8.dll stops everything here.
findstr /m /l /c:"ff13-srpractice" "dinput8.dll" >nul || goto foreign
if exist "dinput8.dll.off" goto both
ren "dinput8.dll" "dinput8.dll.off"
if errorlevel 1 goto locked

:launch
echo Practice tools: OFF  (no dinput8.dll in the game folder)
start "" "steam://rungameid/292120"
timeout /t 3 >nul
exit /b 0

:running
echo [!] FINAL FANTASY XIII is running. Close it, then run this again.
pause
exit /b 1

:foreign
echo [!] The dinput8.dll here is not this mod's (another mod?).
echo     Nothing was renamed, and the game was not started.
pause
exit /b 1

:both
echo [!] Both dinput8.dll and dinput8.dll.off are here. Nothing was renamed.
echo     Keep only this mod's dll (as dinput8.dll), then run this again.
pause
exit /b 1

:locked
echo [!] Could not park dinput8.dll -- is the game still running?
echo     Close FINAL FANTASY XIII, then run this again.
pause
exit /b 1
