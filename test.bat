@echo off
REM  test.bat -- run the suite. Exits non-zero if anything fails, so it can
REM  gate a release or sit in front of a commit.
REM
REM  These test the two pieces that are pure computation and can therefore be
REM  checked by a machine: the annotation rasteriser and the PNG writer. The
REM  rest of the program is a window, a hotkey and a capture, and is tested by
REM  running it -- but those two are exactly where a silent regression hides,
REM  because a broken compressor still produces files that open.
REM
REM  Needs only gcc. Python and Pillow are optional and add two more decoders.
setlocal enabledelayedexpansion
set FAILED=0
set HERE=%~dp0

echo === paint.h ===================================================
gcc -O2 -std=c11 -Wall -Wextra -I"%HERE%." -o "%HERE%tests\test_paint.exe" "%HERE%tests\test_paint.c" -lm
if %errorlevel% neq 0 ( echo   COMPILE FAILED & set FAILED=1 & goto png )
"%HERE%tests\test_paint.exe"
if %errorlevel% neq 0 set FAILED=1

:png
echo.
echo === png_write.h ===============================================
gcc -O2 -std=c11 -Wall -Wextra -I"%HERE%." -o "%HERE%tests\test_png.exe" "%HERE%tests\test_png.c" -lm
if %errorlevel% neq 0 ( echo   COMPILE FAILED & set FAILED=1 & goto indep )
"%HERE%tests\test_png.exe"
if %errorlevel% neq 0 set FAILED=1

:indep
echo.
echo === independent decoders (zlib, PIL) ==========================
where python >nul 2>nul
if %errorlevel% neq 0 (
    echo   skipped: python is not on PATH
    goto report
)
gcc -O2 -std=c11 -Wall -Wextra -I"%HERE%." -o "%HERE%tests\make_sample.exe" "%HERE%tests\make_sample.c" -lm
if %errorlevel% neq 0 ( echo   COMPILE FAILED & set FAILED=1 & goto report )
python "%HERE%tests\verify_png.py"
if %errorlevel% neq 0 set FAILED=1

:report
echo.
if "%FAILED%"=="0" (
    echo ALL TESTS PASSED
    del /q "%HERE%tests\*.exe" 2>nul
    exit /b 0
) else (
    echo TESTS FAILED
    exit /b 1
)
