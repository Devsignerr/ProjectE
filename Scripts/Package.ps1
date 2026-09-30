<#
.SYNOPSIS
    ProjectE 패키징: Release 빌드 → 쿠킹 → Build\Package\<프로젝트명>\ 에 배포용 폴더 스테이징
.EXAMPLE
    .\Scripts\Package.ps1                              # Projects\Sample, Release
    .\Scripts\Package.ps1 -Project Projects\MyGame     # 다른 프로젝트
    .\Scripts\Package.ps1 -Config Debug                # 디버그 패키지 (검증용, 배포 불가 — 디버그 CRT)
.NOTES
    패키지 배치 (exe를 더블클릭하면 바로 실행):
        <ExecutableName>.exe         ProjectERuntime.exe 복사본 + 프로젝트 아이콘/버전 리소스 (ProjectECook --stamp-exe)
        ProjectEEngine.dll, <GameModule>.dll, VC++ 런타임 DLL (app-local)
        Engine\Packaged.json         패키지 표식 → 런타임이 Saved를 %LOCALAPPDATA%로 옮긴다
        Engine\Shaders\              쿠킹된 DXIL + Shaders.json(엔진 마커). DXC DLL은 넣지 않는다 (dxcompiler.dll 지연 로드)
        Engine\Content\              엔진 콘텐츠 (기본 글꼴 등)
        <ExecutableName>\            .eproject + Content + Cooked + Config (런타임이 exe 이름 폴더에서 프로젝트를 찾는다)
    심볼: Build\Package\<프로젝트명>-Symbols\ 에 PDB + 같은 바이너리 (크래시 덤프 분석용, 배포하지 않는다)
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

# VS 설치 경로 (VC++ 재배포 DLL, dumpbin)
function Get-VsInstallPath {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $VsWhere)) { throw "vswhere.exe를 찾을 수 없습니다: $VsWhere" }
    $Path = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $Path) { throw "VC++ 도구가 설치된 Visual Studio를 찾을 수 없습니다" }
    return $Path
}

# 가장 높은 버전 폴더 (이름이 버전 형식인 것만)
function Get-LatestVersionDirectory([string]$Parent) {
    Get-ChildItem -Directory $Parent -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
}

