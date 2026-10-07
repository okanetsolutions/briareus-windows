@echo off
rem Builds Briareus for Windows with Visual Studio Build Tools. Run from a Developer Command Prompt (cl.exe on PATH).
setlocal
set OUT=build
if not exist %OUT% mkdir %OUT%
rem /W4 with the SDK headers quiet and unused parameters allowed, as with GCC. CI adds /WX through the CL variable.
rem OPT replaces /O2: the CI leak job builds against the debug CRT with run-time checks (/Od /MTd /RTC1).
if not defined OPT set OPT=/O2
set CFLAGS=/nologo /std:c11 %OPT% /W4 /wd4100 /external:anglebrackets /external:W0 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /D_CRT_SECURE_NO_WARNINGS /Icore /Iapp /Fo%OUT%\ /utf-8
rem app\canvas.cpp is C++ in C style (the SDK declares DirectWrite for C++ only): no exceptions, no RTTI.
set CXXFLAGS=%CFLAGS:/std:c11=/std:c++17% /EHs-c- /GR-
set LIBS=winhttp.lib advapi32.lib ole32.lib comctl32.lib gdi32.lib user32.lib shell32.lib uuid.lib dwmapi.lib winmm.lib mfplat.lib mfreadwrite.lib mfuuid.lib shlwapi.lib uxtheme.lib comdlg32.lib msimg32.lib d2d1.lib dwrite.lib
rem miniaudio, the meeting assistant's audio library, is fetched at the version and hash the Makefile pins.
set MINIAUDIO_VERSION=0.11.25
set MINIAUDIO_SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
if not exist third_party\miniaudio\miniaudio.h (
  if not exist third_party\miniaudio mkdir third_party\miniaudio
  curl -fsSL https://raw.githubusercontent.com/mackron/miniaudio/%MINIAUDIO_VERSION%/miniaudio.h -o third_party\miniaudio\miniaudio.h.download || exit /b 1
  powershell -NoProfile -Command "if ((Get-FileHash third_party\miniaudio\miniaudio.h.download -Algorithm SHA256).Hash -ne '%MINIAUDIO_SHA256%') { exit 1 }" || (echo miniaudio.h does not match its pinned SHA-256 & exit /b 1)
  move /y third_party\miniaudio\miniaudio.h.download third_party\miniaudio\miniaudio.h >nul || exit /b 1
)
rc /nologo /Ires /Iapp /fo %OUT%\briareus.res res\briareus.rc || exit /b 1
cl /c %CXXFLAGS% app\canvas.cpp || exit /b 1
cl %CFLAGS% core\*.c app\*.c %OUT%\canvas.obj %OUT%\briareus.res /Fe%OUT%\Briareus.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup %LIBS% || exit /b 1
cl %CFLAGS% core\*.c tests\harness.c tests\core_*.c /Fe%OUT%\core_tests.exe /link winhttp.lib advapi32.lib ole32.lib || exit /b 1
rem The app tests link the app itself, WinMain included, under the tests' console main.
cl %CFLAGS% core\*.c app\*.c %OUT%\canvas.obj tests\harness.c tests\app_*.c /Fe%OUT%\app_tests.exe /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
%OUT%\core_tests.exe || exit /b 1
%OUT%\app_tests.exe || exit /b 1
endlocal
