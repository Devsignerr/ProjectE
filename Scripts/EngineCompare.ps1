<#
.SYNOPSIS
    ProjectE ↔ Godot 성능 비교: 같은 벤치 씬(BenchSquare)을 같은 조건으로 번갈아 돌려 중앙값 표를 만든다.
.DESCRIPTION
    씬은 Tools/DemoMap/BuildBenchSquare.py가 두 엔진용으로 함께 만든다(같은 glTF 에셋·배치·라이트·카메라 궤도).
    공통 조건: 고정 dt 60Hz(ProjectE --fixed-delta 60 / Godot --fixed-fps 60 — 프레임마다 같은 화면), vsync 끔, 워밍업 200 + 측정 1200프레임
    (= 카메라 궤도 한 바퀴 20초), 해상도 100%. 켠 효과: 방향광 CSM(4장 2048, 60m, λ 0.75) + 로컬 라이트 그림자, SSAO, 블룸, ACES, TAA.
    끈 효과: ProjectE RT/SSR(--no-raytracing --no-ssr, DDGI는 볼륨 없음), Godot SSR/SSIL/SDFGI(씬 Environment). 두 엔진 모두 기본 LOD 켬.
    720p = 창 1280x720, 1440p = 주 모니터 테두리 없는 전체 화면(2560x1440).
    ProjectE는 숨긴 창(자동 검증), Godot은 창을 숨길 수 없어 보이는 창으로 돈다.
    엔진마다 [성능] 로그를 읽어 Saved/EngineCompare/<시각>/ 에 원본 로그와 summary.md / results.json을 쓴다.
.EXAMPLE
    .\Scripts\EngineCompare.ps1                       # 720p·1440p × LOD 기본/끔 × 3회
    .\Scripts\EngineCompare.ps1 -Repeats 1 -Resolutions 720 -SkipNoLod
#>
param(
    [int]$Repeats = 3,
    [string[]]$Resolutions = @("720", "1440"),
    [switch]$SkipNoLod,   # LOD 끔 회차 생략
    [int]$Warmup = 200,
    [int]$Frames = 1400,
    [string]$GodotExe = "E:\Godot\Godot_v4.7.2-stable_win64_console.exe",
    [string]$GodotProject = "E:\Godot\TestProject\test-project"
)

$ErrorActionPreference = "Stop"
$Resolutions = @($Resolutions | ForEach-Object { $_ -split "," } | Where-Object { $_ -ne "" })
$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $RootDir
$OutDir = Join-Path $RootDir ("Saved\EngineCompare\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Get-Number([string[]]$Text, [string]$Pattern)
{
    foreach ($Line in $Text)
    {
        $Match = [regex]::Match($Line, $Pattern)
        if ($Match.Success) { return [double]$Match.Groups[1].Value }
    }
    return [double]::NaN
}

function Invoke-ProjectE([string]$Res, [bool]$NoLod, [string]$Name)
{
    $Extra = "--scene Scenes/Bench/BenchSquare.escene --no-raytracing --no-ssr --fixed-delta 60 --no-vsync --perf-capture --perf-warmup $Warmup"
    if ($Res -eq "1440") { $Extra += " --window-mode BorderlessFullscreen" }
    if ($NoLod) { $Extra += " --no-lod" }
    powershell -ExecutionPolicy Bypass -File Scripts\Verify.ps1 -Target Runtime -Config Release -Frames $Frames -TimeoutSeconds 3000 -Name $Name -ExtraArgs $Extra | Out-Null
    $Log = Join-Path $RootDir "Saved\Verify\$Name.log"
    Copy-Item $Log (Join-Path $OutDir "$Name.log")
    $Text = Get-Content $Log -Encoding UTF8 | Where-Object { $_ -match "\[성능\]" }
    return [ordered]@{
        Frame = Get-Number $Text "프레임 평균 \([A-Za-z]+\): 프레임 ([0-9.]+) ms"
        P50 = Get-Number $Text "p50 ([0-9.]+)"; P95 = Get-Number $Text "p95 ([0-9.]+)"; P99 = Get-Number $Text "p99 ([0-9.]+)"
        GameThread = Get-Number $Text "게임 스레드 ([0-9.]+) ms \(BeginFrame"
        RenderCpu = Get-Number $Text "CPU ms: 전체 ([0-9.]+)"
        Gpu = Get-Number $Text "GPU ms: 전체 ([0-9.]+)"
        Draws = Get-Number $Text "드로우 ([0-9.]+) \(그림자"
        Triangles = Get-Number $Text "삼각형 ([0-9.]+)"
        Resolution = [regex]::Match(($Text -join "`n"), "씬 ([0-9]+x[0-9]+)").Groups[1].Value
    }
}

function Invoke-Godot([string]$Res, [bool]$NoLod, [string]$Name)
{
    $Log = Join-Path $OutDir "$Name.log"
    # 출력 리디렉션은 PS 5.1이 UTF-16 + 콘솔 코드 페이지로 바꿔 한글이 깨지므로 Godot --log-file(UTF-8)로 받는다
    $GodotArgs = @("--log-file", $Log, "--path", $GodotProject, "res://scenes/BenchSquare.tscn", "--fixed-fps", "60")
    if ($Res -eq "1440") { $GodotArgs += "--fullscreen" } else { $GodotArgs += @("--resolution", "1280x720") }
    $GodotArgs += @("--", "--perf-capture", "--perf-warmup", "$Warmup", "--frames", "$Frames")
    if ($NoLod) { $GodotArgs += "--no-lod" }
    & $GodotExe @GodotArgs *> $null
    $Text = Get-Content $Log -Encoding UTF8 | Where-Object { $_ -match "\[성능\]" }
    return [ordered]@{
        Frame = Get-Number $Text "프레임 ([0-9.]+)ms"
        P50 = Get-Number $Text "p50 ([0-9.]+)"; P95 = Get-Number $Text "p95 ([0-9.]+)"; P99 = Get-Number $Text "p99 ([0-9.]+)"
        GameThread = Get-Number $Text "게임 처리\(_process 합\) ([0-9.]+)ms"
        RenderCpu = Get-Number $Text "렌더 CPU ([0-9.]+)ms"
        Gpu = Get-Number $Text "GPU ([0-9.]+)ms"
        Draws = Get-Number $Text "드로우 ([0-9.]+),"
        Triangles = Get-Number $Text "프리미티브 ([0-9.]+)"
        Resolution = ([regex]::Match(($Text -join "`n"), "해상도 \(([0-9]+), ([0-9]+)\)").Groups | Select-Object -Skip 1 | ForEach-Object { $_.Value }) -join "x"
    }
}

$Configs = @()
foreach ($Res in $Resolutions)
{
    $Configs += [pscustomobject]@{ Res = $Res; NoLod = $false }
    if (-not $SkipNoLod) { $Configs += [pscustomobject]@{ Res = $Res; NoLod = $true } }
}

$Runs = @()
for ($Repeat = 1; $Repeat -le $Repeats; ++$Repeat)
{
    foreach ($Config in $Configs)
    {
        $Tag = "{0}p{1}_r{2}" -f $Config.Res, ($(if ($Config.NoLod) { "_nolod" } else { "" })), $Repeat
        # 열 영향이 한쪽에만 쏠리지 않게 회차마다 순서를 바꾼다
        $Engines = if ($Repeat % 2 -eq 1) { @("ProjectE", "Godot") } else { @("Godot", "ProjectE") }
        foreach ($Engine in $Engines)
        {
            $Name = "cmp_{0}_{1}" -f $Engine.ToLower(), $Tag
            $Result = if ($Engine -eq "ProjectE") { Invoke-ProjectE $Config.Res $Config.NoLod $Name } else { Invoke-Godot $Config.Res $Config.NoLod $Name }
            $Runs += [pscustomobject]@{ Engine = $Engine; Res = $Config.Res; NoLod = $Config.NoLod; Repeat = $Repeat; Result = $Result }
            Write-Host ("{0,-8} {1,-14} 프레임 {2,7:N2}ms  p99 {3,7:N2}  GPU {4,6:N2}  ({5})" -f $Engine, $Tag, $Result.Frame, $Result.P99, $Result.Gpu, $Result.Resolution)
        }
    }
}
$Runs | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutDir "results.json") -Encoding UTF8

