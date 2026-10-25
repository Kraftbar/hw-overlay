@echo off
rem Builds overlay.exe with MSVC. The manifest makes Windows ask for admin
rem on launch, which the ASUS BIOS sensor interface requires.
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /W4 /utf-8 overlay.c user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib wbemuuid.lib ^
   /link /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
