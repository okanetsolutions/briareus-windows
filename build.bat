@echo off
rem Builds Briareus for Windows with Visual Studio Build Tools. Run from a Developer Command Prompt (cl.exe on PATH).
setlocal
set OUT=build
if not exist %OUT% mkdir %OUT%
set CFLAGS=/nologo /std:c11 /O2 /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /D_CRT_SECURE_NO_WARNINGS /Icore /Iapp /Fo%OUT%\ /utf-8
set LIBS=winhttp.lib advapi32.lib ole32.lib comctl32.lib gdi32.lib user32.lib shell32.lib uuid.lib dwmapi.lib winmm.lib mfplat.lib mfreadwrite.lib mfuuid.lib shlwapi.lib uxtheme.lib comdlg32.lib msimg32.lib gdiplus.lib
rc /nologo /Ires /Iapp /fo %OUT%\briareus.res res\briareus.rc || exit /b 1
cl %CFLAGS% core\*.c app\*.c %OUT%\briareus.res /Fe%OUT%\Briareus.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup %LIBS% || exit /b 1
cl %CFLAGS% core\*.c tests\core_tests.c /Fe%OUT%\core_tests.exe /link winhttp.lib advapi32.lib ole32.lib || exit /b 1
%OUT%\core_tests.exe
endlocal
