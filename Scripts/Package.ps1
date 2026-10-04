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
        <ExecutableName>\            .eproject + Config + Content.epak (런타임이 exe 이름 폴더에서 프로젝트를 찾고 *.epak을 마운트한다)
    Content.epak: <Exe>\Content, <Exe>\Cooked, Engine\Content, Engine\Shaders\Cooked를 패키지 루트 기준 키로 묶은 것 (압축 없음, 항목별 해시 검사).
        Shaders.json(엔진 마커), Packaged.json, .eproject, Config는 파일로 남긴다. -NoPak이면 묶지 않는다 (디버깅)
    심볼: Build\Package\<프로젝트명>-Symbols\ 에 PDB + 같은 바이너리 (크래시 덤프 분석용, 배포하지 않는다)
    콘텐츠는 의존성 기준: ProjectECook --package-manifest가 루트(게임/서버 기본 맵, 플레이어 프리팹, 문자열 표, 설정 문자열,
        프로젝트 설정 패키징 → 추가 에셋 = Packaging.AdditionalAssets)에서 참조를 따라간 매니페스트(Build\Package\<프로젝트명>-Manifest.txt,
        규칙은 Tools\Cook\Source\PackageManifest.h 머리 주석)를 쓰고, 그 모델/단독 이미지(실제 용도만)만 쿠킹한다.
        스테이징은 매니페스트 F 파일 + 그 쿠킹본(.emodel / .<용도>.etex)만 복사한다. glTF 버퍼/이미지처럼 모델 원본만 쓰는 파일(I)은 넣지 않는다.
        코드가 경로를 조립해 여는 에셋은 Packaging.AdditionalAssets에 적는다. -AllContent: 예전처럼 Content 전체를 쿠킹·복사.
    Content의 원본 모델/이미지는 쿠킹본이 있으면 제외한다 (쿠킹본은 원본이 없으면 그대로 신뢰됨).
    -IncludeSources: 셰이더 소스 + DXC + 원본 에셋(매니페스트 I 파일 포함)까지 포함 (패키지에서 셰이더 핫 리로드/디버깅용)
    기본값은 프로젝트 설정(에디터 → 편집 → 프로젝트 설정 → 패키징 = Config\Packaging.json)이고 명령줄 인자가 우선한다.
    실행 파일 이름/아이콘/버전은 프로젝트 설정 → 프로젝트 정보(Config\Project.json).
#>
param(
    [string]$Project = "Projects\Sample",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "",
    [switch]$IncludeSources,
    [switch]$NoPak,
    [switch]$AllContent
)

