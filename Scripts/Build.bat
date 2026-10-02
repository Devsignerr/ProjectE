@echo off
rem Build.ps1과 같은 절차(Ninja 구성 + 빌드)를 cmd로 수행한다.
rem 회사 PC 그룹 정책(AllSigned)이 서명 없는 .ps1 실행을 막아 루트 배치 파일은 이 스크립트를 쓴다.
rem 사용법: Build.bat [-Config Debug^|Release] [-Run] [-RunSandbox] [-Test] [-Clean]
setlocal EnableExtensions
chcp 65001 >nul

for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "CONFIG=Debug"
set "RUN="
set "RUNSANDBOX="
set "TEST="
set "CLEAN="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-Config" (
    set "CONFIG=%~2"
    shift
    shift
    goto parse
)
if /i "%~1"=="-Run" (set "RUN=1" & shift & goto parse)
if /i "%~1"=="-RunSandbox" (set "RUNSANDBOX=1" & shift & goto parse)
if /i "%~1"=="-Test" (set "TEST=1" & shift & goto parse)
if /i "%~1"=="-Clean" (set "CLEAN=1" & shift & goto parse)
echo 알 수 없는 인자: %~1
exit /b 1
:parsed

rem 저장소 소유자 PC면 이 저장소의 커밋 작성자를 Devsignerr로 (이미 지정됐으면 조용히 넘어감, 실패해도 빌드는 계속)
call "%~dp0SetupGit.bat" -Auto

if /i "%CONFIG%"=="Debug" (
    set "PRESET=ninja-debug"
) else if /i "%CONFIG%"=="Release" (
    set "PRESET=ninja-release"
) else (
    echo -Config는 Debug 또는 Release만 가능합니다: %CONFIG%
    exit /b 1
)

rem vswhere로 C++ 도구가 포함된 VS 설치 경로 탐색
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe를 찾을 수 없습니다. Visual Studio가 설치되어 있는지 확인하세요.
    exit /b 1
)
set "VSPATH="
for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%P"
if not defined VSPATH (
    echo C++ 도구가 포함된 Visual Studio를 찾을 수 없습니다.
    exit /b 1
)

set "CMAKEBIN=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if not exist "%CMAKEBIN%\cmake.exe" (
    echo VS 번들 CMake를 찾을 수 없습니다: "%CMAKEBIN%\cmake.exe"
    exit /b 1
)

rem Ninja + cl 조합은 VS 개발자 환경 변수(INCLUDE/LIB 등)가 필요하다
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo >nul 2>&1
if errorlevel 1 (
    echo VS 개발자 환경 설정 실패: "%VSPATH%\Common7\Tools\VsDevCmd.bat"
    exit /b 1
)
set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

cd /d "%ROOT%"
set "BUILDDIR=%ROOT%\Build\%PRESET%"
set "BINDIR=%BUILDDIR%\Bin"

if defined CLEAN if exist "%BUILDDIR%" (
    echo 빌드 디렉터리 삭제: "%BUILDDIR%"
    rmdir /s /q "%BUILDDIR%"
)

echo == 구성 (%PRESET%) ==
"%CMAKEBIN%\cmake.exe" --preset %PRESET%
if errorlevel 1 (
    echo CMake 구성 실패
    exit /b 1
)

echo == 빌드 (%PRESET%) ==
"%CMAKEBIN%\cmake.exe" --build --preset %PRESET%
if errorlevel 1 (
    echo CMake 빌드 실패
    exit /b 1
)

echo == 완료: %BINDIR%\ProjectEEditor.exe ==

if defined TEST (
    echo == 테스트 (%PRESET%^) ==
    "%CMAKEBIN%\ctest.exe" --preset %PRESET%
    if errorlevel 1 (
        echo 테스트 실패
        exit /b 1
    )
)

if defined RUN (
    echo == 에디터 실행 ==
    start "" /d "%ROOT%" "%BINDIR%\ProjectEEditor.exe"
)

if defined RUNSANDBOX (
    echo == Sandbox 실행 ==
    "%BINDIR%\Sandbox.exe"
)

exit /b 0
