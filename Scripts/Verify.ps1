<#
.SYNOPSIS
    자동 화면 검증: 에디터(또는 런타임/Sandbox)를 실행해 스크린샷과 로그를 남기고, D3D12 디버그 레이어 오류/경고를 요약한다.
.EXAMPLE
    .\Scripts\Verify.ps1                                   # 에디터, 150 프레임, Saved/Verify/editor.png
    .\Scripts\Verify.ps1 -Target Runtime -Name runtime
    .\Scripts\Verify.ps1 -ExtraArgs "--select DamagedHelmet" -Name outline
    .\Scripts\Verify.ps1 -Multiplayer                      # 전용 서버 + 런타임 클라이언트 2개 (Tests/Multiplayer)
    .\Scripts\Verify.ps1 -Multiplayer -Clients 3 -ExtraArgs "--net-lag 80 --net-loss 5" -Name mp_lossy
.NOTES
    -Multiplayer: 서버 로그와 클라이언트마다 스크린샷/로그(<Name>_client<N>.png/.log). 클라이언트가 서버에 입장하지 못해도 오류(1)
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
    [int]$TimeoutSeconds = 120,
    [switch]$Multiplayer,                               # 전용 서버 + 런타임 클라이언트 여러 개
    [int]$Clients = 2,
    [string]$Scene = "Scenes/Tests/Multiplayer.escene",
    [int]$Port = 27800
)

$ErrorActionPreference = "Stop"
$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")

# 로그들의 Error/Warning/Fatal을 종류별로 묶어 출력하고 Error/Fatal 수를 돌려준다 (주소, 서브리소스 번호, 밉 번호 제거)
function Show-LogIssues([string[]]$LogPaths) {
    $Issues = Select-String -Path $LogPaths -Pattern ": (Error|Warning|Fatal): " | ForEach-Object {
        $Line = $_.Line -replace '^\[[^\]]*\] ', ''
        $Line = $Line -replace '0x[0-9A-Fa-f]{8,16}', ''
        $Line = $Line -replace '\(subresource: \d+\)', ''
        $Line -replace '_\d+(?=[''"])', '_N'
    }
    $Groups = $Issues | Group-Object | Sort-Object Count -Descending
    foreach ($Group in ($Groups | Select-Object -First 15)) {
        $Text = $Group.Name
        if ($Text.Length -gt 240) { $Text = $Text.Substring(0, 240) + "..." }
        $Color = "Yellow"
        if ($Group.Name -match ": (Error|Fatal): ") { $Color = "Red" }
        Write-Host ("{0,5}x {1}" -f $Group.Count, $Text) -ForegroundColor $Color
    }
    return ($Issues | Where-Object { $_ -match ": (Error|Fatal): " }).Count
}

if ($Multiplayer) {
    $BinDir = Join-Path $RootDir "Build\ninja-$($Config.ToLower())\Bin"
    $ServerExe  = Join-Path $BinDir "ProjectEServer.exe"
    $RuntimeExe = Join-Path $BinDir "ProjectERuntime.exe"
    foreach ($Required in @($ServerExe, $RuntimeExe)) {
        if (-not (Test-Path $Required)) { Write-Host "실행 파일 없음: $Required (먼저 Build.ps1)" -ForegroundColor Red; exit 2 }
    }
    if ($Name -eq "") { $Name = "multiplayer" }
    $OutDir = Join-Path $RootDir "Saved\Verify"
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $ServerLog = Join-Path $OutDir "$($Name)_server.log"
    Remove-Item (Join-Path $OutDir "$($Name)_*") -ErrorAction SilentlyContinue

    # 서버는 클라이언트보다 오래 돈다 (60Hz 틱: 클라이언트 프레임 + 여유 8초)
    $ServerArgs = "--scene `"$Scene`" --port $Port --exit-after $($Frames + 480) --log `"$ServerLog`" $ExtraArgs"
    $Server = Start-Process -FilePath $ServerExe -ArgumentList $ServerArgs -PassThru -WorkingDirectory $RootDir -WindowStyle Hidden
    Start-Sleep -Milliseconds 1000

    $ClientProcesses = @()
    $ClientLogs      = @()
    for ($Index = 1; $Index -le $Clients; $Index++) {
        $ClientShot = Join-Path $OutDir "$($Name)_client$Index.png"
        $ClientLog  = Join-Path $OutDir "$($Name)_client$Index.log"
        $ClientLogs += $ClientLog
        $ClientArgs = "--exit-after $Frames --screenshot `"$ClientShot`" --log `"$ClientLog`" --scene `"$Scene`" --connect 127.0.0.1:$Port $ExtraArgs"
        $ClientProcesses += Start-Process -FilePath $RuntimeExe -ArgumentList $ClientArgs -PassThru -WorkingDirectory $RootDir
        Start-Sleep -Milliseconds 300
    }

    $Failed = $false
    foreach ($Client in $ClientProcesses) {
        if (-not $Client.WaitForExit($TimeoutSeconds * 1000)) { $Client.Kill(); $Failed = $true; Write-Host "클라이언트 시간 초과" -ForegroundColor Red }
        elseif ($Client.ExitCode -ne 0) { $Failed = $true; Write-Host "클라이언트 비정상 종료 ($($Client.ExitCode))" -ForegroundColor Red }
    }
    if (-not $Server.HasExited) { $Server.Kill(); $Server.WaitForExit() }
    elseif ($Server.ExitCode -ne 0) { $Failed = $true; Write-Host "서버 비정상 종료 ($($Server.ExitCode))" -ForegroundColor Red }

    Write-Host "서버 로그: $ServerLog"
    $JoinFailures = 0
    for ($Index = 1; $Index -le $Clients; $Index++) {
        $ClientLog = $ClientLogs[$Index - 1]
        $Joined    = (Test-Path $ClientLog) -and (Select-String -Path $ClientLog -Pattern "서버 입장" -Quiet)
        $Color     = if ($Joined) { "Green" } else { "Red" }
        Write-Host ("클라이언트 {0}: {1}  ({2})" -f $Index, $(if ($Joined) { "입장" } else { "입장 실패" }), (Join-Path $OutDir "$($Name)_client$Index.png")) -ForegroundColor $Color
        if (-not $Joined) { $JoinFailures++ }
    }
    $ExistingLogs = @($ServerLog) + $ClientLogs | Where-Object { Test-Path $_ }
    $ErrorCount   = Show-LogIssues $ExistingLogs
    if ($Failed) { exit 2 }
    if ($ErrorCount -gt 0 -or $JoinFailures -gt 0) { Write-Host "오류 $ErrorCount 건, 입장 실패 $JoinFailures" -ForegroundColor Red; exit 1 }
    Write-Host "오류 없음" -ForegroundColor Green
    exit 0
}
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

$ErrorCount = Show-LogIssues @($Log)
if ($Process.ExitCode -ne 0) { Write-Host "비정상 종료" -ForegroundColor Red; exit 2 }
if ($ErrorCount -gt 0) { Write-Host "오류 $ErrorCount 건" -ForegroundColor Red; exit 1 }
Write-Host "오류 없음" -ForegroundColor Green
exit 0
