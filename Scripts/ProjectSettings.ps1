<#
.SYNOPSIS
    프로젝트 설정 읽기 (Package.ps1 / SteamUpload.ps1이 dot-source로 쓴다)
.NOTES
    엔진(FProjectSettings)과 같은 규칙: 기본값 → .eproject의 예전 필드(마이그레이션) → Config\<섹션>.json
    반환: @{ Name; GameModule; EngineVersion; Info = 프로젝트 정보; Packaging = 패키징 }
#>

function Read-JsonFile([string]$Path) {
    if (Test-Path $Path) { return Get-Content $Path -Raw -Encoding UTF8 | ConvertFrom-Json }
    return $null
}

function Read-ProjectSettings([string]$ProjectDir) {
    $ProjectFile = Get-ChildItem -Path $ProjectDir -Filter "*.eproject" | Select-Object -First 1
    if (-not $ProjectFile) { throw "프로젝트 파일(.eproject)을 찾을 수 없습니다: $ProjectDir" }
    $Descriptor = Read-JsonFile $ProjectFile.FullName
    $Name       = if ($Descriptor.Name) { $Descriptor.Name } else { $ProjectFile.BaseName }

    # 프로젝트 정보: 기본값 ← .eproject 예전 필드 ← Config\Project.json
    $Info = [ordered]@{ DisplayName = ""; Version = "1.0.0"; Company = ""; Icon = ""; ExecutableName = ""; SteamAppId = 0; SteamDepotId = 0 }
    foreach ($Key in @($Info.Keys)) {
        if ($null -ne $Descriptor.$Key -and "$($Descriptor.$Key)" -ne "") { $Info[$Key] = $Descriptor.$Key }
    }
    $ProjectJson = Read-JsonFile (Join-Path $ProjectDir "Config\Project.json")
    if ($ProjectJson) {
        foreach ($Key in @($Info.Keys)) { if ($null -ne $ProjectJson.$Key) { $Info[$Key] = $ProjectJson.$Key } }
    }
    if (-not $Info.ExecutableName) { $Info.ExecutableName = $Name }
    if (-not $Info.DisplayName) { $Info.DisplayName = $Name }
    if ([uint32]$Info.SteamDepotId -eq 0 -and [uint32]$Info.SteamAppId -ne 0) { $Info.SteamDepotId = [uint32]$Info.SteamAppId + 1 }

    # 패키징: 기본값 ← Config\Packaging.json
    $Packaging = [ordered]@{ Configuration = "Release"; UsePak = $true; IncludeSourceAssets = $false; AdditionalDirectories = ""; AdditionalAssets = "" }
    $PackagingJson = Read-JsonFile (Join-Path $ProjectDir "Config\Packaging.json")
    if ($PackagingJson) {
        foreach ($Key in @($Packaging.Keys)) { if ($null -ne $PackagingJson.$Key) { $Packaging[$Key] = $PackagingJson.$Key } }
    }
    if ($Packaging.Configuration -notin @("Release", "Debug")) { throw "Config\Packaging.json Configuration은 Release 또는 Debug여야 합니다: $($Packaging.Configuration)" }

    return [pscustomobject]@{
        File          = $ProjectFile
        Name          = $Name
        GameModule    = $Descriptor.GameModule
        EngineVersion = $Descriptor.EngineVersion
        Info          = [pscustomobject]$Info
        Packaging     = [pscustomobject]$Packaging
    }
}
