@rem Shorthand to flash then monitor
@echo off
idf.py flash

IF ERRORLEVEL 1 GOTO error
echo [91mFlashed successfully.[0m
idf.py monitor

:error
rem echo with colour, source: https://stackoverflow.com/a/38617204
echo [91mFlash error![0m
exit /b