@echo off
rem FF13-SpeedrunPractice launcher -- practice tools ON.
rem Restores dinput8.dll (from dinput8.dll.off if it was parked) and starts
rem the game through Steam. Idempotent: declares a state, does not toggle.
rem ASCII only on purpose: cmd.exe parses this file in the OEM codepage.
setlocal
cd /d "%~dp0"

tasklist /fi "imagename eq ffXiiiimg.exe" 2>nul | find /i "ffXiiiimg.exe" >nul && goto running

rem Only the mod's own dll counts: another dinput8.dll would start without the mod.
if exist "dinput8.dll" goto active
if not exist "dinput8.dll.off" goto missing
findstr /m /l /c:"ff13-srpractice" "dinput8.dll.off" >nul || goto foreign
ren "dinput8.dll.off" "dinput8.dll"
if errorlevel 1 goto locked
goto launch

:active
findstr /m /l /c:"ff13-srpractice" "dinput8.dll" >nul || goto foreign

:launch
echo Practice tools: ON   (dinput8.dll in place)
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

:missing
echo [!] dinput8.dll not found here (neither active nor parked as dinput8.dll.off).
echo     Put the mod's dinput8.dll next to this file first.
pause
exit /b 1

:locked
echo [!] Could not restore dinput8.dll -- is the game still running?
echo     Close FINAL FANTASY XIII, then run this again.
pause
exit /b 1
