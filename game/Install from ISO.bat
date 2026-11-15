@echo off
rem Drag the game's .iso onto this file, or double-click it to pick the .iso.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-from-iso.ps1" %*
pause
