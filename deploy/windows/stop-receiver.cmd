@echo off
rem Asks the running receiver to release contacts and exit.
powershell -NoProfile -WindowStyle Hidden -Command "& %~dp0mtp-receiver.exe --stop"
