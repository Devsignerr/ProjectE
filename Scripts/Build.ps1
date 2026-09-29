<#
.SYNOPSIS
    ProjectE 구성 + 빌드 스크립트 (VS 2022 번들 CMake / Ninja 사용, PATH 등록 불필요)
.EXAMPLE
    .\Scripts\Build.ps1                       # Ninja Debug 빌드
    .\Scripts\Build.ps1 -Config Release       # Ninja Release 빌드
    .\Scripts\Build.ps1 -Run                  # Debug 빌드 후 에디터(ProjectEEditor) 실행
    .\Scripts\Build.ps1 -RunSandbox           # Debug 빌드 후 Sandbox(런타임 데모) 실행
    .\Scripts\Build.ps1 -Test                 # Debug 빌드 후 단위 테스트(ctest) 실행
    .\Scripts\Build.ps1 -VisualStudio         # .sln 생성 (Build\vs2022\ProjectE.sln) + 빌드
    .\Scripts\Build.ps1 -Clean                # 빌드 디렉터리 삭제 후 처음부터
#>
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [switch]$Run,
    [switch]$RunSandbox,
    [switch]$Test,
    [switch]$Clean,
    [switch]$VisualStudio
)

$ErrorActionPreference = "Stop"
$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")

# vswhere로 C++ 도구가 포함된 VS 설치 경로 탐색
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) { throw "vswhere.exe를 찾을 수 없습니다. Visual Studio 2022가 설치되어 있는지 확인하세요." }
$VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsPath) { throw "C++ 도구가 포함된 Visual Studio를 찾을 수 없습니다." }

$CMake    = Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$CTest    = Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$NinjaDir = Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$VsDevCmd = Join-Path $VsPath "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $CMake)) { throw "VS 번들 CMake를 찾을 수 없습니다: $CMake" }

if ($VisualStudio) {
    $ConfigurePreset = "vs2022"
    $BuildPreset     = "vs2022-$($Config.ToLower())"
    $BuildDir        = Join-Path $RootDir "Build\vs2022"
    $BinDir          = Join-Path $BuildDir "Bin\$Config"
}
else {
    $ConfigurePreset = "ninja-$($Config.ToLower())"
    $BuildPreset     = $ConfigurePreset
    $BuildDir        = Join-Path $RootDir "Build\$ConfigurePreset"
    $BinDir          = Join-Path $BuildDir "Bin"
}
$EditorExe  = Join-Path $BinDir "ProjectEEditor.exe"
$SandboxExe = Join-Path $BinDir "Sandbox.exe"
$Exe        = $EditorExe

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "빌드 디렉터리 삭제: $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# Ninja + cl 조합은 VS 개발자 환경 변수(INCLUDE/LIB 등)가 필요하므로 VsDevCmd를 거쳐 실행
function Invoke-CMake([string[]]$Arguments) {
    if ($VisualStudio) {
        & $CMake @Arguments
    }
    else {
        $env:PATH = "$NinjaDir;$env:PATH"
        $Quoted = ($Arguments | ForEach-Object { "`"$_`"" }) -join " "
        cmd /c "`"$VsDevCmd`" -arch=x64 -host_arch=x64 -no_logo >nul 2>&1 && `"$CMake`" $Quoted"
    }
    if ($LASTEXITCODE -ne 0) { throw "CMake 실패 ($LASTEXITCODE): cmake $($Arguments -join ' ')" }
}

Push-Location $RootDir
try {
    Write-Host "== 구성 ($ConfigurePreset) ==" -ForegroundColor Cyan
    Invoke-CMake @("--preset", $ConfigurePreset)

    Write-Host "== 빌드 ($BuildPreset) ==" -ForegroundColor Cyan
    Invoke-CMake @("--build", "--preset", $BuildPreset)

    Write-Host "== 완료: $Exe ==" -ForegroundColor Green

    if ($Test) {
        Write-Host "== 테스트 ($BuildPreset) ==" -ForegroundColor Cyan
        & $CTest --preset $BuildPreset
        if ($LASTEXITCODE -ne 0) { throw "테스트 실패 ($LASTEXITCODE)" }
    }

    if ($Run) {
        Write-Host "== 에디터 실행 ==" -ForegroundColor Cyan
        Start-Process -FilePath $EditorExe -WorkingDirectory $RootDir
    }
    if ($RunSandbox) {
        Write-Host "== Sandbox 실행 ==" -ForegroundColor Cyan
        & $SandboxExe
    }
}
finally {
    Pop-Location
}
