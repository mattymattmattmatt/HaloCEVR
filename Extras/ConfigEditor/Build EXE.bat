@echo off
REM ============================================================
REM  Build "HaloCEVR Config Editor.exe"
REM  Bakes the editor HTML + icon into a single self-contained exe.
REM  Uses csc.exe that ships with Windows (.NET Framework 4) -
REM  nothing to install. Re-run this whenever you edit the HTML.
REM
REM  Keep these 3 files together in this folder:
REM    - Launcher.cs
REM    - halocevr-config-editor.html
REM    - HaloCEVR_Config_Editor_Icon.ico
REM ============================================================
setlocal
cd /d "%~dp0"

set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
  echo [!] csc.exe ^(.NET Framework 4^) not found.
  echo     It normally ships with Windows 10/11. Install ".NET Framework 4.x" if missing.
  pause & exit /b 1
)

if not exist "Launcher.cs" echo [!] Launcher.cs missing & pause & exit /b 1
if not exist "halocevr-config-editor.html" echo [!] halocevr-config-editor.html missing & pause & exit /b 1
if not exist "HaloCEVR_Config_Editor_Icon.ico" echo [!] HaloCEVR_Config_Editor_Icon.ico missing & pause & exit /b 1

echo Building...
"%CSC%" /nologo /target:winexe /out:"HaloCEVR Config Editor.exe" ^
  /win32icon:"HaloCEVR_Config_Editor_Icon.ico" ^
  /resource:"halocevr-config-editor.html" ^
  /reference:System.Windows.Forms.dll ^
  Launcher.cs

if exist "HaloCEVR Config Editor.exe" (
  echo.
  echo [ok] Built "HaloCEVR Config Editor.exe"
  echo      One file - icon and editor are baked in. Move/rename/pin it freely.
) else (
  echo.
  echo [!] Build failed - see the messages above.
)
echo.
pause