$ErrorActionPreference = "Stop"
$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "ProjectSettings.ps1")

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
    $ProjectDir  = Resolve-Path $Project
    $Settings    = Read-ProjectSettings $ProjectDir
    $ProjectFile = $Settings.File
    $ProjectName = $ProjectFile.BaseName
    $ExeName     = $Settings.Info.ExecutableName
    if ($ExeName -match '[<>:"/\\|?*]') { throw "실행 파일 이름에 파일 이름으로 쓸 수 없는 문자가 있습니다: $ExeName" }
    $GameModule  = $Settings.GameModule
    if ($GameModule -and ($ExeName -eq $GameModule)) { throw "실행 파일 이름($ExeName)이 게임 모듈 이름과 같으면 PDB가 겹칩니다. 프로젝트 설정 → 프로젝트 정보 → 실행 파일 이름을 바꾸세요" }
    # 명령줄 인자가 없으면 프로젝트 설정(패키징)
    if (-not $Config) { $Config = $Settings.Packaging.Configuration }
    if (-not $PSBoundParameters.ContainsKey("IncludeSources")) { $IncludeSources = [bool]$Settings.Packaging.IncludeSourceAssets }
    if (-not $PSBoundParameters.ContainsKey("NoPak")) { $NoPak = -not [bool]$Settings.Packaging.UsePak }
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
    $ManifestPath = Join-Path $RootDir "Build\Package\$ProjectName-Manifest.txt"
    if ($AllContent) {
        & $CookExe --project $ProjectDir
    }
    else {
        & $CookExe --project $ProjectDir --package-manifest $ManifestPath
    }
    if ($LASTEXITCODE -ne 0) { throw "쿠킹 실패" }

    # 매니페스트 읽기 (F 패키지 파일, M 모델, T 단독 이미지 + 용도, I 모델 원본만 쓰는 파일, W 경고)
    $Manifest = $null
    if (-not $AllContent) {
        $Manifest = @{ Files = New-Object System.Collections.Generic.List[string]; Models = @{}; Images = @{}; Internal = New-Object System.Collections.Generic.List[string] }
        foreach ($Line in [System.IO.File]::ReadAllLines($ManifestPath, [System.Text.Encoding]::UTF8)) {
            if (-not $Line -or $Line.StartsWith("#")) { continue }
            $Parts = $Line.Split("`t")
            switch ($Parts[0]) {
                "F" { $Manifest.Files.Add($Parts[1]) }
                "M" { $Manifest.Models[$Parts[1].ToLowerInvariant()] = $true }
                "T" { $Manifest.Images[$Parts[1].ToLowerInvariant()] = @($Parts[2].Split(",")) }
                "I" { $Manifest.Internal.Add($Parts[1]) }
                "W" { Write-Host "매니페스트 경고: $($Parts[1])" -ForegroundColor Yellow }
            }
        }
        Write-Host ("매니페스트: 파일 {0}, 모델 {1}, 단독 이미지 {2}, 모델 내부 {3} → {4}" -f $Manifest.Files.Count, $Manifest.Models.Count, $Manifest.Images.Count, $Manifest.Internal.Count, $ManifestPath) -ForegroundColor Green
    }

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
    # Steamworks (SDK를 지정해 빌드했으면 엔진 DLL이 steam_api64.dll을 가져온다)
    $SteamDll = Join-Path $BinDir "steam_api64.dll"
    if (Test-Path $SteamDll) {
        Copy-Item $SteamDll $PackageDir
        if (-not [uint32]$Settings.Info.SteamAppId) { Write-Host "주의: steam_api64.dll은 있지만 프로젝트 설정에 Steam App ID가 없어 Steam을 초기화하지 않습니다" -ForegroundColor Yellow }
    }

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
        Version       = $Settings.Info.Version
        Config        = $Config
        EngineVersion = $Settings.EngineVersion
        PackagedAt    = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
    }
    [System.IO.File]::WriteAllText((Join-Path $PackageDir "Engine\Packaged.json"), ($PackagedInfo | ConvertTo-Json), (New-Object System.Text.UTF8Encoding $false))

    # 프로젝트: <ExeName>\ 에 .eproject + Content + Config (Saved 제외)
    $ProjectDst = Join-Path $PackageDir $ExeName
    New-Item -ItemType Directory -Force $ProjectDst | Out-Null
    Copy-Item $ProjectFile.FullName $ProjectDst
    $SourceExtensions = @(".glb", ".gltf", ".fbx", ".png", ".jpg", ".jpeg", ".tga", ".bmp")
    $ExcludedCount = 0
    $ContentSrcDir = Join-Path $ProjectDir "Content"
    $CookedSrcDir  = Join-Path $ProjectDir "Cooked"
    if ($Manifest) {
        # 매니페스트 기준: F 파일(+ -IncludeSources면 I 파일)만, 쿠킹본이 있는 원본 모델/이미지는 빼고 그 쿠킹본을 넣는다
        $ContentDst = Join-Path $ProjectDst "Content"
        $CookedDst  = Join-Path $ProjectDst "Cooked"
        $Staged = New-Object System.Collections.Generic.List[string]
        $Staged.AddRange($Manifest.Files)
        if ($IncludeSources) { $Staged.AddRange($Manifest.Internal) }
        $CookedFiles = New-Object System.Collections.Generic.List[string] # Cooked 기준 상대 경로
        foreach ($Relative in $Staged) {
            $Key = $Relative.ToLowerInvariant()
            $Cooked = @()
            if ($Manifest.Models.ContainsKey($Key)) { $Cooked = @("$Relative.emodel") }
            elseif ($Manifest.Images.ContainsKey($Key)) { $Cooked = @($Manifest.Images[$Key] | ForEach-Object { "$Relative.$_.etex" }) }
            foreach ($CookedRelative in $Cooked) {
                if (-not (Test-Path -LiteralPath (Join-Path $CookedSrcDir $CookedRelative))) { throw "쿠킹본이 없습니다 (쿠킹 실패?): Cooked\$CookedRelative" }
                $CookedFiles.Add($CookedRelative)
            }
            $Extension = [System.IO.Path]::GetExtension($Relative).ToLowerInvariant()
            if (-not $IncludeSources -and $Cooked.Count -gt 0 -and ($SourceExtensions -contains $Extension)) {
                $ExcludedCount++
                continue
            }
            $Destination = Join-Path $ContentDst $Relative
            New-Item -ItemType Directory -Force (Split-Path $Destination -Parent) | Out-Null
            Copy-Item -LiteralPath (Join-Path $ContentSrcDir $Relative) -Destination $Destination
        }
        # 쿠킹 에셋: 매니페스트 모델/단독 이미지의 쿠킹본만. Copy-Item은 수정 시각을 보존하므로 원본보다 새롭다는 판정이 유지된다
        foreach ($CookedRelative in $CookedFiles) {
            $Destination = Join-Path $CookedDst $CookedRelative
            New-Item -ItemType Directory -Force (Split-Path $Destination -Parent) | Out-Null
            Copy-Item -LiteralPath (Join-Path $CookedSrcDir $CookedRelative) -Destination $Destination
        }
    }
    elseif (Test-Path $ContentSrcDir) {
        Copy-Item -Recurse -Force $ContentSrcDir (Join-Path $ProjectDst "Content")
    }
    # -AllContent: 쿠킹본이 있는 원본 모델/이미지 제외 (Cooked/<상대 경로>.emodel 또는 .<용도>.etex)
    if (-not $Manifest -and -not $IncludeSources -and (Test-Path (Join-Path $ProjectDst "Content"))) {
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
    # 프로젝트 설정 → 패키징 → 추가 폴더 (pak에 넣지 않고 파일로)
    foreach ($Extra in ("$($Settings.Packaging.AdditionalDirectories)" -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        $ExtraSrc = Join-Path $ProjectDir $Extra
        if (-not (Test-Path $ExtraSrc)) { throw "프로젝트 설정의 추가 폴더가 없습니다: $ExtraSrc" }
        Copy-Item -Recurse -Force $ExtraSrc (Join-Path $ProjectDst $Extra)
        Write-Host "추가 폴더: $Extra" -ForegroundColor Green
    }
    # -AllContent: 쿠킹 에셋 전체 (.emodel/.<용도>.etex, 이전 형식 .etex 제외). Copy-Item은 수정 시각을 보존하므로 원본보다 새롭다는 판정이 유지된다
    if (-not $Manifest -and (Test-Path (Join-Path $ProjectDir "Cooked"))) {
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
    $ContentStats = Get-ChildItem (Join-Path $ProjectDst "Content") -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum
    $CookedStats  = Get-ChildItem (Join-Path $ProjectDst "Cooked") -Recurse -File -Include "*.emodel", "*.etex" -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum
    Write-Host ("검사: Content 파일 {0}개 ({1:N1} MB){2}" -f $ContentStats.Count, ($ContentStats.Sum / 1MB), $(if ($Manifest) { " — 매니페스트 기준" } else { " — -AllContent" })) -ForegroundColor Green
    Write-Host ("검사: 쿠킹된 에셋 {0}개 ({1:N1} MB)" -f $CookedStats.Count, ($CookedStats.Sum / 1MB)) -ForegroundColor Green

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

    # ---- 5. pak: 콘텐츠 폴더를 하나로 묶고 원본 폴더는 지운다
    if (-not $NoPak) {
        $PakDirs = @("$ExeName\Content", "$ExeName\Cooked", "Engine\Content", "Engine\Shaders\Cooked") |
            Where-Object { Test-Path (Join-Path $PackageDir $_) }
        $PakFile = Join-Path $ProjectDst "Content.epak"
        & $CookExe --make-pak $PakFile --pak-root $PackageDir --pak-dirs ($PakDirs -join ";")
        if ($LASTEXITCODE -ne 0) { throw "pak 생성 실패" }
        foreach ($Dir in $PakDirs) { Remove-Item -Recurse -Force (Join-Path $PackageDir $Dir) }
        Write-Host ("pak: {0} ({1:N1} MB, {2})" -f $PakFile, ((Get-Item $PakFile).Length / 1MB), ($PakDirs -join ', ')) -ForegroundColor Green
    }

    $TotalBytes = (Get-ChildItem -Recurse -File $PackageDir | Measure-Object -Property Length -Sum).Sum
    Write-Host ("== 완료: {0} ({1:N1} MB) ==" -f $PackageDir, ($TotalBytes / 1MB)) -ForegroundColor Green
    Write-Host "실행: $PackageExe"
    Write-Host "심볼: $SymbolsDir"
}
finally {
    Pop-Location
}
