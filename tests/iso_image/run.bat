@echo off
rem Builds and runs the disc-image overlay check. See README.md.
rem   run.bat <disc image> <extracted tree>
setlocal
if "%~1"=="" goto usage
if "%~2"=="" goto usage

where cl >nul 2>&1
if errorlevel 1 call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set WORK=%TEMP%\iso_image_test
set EXE=%WORK%\iso_test.exe
if not exist "%WORK%" mkdir "%WORK%"
if not exist "%WORK%\mount" mkdir "%WORK%\mount"

cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /Fe"%EXE%" ^
   "%~dp0test_main.c" "%~dp0..\..\src\kernel\kernel_iso.c" || exit /b 1

rem the image is the fourth argument: the test writes the mount description
rem itself, so paths with spaces and parentheses never pass through cmd
"%EXE%" "%WORK%\mount" "%~f2" /full "%~f1"
exit /b %ERRORLEVEL%

:usage
echo usage: run.bat "<disc image>" "<extracted tree>"
exit /b 2
