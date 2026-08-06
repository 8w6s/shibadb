@echo off
setlocal
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
set "NINJADIR=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "SRC=%~dp0"
set "BUILD=%SRC%build-msvc"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%NINJADIR%;%PATH%"

if "%1"=="configure" (
    "%CMAKE%" -S "%SRC%." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAKE_PROGRAM="%NINJADIR%\ninja.exe" -DSDB_BUILD_TESTS=ON
    exit /b %errorlevel%
)
rem configurefast is retained as a compatibility alias. The /wd4701 and
rem _CRT_SECURE_NO_WARNINGS overrides were removed once every C4701 was fixed
rem at source (variables initialized at declaration in btree.c/engine.c); the
rem tree now builds clean under /W4 /WX with no suppressions. This target is
rem now identical to `configure`.
if "%1"=="configurefast" (
    "%CMAKE%" -S "%SRC%." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAKE_PROGRAM="%NINJADIR%\ninja.exe" -DSDB_BUILD_TESTS=ON
    exit /b %errorlevel%
)
if "%1"=="build" (
    "%CMAKE%" --build "%BUILD%" --parallel -- -k 0
    exit /b %errorlevel%
)
if "%1"=="test" (
    "%CTEST%" --test-dir "%BUILD%" --output-on-failure -j 4
    exit /b %errorlevel%
)
echo Unknown phase: %1
exit /b 2
