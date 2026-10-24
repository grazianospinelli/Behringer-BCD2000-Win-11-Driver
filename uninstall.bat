@echo off
net session >nul 2>&1
if errorlevel 1 (
  echo Please run this file as administrator: right-click, then Run as administrator.
  pause
  exit /b 1
)
regsvr32 /u "%~dp0bcd2000asio.dll"
pause
