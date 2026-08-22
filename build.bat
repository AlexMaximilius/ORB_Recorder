@echo off
REM build orb_recorder.exe -- Windows build
setlocal
set GLFW_ROOT=C:\glfw-3.3.8.bin.WIN64
if not exist "%GLFW_ROOT%\include\GLFW\glfw3.h" set GLFW_ROOT=C:\GLFW3
if not exist "%GLFW_ROOT%\include\GLFW\glfw3.h" (
    echo ERROR: GLFW headers not found. Edit GLFW_ROOT in build.bat.
    exit /b 1
)
echo Using GLFW at %GLFW_ROOT%

windres orb_recorder.rc -O coff -o orb_recorder_res.o
if %errorlevel% neq 0 ( echo windres FAILED & exit /b 1 )

REM  -static links the GCC runtime in. Without it the exe needs
REM  libwinpthread-1.dll, which only exists on machines that have MinGW --
REM  so it runs on the build box and fails everywhere else. Costs 42 KB.
gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -mwindows -static -I"%GLFW_ROOT%\include" -o orb_recorder.exe orb_recorder.c platform_win32.c platform_win32_video.c orb_recorder_res.o -L"%GLFW_ROOT%\lib-mingw-w64" -lglfw3 -lopengl32 -lglu32 -lgdi32 -luser32 -lshell32 -lole32 -lshlwapi -lcomdlg32 -lmfplat -lmfreadwrite -lmfuuid -lmf -lksuser -lwindowscodecs -ladvapi32 -lkernel32 -lm

if %errorlevel% equ 0 (
    echo BUILD OK -- orb_recorder.exe
    echo Run test.bat before shipping it.
) else (
    echo BUILD FAILED
    exit /b 1
)
endlocal
