<#
.SYNOPSIS
    ProjectE 패키징: Release 빌드 → 셰이더 쿠킹 → Build\Package\<프로젝트명>\ 에 스테이징
.EXAMPLE
    .\Scripts\Package.ps1                              # Projects\Sample, Release
    .\Scripts\Package.ps1 -Project Projects\MyGame     # 다른 프로젝트
    .\Scripts\Package.ps1 -Config Debug                # 디버그 패키지 (검증용)
.NOTES
    과도기 패키지: 쿠킹된 DXIL과 함께 셰이더 소스와 dxcompiler.dll/dxil.dll도 포함한다
    (런타임이 쿠킹 파일이 없거나 오래되면 DXC로 폴백 컴파일하기 때문). 에셋 쿠킹은 후속 단계.
#>
param(
    [string]$Project = "Projects\Sample",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $RootDir
try {
    # ---- 프로젝트 확인
    $ProjectDir = Resolve-Path $Project
    $ProjectFile = Get-ChildItem -Path $ProjectDir -Filter "*.eproject" | Select-Object -First 1
    if (-not $ProjectFile) { throw "프로젝트 파일(.eproject)을 찾을 수 없습니다: $ProjectDir" }
    $ProjectName = $ProjectFile.BaseName
    Write-Host "== 패키징: $ProjectName ($Config) ==" -ForegroundColor Cyan

    # ---- 1. 빌드
    & (Join-Path $PSScriptRoot "Build.ps1") -Config $Config
    if ($LASTEXITCODE -ne 0) { throw "빌드 실패" }

    $BinDir = Join-Path $RootDir "Build\ninja-$($Config.ToLower())\Bin"
    foreach ($Required in @("ProjectERuntime.exe", "ProjectECook.exe", "dxcompiler.dll", "dxil.dll")) {
        if (-not (Test-Path (Join-Path $BinDir $Required))) { throw "빌드 산출물이 없습니다: $Required" }
    }

    # ---- 2. 쿠킹 (셰이더 → Engine\Shaders\Cooked)
    Write-Host "== 쿠킹 ==" -ForegroundColor Cyan
    & (Join-Path $BinDir "ProjectECook.exe") --project $ProjectDir
    if ($LASTEXITCODE -ne 0) { throw "쿠킹 실패" }

    # ---- 3. 스테이징
    $PackageDir = Join-Path $RootDir "Build\Package\$ProjectName"
    Write-Host "== 스테이징: $PackageDir ==" -ForegroundColor Cyan
    if (Test-Path $PackageDir) { Remove-Item -Recurse -Force $PackageDir }
    New-Item -ItemType Directory -Force $PackageDir | Out-Null

    Copy-Item (Join-Path $BinDir "ProjectERuntime.exe") $PackageDir
    Copy-Item (Join-Path $BinDir "dxcompiler.dll") $PackageDir
    Copy-Item (Join-Path $BinDir "dxil.dll") $PackageDir

    # 엔진 셰이더 (소스 + Cooked). FPaths 마커(Engine/Shaders/Common.hlsli)가 exe 옆에 오도록 배치
    $EngineShaderDst = Join-Path $PackageDir "Engine\Shaders"
    New-Item -ItemType Directory -Force $EngineShaderDst | Out-Null
    Copy-Item -Recurse -Force (Join-Path $RootDir "Engine\Shaders\*") $EngineShaderDst

    # 프로젝트: .eproject + Content + Config (Saved 제외)
    $ProjectDst = Join-Path $PackageDir "Projects\$ProjectName"
    New-Item -ItemType Directory -Force $ProjectDst | Out-Null
    Copy-Item $ProjectFile.FullName $ProjectDst
    if (Test-Path (Join-Path $ProjectDir "Content")) {
        Copy-Item -Recurse -Force (Join-Path $ProjectDir "Content") (Join-Path $ProjectDst "Content")
    }
    if (Test-Path (Join-Path $ProjectDir "Config")) {
        Copy-Item -Recurse -Force (Join-Path $ProjectDir "Config") (Join-Path $ProjectDst "Config")
    }
    # 쿠킹 에셋 (.emodel/.etex). Copy-Item은 수정 시각을 보존하므로 원본보다 새롭다는 판정이 유지된다
    if (Test-Path (Join-Path $ProjectDir "Cooked")) {
        Copy-Item -Recurse -Force (Join-Path $ProjectDir "Cooked") (Join-Path $ProjectDst "Cooked")
    }

    # 실행 배치 (Sample이 아닌 프로젝트도 --project로 명시)
    $RunBat = "@echo off`r`ncd /d `"%~dp0`"`r`nstart `"`" `"%~dp0ProjectERuntime.exe`" --project `"Projects\$ProjectName`"`r`n"
    [System.IO.File]::WriteAllText((Join-Path $PackageDir "Run.bat"), $RunBat, (New-Object System.Text.UTF8Encoding $true))

    # ---- 4. 검사: exe 위치에서 엔진 마커가 발견되는지
    $Marker = Join-Path $PackageDir "Engine\Shaders\Common.hlsli"
    if (Test-Path $Marker) {
        Write-Host "검사: 엔진 마커 발견 → $Marker" -ForegroundColor Green
    }
    else {
        throw "검사 실패: 엔진 마커(Engine\Shaders\Common.hlsli)가 패키지에 없습니다"
    }
    $CookedCount = (Get-ChildItem (Join-Path $EngineShaderDst "Cooked") -Filter "*.dxil" -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Host "검사: 쿠킹된 셰이더 $CookedCount 개" -ForegroundColor Green
    $CookedAssetCount = (Get-ChildItem (Join-Path $ProjectDst "Cooked") -Recurse -Include "*.emodel", "*.etex" -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Host "검사: 쿠킹된 에셋 $CookedAssetCount 개" -ForegroundColor Green

    $TotalBytes = (Get-ChildItem -Recurse -File $PackageDir | Measure-Object -Property Length -Sum).Sum
    Write-Host ("== 완료: {0} ({1:N1} MB) ==" -f $PackageDir, ($TotalBytes / 1MB)) -ForegroundColor Green
    Write-Host "실행: $PackageDir\Run.bat"
}
finally {
    Pop-Location
}
