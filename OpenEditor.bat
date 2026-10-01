@echo off
chcp 65001 >nul
cd /d "%~dp0"

rem 빌드 없이 이미 빌드된 에디터 중 가장 최근 것을 실행한다 (인자는 에디터로 전달)
set "EDITOR_EXE="
for /f "usebackq delims=" %%E in (`powershell -NoProfile -Command "Get-ChildItem -Path 'Build' -Recurse -Filter 'ProjectEEditor.exe' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\_deps\*' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName"`) do set "EDITOR_EXE=%%E"

if not defined EDITOR_EXE (
    echo 빌드된 에디터가 없습니다. RunEditor.bat 또는 Build.bat으로 먼저 빌드하세요.
    pause
    exit /b 1
)

echo 실행: %EDITOR_EXE%
start "" "%EDITOR_EXE%" %*
