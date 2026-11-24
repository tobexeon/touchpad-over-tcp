@echo off
rem Starts the receiver with no console window. Keep mtp-receiver.exe next to this file.
powershell -NoProfile -WindowStyle Hidden -Command "Start-Process -FilePath %~dp0mtp-receiver.exe -ArgumentList 39871 -WindowStyle Hidden"