function Get-Median([double[]]$Values)
{
    $Sorted = @($Values | Where-Object { -not [double]::IsNaN($_) } | Sort-Object)
    if ($Sorted.Count -eq 0) { return [double]::NaN }
    return $Sorted[[int][math]::Floor(($Sorted.Count - 1) / 2)]
}

$Keys = @(
    @("Frame", "프레임 평균 (ms)"), @("P50", "p50 (ms)"), @("P95", "p95 (ms)"), @("P99", "p99 (ms)"),
    @("GameThread", "게임 스레드/처리 (ms)"), @("RenderCpu", "렌더 CPU (ms)"), @("Gpu", "GPU (ms)"), @("Draws", "드로우"), @("Triangles", "삼각형")
)
$Md = @("# ProjectE vs Godot — BenchSquare", "", "회차 $Repeats 중앙값. 측정 $($Frames - $Warmup)프레임(워밍업 $Warmup), 고정 dt 60Hz, vsync 끔.", "")
foreach ($Config in $Configs)
{
    $Title = "{0}p, LOD {1}" -f $Config.Res, ($(if ($Config.NoLod) { "끔" } else { "기본" }))
    $Md += "## $Title"
    $Md += ""
    $Md += "| 항목 | ProjectE | Godot 4.7.2 | Godot / ProjectE |"
    $Md += "|---|---:|---:|---:|"
    foreach ($Key in $Keys)
    {
        $A = Get-Median @($Runs | Where-Object { $_.Engine -eq "ProjectE" -and $_.Res -eq $Config.Res -and $_.NoLod -eq $Config.NoLod } | ForEach-Object { $_.Result[$Key[0]] })
        $B = Get-Median @($Runs | Where-Object { $_.Engine -eq "Godot" -and $_.Res -eq $Config.Res -and $_.NoLod -eq $Config.NoLod } | ForEach-Object { $_.Result[$Key[0]] })
        $Ratio = if ($A -gt 0) { "{0:N2}x" -f ($B / $A) } else { "-" }
        $Fmt = if ($Key[0] -in @("Draws", "Triangles")) { "{0:N0}" } else { "{0:N2}" }
        $Md += ("| {0} | {1} | {2} | {3} |" -f $Key[1], ($Fmt -f $A), ($Fmt -f $B), $Ratio)
    }
    $Md += ""
}
$Md | Set-Content (Join-Path $OutDir "summary.md") -Encoding UTF8
$Md | ForEach-Object { Write-Host $_ }
Write-Host "결과: $OutDir"
