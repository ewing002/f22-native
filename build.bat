@echo off
rem Builds f22native.exe and f22launcher.exe on Windows with Visual Studio 2022 (MSVC) and puts them into dist\F22Native\.
rem Needs: CMake and the "Desktop development with C++" tools; no game data is needed to build.
setlocal EnableExtensions
cd /d "%~dp0"
where cmake >nul 2>nul || (echo CMake is missing: winget install Kitware.CMake & exit /b 1)
set "VCVARS="
for %%E in (Enterprise Professional Community BuildTools) do (
  if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
    if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS (
  echo Visual Studio 2022 with the C++ tools is missing:
  echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"
  exit /b 1
)
call "%VCVARS%" >nul
cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
if exist dist rmdir /s /q dist
mkdir dist\F22Native
copy build\Release\f22native.exe dist\F22Native\ >nul
copy build\Release\f22launcher.exe dist\F22Native\ >nul
py tools\package.py build\Release dist\F22Native-release.zip
echo.
echo Done: dist\F22Native\f22native.exe and f22launcher.exe  (copy them next to F22.EXE and RESOURCE.RES, see README.md)
