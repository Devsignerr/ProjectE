<#
.SYNOPSIS
    ProjectE 패키징: Release 빌드 → 셰이더 쿠킹 → Build\Package\<프로젝트명>\ 에 스테이징
.EXAMPLE
    .\Scripts\Package.ps1                              # Projects\Sample, Release
    .\Scripts\Package.ps1 -Project Projects\MyGame     # 다른 프로젝트
    .\Scripts\Package.ps1 -Config Debug                # 디버그 패키지 (검증용)
.NOTES
    배포 패키지: 셰이더는 쿠킹된 DXIL + Shaders.json(엔진 마커)만, DXC DLL은 포함하지 않는다 (dxcompiler.dll 지연 로드).
    Content의 원본 모델/이미지는 쿠킹본이 있으면 제외한다 (쿠킹본은 원본이 없으면 그대로 신뢰됨).
    -IncludeSources: 셰이더 소스 + DXC + 원본 에셋까지 포함 (패키지에서 셰이더 핫 리로드/디버깅용)
#>
param(
    [string]$Project = "Projects\Sample",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",
    [switch]$IncludeSources
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
    # 게임 모듈 (.eproject "GameModule")
    $GameModule = (Get-Content $ProjectFile.FullName -Raw -Encoding UTF8 | ConvertFrom-Json).GameModule
    $RequiredFiles = @("ProjectERuntime.exe", "ProjectECook.exe", "ProjectEEngine.dll")
    if ($GameModule) { $RequiredFiles += "$GameModule.dll" }
    foreach ($Required in $RequiredFiles) {
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
    Copy-Item (Join-Path $BinDir "ProjectEEngine.dll") $PackageDir
    if ($GameModule) { Copy-Item (Join-Path $BinDir "$GameModule.dll") $PackageDir }

    # 엔진 셰이더: Shaders.json(FPaths 엔진 마커) + 이 구성의 쿠킹 DXIL (Debug 빌드는 *.debug.dxil을 찾는다)
    $EngineShaderSrc = Join-Path $RootDir "Engine\Shaders"
    $EngineShaderDst = Join-Path $PackageDir "Engine\Shaders"
    New-Item -ItemType Directory -Force (Join-Path $EngineShaderDst "Cooked") | Out-Null
    Copy-Item (Join-Path $EngineShaderSrc "Shaders.json") $EngineShaderDst
    $CookedShaders = Get-ChildItem (Join-Path $EngineShaderSrc "Cooked") -Filter "*.dxil" |
        Where-Object { ($Config -eq "Debug") -eq ($_.Name -like "*.debug.dxil") }
    $CookedShaders | Copy-Item -Destination (Join-Path $EngineShaderDst "Cooked")
    if ($IncludeSources) {
        Copy-Item (Join-Path $EngineShaderSrc "*.hlsl*") $EngineShaderDst
        Copy-Item (Join-Path $BinDir "dxcompiler.dll") $PackageDir
        Copy-Item (Join-Path $BinDir "dxil.dll") $PackageDir
    }

    # 프로젝트: .eproject + Content + Config (Saved 제외)
    $ProjectDst = Join-Path $PackageDir "Projects\$ProjectName"
    New-Item -ItemType Directory -Force $ProjectDst | Out-Null
    Copy-Item $ProjectFile.FullName $ProjectDst
    if (Test-Path (Join-Path $ProjectDir "Content")) {
        Copy-Item -Recurse -Force (Join-Path $ProjectDir "Content") (Join-Path $ProjectDst "Content")
    }
    # 쿠킹본이 있는 원본 모델/이미지 제외 (Cooked/<상대 경로>.emodel 또는 .<용도>.etex)
    $SourceExtensions = @(".glb", ".gltf", ".png", ".jpg", ".jpeg", ".tga", ".bmp")
    $ExcludedCount = 0
    if (-not $IncludeSources -and (Test-Path (Join-Path $ProjectDst "Content"))) {
        $ContentDst = (Resolve-Path (Join-Path $ProjectDst "Content")).Path
        foreach ($File in (Get-ChildItem -Recurse -File $ContentDst | Where-Object { $SourceExtensions -contains $_.Extension.ToLower() })) {
            $Relative  = $File.FullName.Substring($ContentDst.Length + 1)
            $CookedDir = Join-Path $ProjectDir "Cooked"
            $Cooked    = @(Get-ChildItem -Path (Split-Path (Join-Path $CookedDir $Relative) -Parent) -Filter "$($File.Name).*" -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '\.(emodel|(color|linear|normal|mask)\.etex)$' })
            if ($Cooked.Count -gt 0) {
                Remove-Item $File.FullName
                $ExcludedCount++
            }
        }
    }
    if (Test-Path (Join-Path $ProjectDir "Config")) {
        Copy-Item -Recurse -Force (Join-Path $ProjectDir "Config") (Join-Path $ProjectDst "Config")
    }
    # 쿠킹 에셋 (.emodel/.<용도>.etex, 이전 형식 .etex 제외). Copy-Item은 수정 시각을 보존하므로 원본보다 새롭다는 판정이 유지된다
    if (Test-Path (Join-Path $ProjectDir "Cooked")) {
        Copy-Item -Recurse -Force (Join-Path $ProjectDir "Cooked") (Join-Path $ProjectDst "Cooked")
        Get-ChildItem -Recurse -File (Join-Path $ProjectDst "Cooked") |
            Where-Object { $_.Name -notmatch '\.(emodel|(color|linear|normal|mask)\.etex)$' } | Remove-Item
    }

    # 실행 배치 (Sample이 아닌 프로젝트도 --project로 명시)
    $RunBat = "@echo off`r`ncd /d `"%~dp0`"`r`nstart `"`" `"%~dp0ProjectERuntime.exe`" --project `"Projects\$ProjectName`"`r`n"
    [System.IO.File]::WriteAllText((Join-Path $PackageDir "Run.bat"), $RunBat, (New-Object System.Text.UTF8Encoding $true))

    # ---- 4. 검사: exe 위치에서 엔진 마커가 발견되는지
    $Marker = Join-Path $PackageDir "Engine\Shaders\Shaders.json"
    if (Test-Path $Marker) {
        Write-Host "검사: 엔진 마커 발견 → $Marker" -ForegroundColor Green
    }
    else {
        throw "검사 실패: 엔진 마커(Engine\Shaders\Shaders.json)가 패키지에 없습니다"
    }
    $CookedCount = (Get-ChildItem (Join-Path $EngineShaderDst "Cooked") -Filter "*.dxil" -ErrorAction SilentlyContinue | Measure-Object).Count
    if ($CookedCount -eq 0) { throw "검사 실패: 쿠킹된 셰이더가 없습니다" }
    Write-Host "검사: 쿠킹된 셰이더 $CookedCount 개" -ForegroundColor Green
    Write-Host "검사: 쿠킹본으로 대체되어 제외한 원본 에셋 $ExcludedCount 개" -ForegroundColor Green
    $CookedAssetCount = (Get-ChildItem (Join-Path $ProjectDst "Cooked") -Recurse -Include "*.emodel", "*.etex" -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Host "검사: 쿠킹된 에셋 $CookedAssetCount 개" -ForegroundColor Green

    $TotalBytes = (Get-ChildItem -Recurse -File $PackageDir | Measure-Object -Property Length -Sum).Sum
    Write-Host ("== 완료: {0} ({1:N1} MB) ==" -f $PackageDir, ($TotalBytes / 1MB)) -ForegroundColor Green
    Write-Host "실행: $PackageDir\Run.bat"
}
finally {
    Pop-Location
}
