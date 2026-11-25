:; echo "real-size-windows is the Windows command; run real-size here" >&2; exit 2
@echo off
setlocal
set "target=win32-x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "target=win32-arm64"
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set "target=win32-arm64"
"%~dp0..\prebuilds\%target%\real-size.exe" %*
exit /b %ERRORLEVEL%
