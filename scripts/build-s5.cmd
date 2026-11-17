@echo off
rem AICore-C build launcher (build-s0.cmd calls this one): vcvars64 (VS 18 BuildTools) -> cmake -G Ninja Release
rem -> build, -j 4. Logs go to the evidence dir (arg 1, default docs\evidence). One launcher, one build dir.
rem Usage: scripts\build-s5.cmd [evidence-dir]
setlocal EnableExtensions
set "ROOT=%~dp0.."
set "EV=%~1"
if "%EV%"=="" set "EV=%ROOT%\docs\evidence"
set "BUILD=%ROOT%\build"
if not exist "%EV%" mkdir "%EV%"
if not exist "%BUILD%" mkdir "%BUILD%"

set "VSBASE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
echo [s5] vcvars64 > "%EV%\s5-vcvars.log"
call "%VSBASE%\VC\Auxiliary\Build\vcvars64.bat" >> "%EV%\s5-vcvars.log" 2>&1
if errorlevel 1 (
  echo [s5] vcvars64 FALHOU
  exit /b 1
)

rem VS-bundled CMake + Ninja (guaranteed present; not on the Git Bash PATH).
set "PATH=%VSBASE%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSBASE%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

echo [s5] tools > "%EV%\s5-configure.log"
where cmake >> "%EV%\s5-configure.log" 2>&1
cmake --version >> "%EV%\s5-configure.log" 2>&1
where ninja >> "%EV%\s5-configure.log" 2>&1
ninja --version >> "%EV%\s5-configure.log" 2>&1
echo [s5] configure >> "%EV%\s5-configure.log"
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release >> "%EV%\s5-configure.log" 2>&1
if errorlevel 1 (
  echo [s5] configure FALHOU
  exit /b 1
)

echo [s5] build > "%EV%\s5-build.log"
cmake --build "%BUILD%" -j 4 >> "%EV%\s5-build.log" 2>&1
if errorlevel 1 (
  echo [s5] build FALHOU
  exit /b 1
)

echo [s5] OK
exit /b 0