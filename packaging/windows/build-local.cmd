@echo off
setlocal
rem Use Windows PowerShell modules even when this CMD is launched from PowerShell 7.
rem Its inherited PSModulePath can otherwise hide Get-FileHash and Expand-Archive.
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0bootstrap.ps1" %*
set "MARACUJA_BUILD_RESULT=%ERRORLEVEL%"
if not "%MARACUJA_BUILD_RESULT%"=="0" (
  echo.
  echo Build failed. The diagnostic log is in validation\build-windows-*.log
  pause
)
exit /b %MARACUJA_BUILD_RESULT%
