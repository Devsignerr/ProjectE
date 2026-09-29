<#
.SYNOPSIS
    자동 화면 검증: 에디터(또는 런타임/Sandbox)를 실행해 스크린샷과 로그를 남기고, D3D12 디버그 레이어 오류/경고를 요약한다.
.EXAMPLE
    .\Scripts\Verify.ps1                                   # 에디터, 150 프레임, Saved/Verify/editor.png
    .\Scripts\Verify.ps1 -Target Runtime -Name runtime
    .\Scripts\Verify.ps1 -ExtraArgs "--select DamagedHelmet" -Name outline
.NOTES
    종료 코드: 0 = 오류 없음, 1 = 디버그 레이어/엔진 Error 또는 Fatal 발견, 2 = 실행 실패/시간 초과
#>
param(
    [ValidateSet("Editor", "Runtime", "Sandbox")]
    [string]$Target = "Editor",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [int]$Frames = 150,
    [string]$Name = "",
    [string]$ExtraArgs = "",
    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = "Stop"
$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")
$ExeName = @{ Editor = "ProjectEEditor.exe"; Runtime = "ProjectERuntime.exe"; Sandbox = "Sandbox.exe" }[$Target]
$Exe     = Join-Path $RootDir "Build\ninja-$($Config.ToLower())\Bin\$ExeName"
if (-not (Test-Path $Exe)) { Write-Host "실행 파일 없음: $Exe (먼저 Build.ps1)" -ForegroundColor Red; exit 2 }
if ($Name -eq "") { $Name = $Target.ToLower() }

$OutDir = Join-Path $RootDir "Saved\Verify"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Shot = Join-Path $OutDir "$Name.png"
$Log  = Join-Path $OutDir "$Name.log"
Remove-Item $Shot, $Log -ErrorAction SilentlyContinue

$Arguments = "--exit-after $Frames --screenshot `"$Shot`" --log `"$Log`" $ExtraArgs"
$Process = Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru -WorkingDirectory $RootDir
if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
    $Process.Kill()
    Write-Host "시간 초과 ($TimeoutSeconds 초): $ExeName" -ForegroundColor Red
    exit 2
}

Write-Host "종료 코드: $($Process.ExitCode)"
if (Test-Path $Shot) { Write-Host "스크린샷: $Shot" -ForegroundColor Green } else { Write-Host "스크린샷 없음" -ForegroundColor Red }
if (-not (Test-Path $Log)) { Write-Host "로그 없음" -ForegroundColor Red; exit 2 }
Write-Host "로그: $Log"

# 오류/경고를 종류별로 묶어 요약 (주소, 서브리소스 번호, 밉 번호 제거)
$Issues = Select-String -Path $Log -Pattern ": (Error|Warning|Fatal): " | ForEach-Object {
    $Line = $_.Line -replace '^\[[^\]]*\] ', ''
    $Line = $Line -replace '0x[0-9A-Fa-f]{8,16}', ''
    $Line = $Line -replace '\(subresource: \d+\)', ''
    $Line -replace '_\d+(?=[''"])', '_N'
}
$Groups     = $Issues | Group-Object | Sort-Object Count -Descending
$ErrorCount = ($Issues | Where-Object { $_ -match ": (Error|Fatal): " }).Count
foreach ($Group in ($Groups | Select-Object -First 15)) {
    $Text = $Group.Name
    if ($Text.Length -gt 240) { $Text = $Text.Substring(0, 240) + "..." }
    $Color = "Yellow"
    if ($Group.Name -match ": (Error|Fatal): ") { $Color = "Red" }
    Write-Host ("{0,5}x {1}" -f $Group.Count, $Text) -ForegroundColor $Color
}
if ($Process.ExitCode -ne 0) { Write-Host "비정상 종료" -ForegroundColor Red; exit 2 }
if ($ErrorCount -gt 0) { Write-Host "오류 $ErrorCount 건" -ForegroundColor Red; exit 1 }
Write-Host "오류 없음" -ForegroundColor Green
exit 0
