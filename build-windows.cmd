@echo off
setlocal
call "%~dp0packaging\windows\build-local.cmd" %*
exit /b %ERRORLEVEL%
