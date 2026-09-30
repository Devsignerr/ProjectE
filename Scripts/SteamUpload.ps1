<#
.SYNOPSIS
    SteamPipe 업로드: 패키징 → app/depot 빌드 스크립트(vdf) 생성 → steamcmd로 Steam에 올린다
.EXAMPLE
    .\Scripts\SteamUpload.ps1 -Account mybuilder                         # Projects\Sample 패키징 후 업로드 (기본 브랜치에는 올리기만)
    .\Scripts\SteamUpload.ps1 -Account mybuilder -SetLive beta -Description "0.2 테스트"
    .\Scripts\SteamUpload.ps1 -Account mybuilder -Preview                # 미리보기: 매니페스트만 만들고 올리지 않음
    .\Scripts\SteamUpload.ps1 -GenerateOnly -SkipPackage                 # vdf만 만들어 확인 (로그인 없음)
.NOTES
    - App ID는 .eproject "SteamAppId", Depot ID는 -DepotId (기본 App ID + 1 = Steamworks가 처음 만드는 디포).
    - 로그인: steamcmd가 비밀번호/Steam Guard를 직접 묻는다 (명령줄에 비밀번호를 넣지 않는다). 한 번 로그인하면 steamcmd가 자격을 기억한다.
      계정은 -Account 또는 환경 변수 STEAM_BUILD_ACCOUNT (빌드 전용 계정 권장 — Steamworks 앱 권한 "앱 메타데이터 편집/빌드 업로드").
    - steamcmd: -SteamCmd → 환경 변수 STEAMCMD → CMakeLocal.cmake의 E_STEAMWORKS_SDK_DIR\tools\ContentBuilder\builder\steamcmd.exe
    - 기본 브랜치(default) 공개는 Steamworks 웹에서 한다 (steamcmd의 SetLive는 베타 브랜치만).
    - 파트너 사이트 설정 확인: 설치 → 일반 설치 → 실행 옵션의 실행 파일 = <ExecutableName>.exe (패키지 루트)
#>
param(
    [string]$Project = "Projects\Sample",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",
    [uint32]$AppId = 0,
    [uint32]$DepotId = 0,
    [string]$Account = $env:STEAM_BUILD_ACCOUNT,
    [string]$Description = "",
    [string]$SetLive = "",
    [switch]$Preview,
    [switch]$SkipPackage,
    [switch]$GenerateOnly,
    [string]$SteamCmd = ""
)

$ErrorActionPreference = "Stop"
$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")

# steamcmd 찾기
function Find-SteamCmd([string]$Explicit) {
    if ($Explicit) { return $Explicit }
    if ($env:STEAMCMD) { return $env:STEAMCMD }
    $LocalCMake = Join-Path $RootDir "CMakeLocal.cmake"
    if (Test-Path $LocalCMake) {
        $Match = Select-String -Path $LocalCMake -Pattern 'E_STEAMWORKS_SDK_DIR\s+"([^"]+)"' | Select-Object -First 1
        if ($Match) {
            $Sdk = $Match.Matches[0].Groups[1].Value
            foreach ($Candidate in @("$Sdk\sdk\tools\ContentBuilder\builder\steamcmd.exe", "$Sdk\tools\ContentBuilder\builder\steamcmd.exe")) {
                if (Test-Path $Candidate) { return (Resolve-Path $Candidate).Path }
            }
        }
    }
    return ""
}

