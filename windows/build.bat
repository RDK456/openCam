@echo off
rem Builds out\opencam_vcam.dll and out\openCam.exe with the VS 2022 C++ build tools.
rem /Brepro makes identical sources give identical binaries, so updates skip unchanged files and deltas stay small.
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%PATH%"
set "VS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" set "VS=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set /p VER=<..\VERSION
if not exist out mkdir out

cl /nologo /Brepro /O2 /EHsc /std:c++17 /W3 /MT /LD vcam.cpp /Fe:out\opencam_vcam.dll /Fo:out\ ^
   /link /Brepro /DEF:vcam.def mfplat.lib mfuuid.lib ole32.lib advapi32.lib || exit /b 1

rc /nologo /fo out\app.res app.rc || exit /b 1
cl /nologo /Brepro /utf-8 /O2 /EHsc /std:c++17 /W3 /MT /DOPENCAM_VERSION=\"%VER%\" app.cpp out\app.res /Fe:out\openCam.exe /Fo:out\ ^
   /link /Brepro /SUBSYSTEM:WINDOWS mfplat.lib mfuuid.lib mf.lib mfsensorgroup.lib wmcodecdspuuid.lib ^
   ole32.lib ws2_32.lib user32.lib gdi32.lib shell32.lib advapi32.lib d2d1.lib dwrite.lib dwmapi.lib ^
   wininet.lib bcrypt.lib msdelta.lib || exit /b 1

echo Built out\openCam.exe %VER% and out\opencam_vcam.dll
