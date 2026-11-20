@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -File "%~dp0tools\Start-Upscalp-4K.ps1"
if errorlevel 1 pause