Push-Location $RootDir
try {
    # ---- 프로젝트 확인
    $ProjectDir = Resolve-Path $Project
    $ProjectFile = Get-ChildItem -Path $ProjectDir -Filter "*.eproject" | Select-Object -First 1
    if (-not $ProjectFile) { throw "프로젝트 파일(.eproject)을 찾을 수 없습니다: $ProjectDir" }
    $ProjectName = $ProjectFile.BaseName
    $Descriptor  = Get-Content $ProjectFile.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    $ExeName     = if ($Descriptor.ExecutableName) { $Descriptor.ExecutableName } elseif ($Descriptor.Name) { $Descriptor.Name } else { $ProjectName }
    if ($ExeName -match '[<>:"/\\|?*]') { throw "ExecutableName에 파일 이름으로 쓸 수 없는 문자가 있습니다: $ExeName" }
    $GameModule  = $Descriptor.GameModule
    if ($GameModule -and ($ExeName -eq $GameModule)) { throw "ExecutableName($ExeName)이 게임 모듈 이름과 같으면 PDB가 겹칩니다. .eproject ExecutableName을 바꾸세요" }
    Write-Host "== 패키징: $ProjectName → $ExeName.exe ($Config) ==" -ForegroundColor Cyan

    # ---- 1. 빌드
    & (Join-Path $PSScriptRoot "Build.ps1") -Config $Config
    if ($LASTEXITCODE -ne 0) { throw "빌드 실패" }

    $BinDir = Join-Path $RootDir "Build\ninja-$($Config.ToLower())\Bin"
    $Binaries = @("ProjectERuntime.exe", "ProjectEEngine.dll")
    if ($GameModule) { $Binaries += "$GameModule.dll" }
    foreach ($Required in ($Binaries + "ProjectECook.exe")) {
        if (-not (Test-Path (Join-Path $BinDir $Required))) { throw "빌드 산출물이 없습니다: $Required" }
    }
    $CookExe = Join-Path $BinDir "ProjectECook.exe"

    # ---- 2. 쿠킹 (셰이더 → Engine\Shaders\Cooked, 에셋 → <프로젝트>\Cooked)
    Write-Host "== 쿠킹 ==" -ForegroundColor Cyan
    & $CookExe --project $ProjectDir
    if ($LASTEXITCODE -ne 0) { throw "쿠킹 실패" }

    # ---- 3. 스테이징
    $PackageDir = Join-Path $RootDir "Build\Package\$ProjectName"
    $SymbolsDir = Join-Path $RootDir "Build\Package\$ProjectName-Symbols"
    Write-Host "== 스테이징: $PackageDir ==" -ForegroundColor Cyan
    foreach ($Dir in @($PackageDir, $SymbolsDir)) {
        if (Test-Path $Dir) { Remove-Item -Recurse -Force $Dir }
        New-Item -ItemType Directory -Force $Dir | Out-Null
    }

    # 실행 파일: 런타임 복사본에 프로젝트 아이콘/버전 리소스를 써 넣는다
    $PackageExe = Join-Path $PackageDir "$ExeName.exe"
    Copy-Item (Join-Path $BinDir "ProjectERuntime.exe") $PackageExe
    & $CookExe --project $ProjectDir --stamp-exe $PackageExe
    if ($LASTEXITCODE -ne 0) { throw "실행 파일 스탬프 실패" }
    Copy-Item (Join-Path $BinDir "ProjectEEngine.dll") $PackageDir
    if ($GameModule) { Copy-Item (Join-Path $BinDir "$GameModule.dll") $PackageDir }

    # VC++ 런타임 (app-local). 디버그 CRT는 재배포할 수 없으므로 Release만
    if ($Config -eq "Release") {
        $VsPath   = Get-VsInstallPath
        $RedistVer = Get-LatestVersionDirectory (Join-Path $VsPath "VC\Redist\MSVC")
        if (-not $RedistVer) { throw "VC++ 재배포 폴더를 찾을 수 없습니다: $VsPath\VC\Redist\MSVC" }
        $CrtDir = Get-ChildItem -Directory (Join-Path $RedistVer.FullName "x64") -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
        if (-not $CrtDir) { throw "Microsoft.VC*.CRT 폴더가 없습니다: $($RedistVer.FullName)\x64" }
        Get-ChildItem $CrtDir.FullName -Filter "*.dll" | Copy-Item -Destination $PackageDir
        Write-Host "VC++ 런타임: $($CrtDir.Name) $($RedistVer.Name)" -ForegroundColor Green
    }
    else {
        Write-Host "주의: Debug 패키지는 디버그 CRT가 필요해 VS가 없는 PC에서 실행되지 않습니다 (검증용)" -ForegroundColor Yellow
    }

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

    # 엔진 콘텐츠 (게임 UI 기본 글꼴 등)
    $EngineContentSrc = Join-Path $RootDir "Engine\Content"
    if (Test-Path $EngineContentSrc) {
        Copy-Item -Recurse -Force $EngineContentSrc (Join-Path $PackageDir "Engine\Content")
    }

    # 패키지 표식 (FPaths::IsPackaged)
    $PackagedInfo = [ordered]@{
        Project       = $ProjectName
        Version       = if ($Descriptor.Version) { $Descriptor.Version } else { "1.0.0" }
        Config        = $Config
        EngineVersion = $Descriptor.EngineVersion
        PackagedAt    = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
    }
    [System.IO.File]::WriteAllText((Join-Path $PackageDir "Engine\Packaged.json"), ($PackagedInfo | ConvertTo-Json), (New-Object System.Text.UTF8Encoding $false))

    # 프로젝트: <ExeName>\ 에 .eproject + Content + Config (Saved 제외)
    $ProjectDst = Join-Path $PackageDir $ExeName
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

    # 심볼: PDB + 패키지와 같은 바이너리 (덤프를 열 때 디버거가 둘 다 찾는다)
    foreach ($Binary in $Binaries) {
        $Pdb = Join-Path $BinDir ([System.IO.Path]::ChangeExtension($Binary, ".pdb"))
        if (Test-Path $Pdb) { Copy-Item $Pdb $SymbolsDir }
        else { Write-Host "주의: PDB가 없습니다: $Pdb" -ForegroundColor Yellow }
    }
    Copy-Item $PackageExe $SymbolsDir
    Copy-Item (Join-Path $PackageDir "ProjectEEngine.dll") $SymbolsDir
    if ($GameModule) { Copy-Item (Join-Path $PackageDir "$GameModule.dll") $SymbolsDir }

    # ---- 4. 검사
    $Marker = Join-Path $PackageDir "Engine\Shaders\Shaders.json"
    if (-not (Test-Path $Marker)) { throw "검사 실패: 엔진 마커(Engine\Shaders\Shaders.json)가 패키지에 없습니다" }
    Write-Host "검사: 엔진 마커 발견 → $Marker" -ForegroundColor Green
    $CookedCount = (Get-ChildItem (Join-Path $EngineShaderDst "Cooked") -Filter "*.dxil" -ErrorAction SilentlyContinue | Measure-Object).Count
    if ($CookedCount -eq 0) { throw "검사 실패: 쿠킹된 셰이더가 없습니다" }
    Write-Host "검사: 쿠킹된 셰이더 $CookedCount 개" -ForegroundColor Green
    Write-Host "검사: 쿠킹본으로 대체되어 제외한 원본 에셋 $ExcludedCount 개" -ForegroundColor Green
    $CookedAssetCount = (Get-ChildItem (Join-Path $ProjectDst "Cooked") -Recurse -Include "*.emodel", "*.etex" -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Host "검사: 쿠킹된 에셋 $CookedAssetCount 개" -ForegroundColor Green

    # 종속 DLL: 패키지 바이너리가 가져오는 DLL이 패키지 안 또는 Windows 기본 DLL이어야 한다.
    # 개발 PC에는 VC++ 런타임이 System32에 있으므로 vcruntime/msvcp 계열은 패키지 안에 있어야만 통과
    $ToolsVer = Get-LatestVersionDirectory (Join-Path (Get-VsInstallPath) "VC\Tools\MSVC")
    $DumpBin  = Join-Path $ToolsVer.FullName "bin\Hostx64\x64\dumpbin.exe"
    $System32 = Join-Path $env:SystemRoot "System32"
    $Missing  = @()
    foreach ($Binary in (Get-ChildItem $PackageDir -File | Where-Object { $_.Extension -in ".exe", ".dll" })) {
        $Lines = & $DumpBin /nologo /dependents $Binary.FullName
        $InSection = $false
        foreach ($Line in $Lines) {
            if ($Line -match 'Image has the following dependencies') { $InSection = $true; continue }
            if ($InSection -and $Line -match 'Image has the following delay load dependencies|Summary') { break }
            if (-not $InSection -or $Line.Trim() -eq "") { continue }
            $Dependency = $Line.Trim()
            $IsCrt      = $Dependency -match '^(vcruntime|msvcp|concrt|vccorlib)'
            $InPackage  = Test-Path (Join-Path $PackageDir $Dependency)
            $InSystem   = (Test-Path (Join-Path $System32 $Dependency)) -or ($Dependency -like "api-ms-win-*") -or ($Dependency -like "ext-ms-*")
            if (-not $InPackage -and ($IsCrt -or -not $InSystem)) {
                if ($Config -eq "Debug" -and $IsCrt) { continue } # 디버그 CRT는 동봉하지 않는다 (위 주의 참고)
                $Missing += "$($Binary.Name) → $Dependency"
            }
        }
    }
    if ($Missing.Count -gt 0) { throw "검사 실패: 패키지에 없는 종속 DLL`n  $($Missing -join "`n  ")" }
    Write-Host "검사: 종속 DLL 모두 패키지 또는 Windows 기본 DLL" -ForegroundColor Green

    $TotalBytes = (Get-ChildItem -Recurse -File $PackageDir | Measure-Object -Property Length -Sum).Sum
    Write-Host ("== 완료: {0} ({1:N1} MB) ==" -f $PackageDir, ($TotalBytes / 1MB)) -ForegroundColor Green
    Write-Host "실행: $PackageExe"
    Write-Host "심볼: $SymbolsDir"
}
finally {
    Pop-Location
}
