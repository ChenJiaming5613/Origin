@echo off
REM ---------------------------------------------------------------------------
REM Double-clickable wrapper around build_apk.ps1.
REM
REM This file is intentionally ASCII-only: cmd.exe decodes .cmd files using the
REM console's active code page, so non-ASCII characters here would be mangled.
REM All localized text lives in build_apk.ps1, which carries a UTF-8 BOM.
REM
REM Usage:
REM   build_apk.cmd                      -> debug APK
REM   build_apk.cmd -Run -Logcat         -> build, install, launch, tail log
REM   build_apk.cmd -Config Release      -> release APK (auto debug-signed)
REM ---------------------------------------------------------------------------

REM chcp 65001 makes the PowerShell child process print UTF-8 correctly when
REM this window was opened with a legacy code page (e.g. 936 on zh-CN).
chcp 65001 >nul 2>&1

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_apk.ps1" %*
set "RC=%ERRORLEVEL%"

REM Keep the window open when double-clicked from Explorer so the result stays
REM readable. When invoked from an existing console, cmdcmdline contains the
REM script path and we skip the pause.
echo %cmdcmdline% | find /i "%~nx0" >nul
if not errorlevel 1 pause

exit /b %RC%
