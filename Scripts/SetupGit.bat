@echo off
rem 이 저장소(ProjectE)에만 커밋 작성자를 Devsignerr로 지정한다. PC 전체(--global) 설정은 건드리지 않는다.
rem 사용법: SetupGit.bat         지금 바로 지정 (어느 PC든)
rem         SetupGit.bat -Auto   Build.bat이 부름: 이미 지정돼 있으면 아무것도 안 함 (이 저장소는 소유자 혼자 쓴다는 전제)
setlocal EnableExtensions
chcp 65001 >nul

set "NAME=Devsignerr"
set "EMAIL=45942535+Devsignerr@users.noreply.github.com"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "AUTO="
if /i "%~1"=="-Auto" set "AUTO=1"

where git >nul 2>&1
if errorlevel 1 (
    if defined AUTO exit /b 0
    echo git을 찾을 수 없습니다.
    exit /b 1
)
git -C "%ROOT%" rev-parse --git-dir >nul 2>&1
if errorlevel 1 (
    if defined AUTO exit /b 0
    echo git 저장소가 아닙니다: "%ROOT%"
    exit /b 1
)
if not defined AUTO goto apply

set "CUR="
for /f "usebackq delims=" %%E in (`git -C "%ROOT%" config --local --get user.email 2^>nul`) do set "CUR=%%E"
if /i "%CUR%"=="%EMAIL%" exit /b 0

:apply
git -C "%ROOT%" config --local user.name "%NAME%"
if errorlevel 1 exit /b 1
git -C "%ROOT%" config --local user.email "%EMAIL%"
if errorlevel 1 exit /b 1
echo git 사용자 설정: 이 저장소만 %NAME% ^<%EMAIL%^> 로 지정 (PC 전체 설정은 그대로)
exit /b 0
