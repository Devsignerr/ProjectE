<#
.SYNOPSIS
    규모 추세 측정: Tests/Stress를 유닛 수별로 만들어 성능을 재고, 구간별 시간이 개수보다 빠르게 늘어나는지(비선형) 검사한다.
.DESCRIPTION
    유닛 수마다 Tools/DemoMap/BuildStress.py --count N --out Scenes/Tests/Scale/Stress_N.escene (gitignore)로 씬을 만들고
    런타임 Release를 --perf-capture로 돌려 [성능] 로그(렌더러 CPU/GPU 구간, 게임 틱 구간)를 모은다.
    이웃한 두 규모 사이에서 구간 시간 배율이 개수 배율 × Tolerance를 넘고(그리고 큰 쪽이 MinMs 이상이면) "비선형"으로 표시한다.
    종료 코드: 0 = 비선형 없음, 1 = 비선형 구간 있음, 2 = 실행 실패
    측정 중에는 다른 무거운 작업(빌드·다른 Verify)을 돌리지 않는다 (수치가 흔들린다).
.EXAMPLE
    .\Scripts\ScaleBench.ps1                          # 500, 1000, 2000 (RT 끔)
    .\Scripts\ScaleBench.ps1 -Counts 250,500,1000,2000 -RayTracing
#>
param(
    [int[]]$Counts = @(500, 1000, 2000),
    [switch]$RayTracing,
    [double]$Tolerance = 1.25,
    [double]$MinMs = 0.3,
    [int]$Frames = 600
)

$ErrorActionPreference = "Stop"
$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $RootDir

function Parse-Sections([string]$Line, [string]$Prefix)
{
    $Result = [ordered]@{}
    foreach ($Match in [regex]::Matches($Line, "([^,:(]+?) ([0-9]+\.[0-9]+)"))
    {
        $Name = $Match.Groups[1].Value.Trim()
        if ($Name -match "ms$") { $Name = $Name -replace " ?ms$", "" }
        $Result["$Prefix$Name"] = [double]$Match.Groups[2].Value
    }
    return $Result
}

$Results = [ordered]@{}
foreach ($Count in $Counts)
{
    $Scene = "Scenes/Tests/Scale/Stress_$Count.escene"
    python Tools/DemoMap/BuildStress.py --count $Count --out $Scene | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "씬 생성 실패: $Count"; exit 2 }
    $Name = "scale_$Count" + ($(if ($RayTracing) { "_rt" } else { "" }))
    $Extra = "--scene $Scene --perf-capture --perf-warmup 200 --no-vsync" + ($(if ($RayTracing) { "" } else { " --no-raytracing" }))
    powershell -ExecutionPolicy Bypass -File Scripts\Verify.ps1 -Target Runtime -Config Release -Frames $Frames -TimeoutSeconds 3600 -Name $Name -ExtraArgs $Extra | Out-Null
    $Log = Join-Path $RootDir "Saved\Verify\$Name.log"
    if (-not (Test-Path $Log)) { Write-Host "로그 없음: $Log"; exit 2 }
    $Text = Get-Content $Log -Encoding UTF8
    $Sections = [ordered]@{}
    $Frame = $Text | Select-String "\[성능\] [0-9]+ 프레임 평균" | Select-Object -First 1
    if (-not $Frame) { Write-Host "성능 로그 없음: $Log"; exit 2 }
    $Sections["프레임"] = [double][regex]::Match($Frame.Line, "프레임 ([0-9.]+) ms").Groups[1].Value
    $Cpu = $Text | Select-String "\[성능\] CPU ms:" | Select-Object -First 1
    $Gpu = $Text | Select-String "\[성능\] GPU ms:" | Select-Object -First 1
    $Tick = $Text | Select-String "\[성능\] 게임 틱" | Select-Object -First 1
    if ($Cpu)  { (Parse-Sections ($Cpu.Line -replace "^.*CPU ms:", "") "CPU ").GetEnumerator() | ForEach-Object { $Sections[$_.Key] = $_.Value } }
    if ($Gpu)  { (Parse-Sections ($Gpu.Line -replace "^.*GPU ms:", "") "GPU ").GetEnumerator() | ForEach-Object { $Sections[$_.Key] = $_.Value } }
    if ($Tick) { (Parse-Sections ($Tick.Line -replace "^.*CPU ms:", "") "틱 ").GetEnumerator() | ForEach-Object { $Sections[$_.Key] = $_.Value } }
    $Results["$Count"] = $Sections
    Write-Host ("측정 {0}: 프레임 {1:N2}ms" -f $Count, $Sections["프레임"])
}

# 표 + 비선형 판정 (OrderedDictionary는 정수 키를 인덱스로 보므로 문자열 키) (이웃한 규모 쌍)
$Keys = $Results["$($Counts[-1])"].Keys | Where-Object { $Results["$($Counts[-1])"][$_] -ge $MinMs -or $_ -eq "프레임" }
$Header = "{0,-22}" -f "구간"
foreach ($Count in $Counts) { $Header += "{0,10}" -f $Count }
$Header += "   판정"
Write-Host ""
Write-Host $Header
$Bad = 0
foreach ($Key in $Keys)
{
    $Row = "{0,-22}" -f $Key
    foreach ($Count in $Counts) { $Row += "{0,10:N3}" -f [double]$Results["$Count"][$Key] }
    $Verdict = ""
    for ($Index = 1; $Index -lt $Counts.Count; ++$Index)
    {
        $A = [double]$Results["$($Counts[$Index - 1])"][$Key]
        $B = [double]$Results["$($Counts[$Index])"][$Key]
        $Scale = $Counts[$Index] / $Counts[$Index - 1]
        if ($A -gt 0.05 -and $B -ge $MinMs -and ($B / $A) -gt $Scale * $Tolerance -and $Key -notmatch "^(프레임|CPU 전체|GPU 전체|틱 합)")
        {
            $Verdict += (" 비선형({0}→{1}: ×{2:N2})" -f $Counts[$Index - 1], $Counts[$Index], ($B / $A))
            $Bad++
        }
    }
    Write-Host ($Row + "  " + $Verdict)
}
Write-Host ""
if ($Bad -gt 0) { Write-Host "비선형 구간 $Bad 개 (개수 배율 × $Tolerance 초과)"; exit 1 }
Write-Host "비선형 구간 없음"
exit 0
