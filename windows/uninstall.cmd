@echo off
setlocal
if not exist "%~dp0uninstall.ps1" (
  echo Please extract the whole ZIP first, then run uninstall.cmd again.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
set RC=%errorlevel%
pause
exit /b %RC%
