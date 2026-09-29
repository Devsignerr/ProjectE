@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo Visual Studio 솔루션 생성: Builds2022\ProjectE.sln
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Build.ps1" -VisualStudio
if errorlevel 1 (
    echo.
    echo 솔루션 생성 실패. VS Installer에서 복구가 필요할 수 있습니다 ^(CLAUDE.md 빌드 절 참고^).
    pause
) else (
    start "" "%~dp0Builds2022\ProjectE.sln"
)