Push-Location $RootDir
try {
    # ---- 프로젝트
    $ProjectDir  = Resolve-Path $Project
    $ProjectFile = Get-ChildItem -Path $ProjectDir -Filter "*.eproject" | Select-Object -First 1
    if (-not $ProjectFile) { throw "프로젝트 파일(.eproject)을 찾을 수 없습니다: $ProjectDir" }
    $ProjectName = $ProjectFile.BaseName
    $Descriptor  = Get-Content $ProjectFile.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($AppId -eq 0) { $AppId = [uint32]$Descriptor.SteamAppId }
    if ($AppId -eq 0) { throw ".eproject에 SteamAppId가 없습니다 (-AppId로 지정 가능)" }
    if ($DepotId -eq 0) { $DepotId = $AppId + 1 }
    $ExeName = if ($Descriptor.ExecutableName) { $Descriptor.ExecutableName } elseif ($Descriptor.Name) { $Descriptor.Name } else { $ProjectName }
    $Version = if ($Descriptor.Version) { $Descriptor.Version } else { "1.0.0" }
    if (-not $Description) { $Description = "$ProjectName $Version ($Config) $(Get-Date -Format 'yyyy-MM-dd HH:mm')" }
    if ($SetLive -eq "default") { throw "기본 브랜치 공개는 Steamworks 웹에서 합니다 (-SetLive는 베타 브랜치 이름만)" }
    if ($Config -eq "Debug") { Write-Host "주의: Debug 패키지는 디버그 CRT가 필요해 일반 PC에서 실행되지 않습니다" -ForegroundColor Yellow }
    Write-Host "== SteamPipe: $ProjectName → App $AppId / Depot $DepotId ==" -ForegroundColor Cyan

    # ---- 1. 패키징
    $PackageDir = Join-Path $RootDir "Build\Package\$ProjectName"
    if (-not $SkipPackage) {
        & (Join-Path $PSScriptRoot "Package.ps1") -Project $Project -Config $Config
        if ($LASTEXITCODE -ne 0) { throw "패키징 실패" }
    }
    if (-not (Test-Path (Join-Path $PackageDir "$ExeName.exe"))) { throw "패키지가 없습니다 ($PackageDir\$ExeName.exe) — -SkipPackage를 빼고 실행하세요" }
    if (Test-Path (Join-Path $PackageDir "steam_appid.txt")) { throw "패키지에 steam_appid.txt가 있습니다 (개발용 파일 — 배포하면 Steam 재실행 검사가 꺼진다)" }
    $PackagedInfo = Join-Path $PackageDir "Engine\Packaged.json"
    if (Test-Path $PackagedInfo) {
        $Packaged = Get-Content $PackagedInfo -Raw | ConvertFrom-Json
        if ($Packaged.Config -ne $Config) { Write-Host "주의: 패키지 구성($($Packaged.Config))이 -Config($Config)와 다릅니다" -ForegroundColor Yellow }
    }

    # ---- 2. 빌드 스크립트 (Build\SteamPipe\<프로젝트>\)
    $PipeDir   = Join-Path $RootDir "Build\SteamPipe\$ProjectName"
    $OutputDir = Join-Path $PipeDir "output" # steamcmd 빌드 캐시/로그
    New-Item -ItemType Directory -Force $OutputDir | Out-Null
    $DepotVdf = Join-Path $PipeDir "depot_build_$DepotId.vdf"
    $AppVdf   = Join-Path $PipeDir "app_build_$AppId.vdf"
    # vdf 문자열에는 따옴표를 넣지 않고, 경로 끝에 \를 두지 않는다 (\" 이스케이프로 읽힐 수 있음)
    $SafeDescription = $Description -replace '"', "'"

    $DepotText = @"
"DepotBuild"
{
	"DepotID" "$DepotId"
	"FileMapping"
	{
		"LocalPath" "*"
		"DepotPath" "."
		"Recursive" "1"
	}
	"FileExclusion" "*.pdb"
}
"@
    $AppText = @"
"AppBuild"
{
	"AppID" "$AppId"
	"Desc" "$SafeDescription"
	"Preview" "$(if ($Preview) { 1 } else { 0 })"
	"SetLive" "$SetLive"
	"ContentRoot" "$PackageDir"
	"BuildOutput" "$OutputDir"
	"Depots"
	{
		"$DepotId" "$DepotVdf"
	}
}
"@
    $Utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($DepotVdf, $DepotText, $Utf8NoBom)
    [System.IO.File]::WriteAllText($AppVdf, $AppText, $Utf8NoBom)
    $FileCount  = (Get-ChildItem -Recurse -File $PackageDir | Where-Object { $_.Extension -ne ".pdb" } | Measure-Object).Count
    $TotalBytes = (Get-ChildItem -Recurse -File $PackageDir | Where-Object { $_.Extension -ne ".pdb" } | Measure-Object -Property Length -Sum).Sum
    Write-Host ("빌드 스크립트: {0}  (콘텐츠 {1}개 파일, {2:N1} MB, 설명 '{3}'{4}{5})" -f $AppVdf, $FileCount, ($TotalBytes / 1MB), $SafeDescription,
        $(if ($SetLive) { ", 브랜치 $SetLive 공개" } else { "" }), $(if ($Preview) { ", 미리보기" } else { "" })) -ForegroundColor Green
    Write-Host "Steamworks 실행 옵션의 실행 파일: $ExeName.exe" -ForegroundColor Green
    if ($GenerateOnly) {
        Write-Host "== vdf만 생성했습니다 (-GenerateOnly) ==" -ForegroundColor Cyan
        return
    }

    # ---- 3. 업로드
    $SteamCmdPath = Find-SteamCmd $SteamCmd
    if (-not $SteamCmdPath -or -not (Test-Path $SteamCmdPath)) {
        throw "steamcmd.exe를 찾을 수 없습니다. -SteamCmd, 환경 변수 STEAMCMD, 또는 CMakeLocal.cmake의 E_STEAMWORKS_SDK_DIR(SDK tools\ContentBuilder\builder)을 확인하세요"
    }
    if (-not $Account) { throw "Steam 계정이 필요합니다 (-Account 또는 환경 변수 STEAM_BUILD_ACCOUNT)" }
    Write-Host "== steamcmd 업로드 (로그인: $Account — 비밀번호/Steam Guard는 steamcmd가 묻습니다) ==" -ForegroundColor Cyan
    & $SteamCmdPath +login $Account +run_app_build $AppVdf +quit
    if ($LASTEXITCODE -ne 0) { throw "steamcmd 실패 (종료 코드 $LASTEXITCODE) — 로그: $OutputDir" }
    Write-Host "== 업로드 완료 — Steamworks 웹 → SteamPipe → 빌드에서 확인/공개하세요 (로그: $OutputDir) ==" -ForegroundColor Green
}
finally {
    Pop-Location
}
