@echo off
chcp 65001 >nul
cd /d "%~dp0"
call "%~dp0Scripts\Build.bat" -Config Release -Run
if errorlevel 1 (
    echo.
    echo 빌드 또는 실행 실패. 위 로그를 확인하세요.
    pause
)
