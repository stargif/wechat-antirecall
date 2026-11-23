@echo off
setlocal
if not exist "%~dp0uninstall.ps1" (
  echo Please extract the whole ZIP first, then run uninstall.cmd again.
  pause
  exit /b 1
)
for /f "tokens=2 delims=:" %%a in ('chcp') do set "CP=%%a"
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
set RC=%errorlevel%
if defined CP chcp %CP% >nul
pause
exit /b %RC%
