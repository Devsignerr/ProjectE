<#
.SYNOPSIS
    Crypt2D 예제용 외부 에셋(ansimuz GothicVania Church/Cemetery, CC0)을 받는다. 원본은 저장소에 넣지 않고(gitignore)
    이 스크립트 + 잠금 파일(Assets.json)로 재현한다.
.DESCRIPTION
    잠금 파일 Projects/Crypt2D/Scripts/Assets.json = 팩 목록(Name, Url, Md5, Files = 압축 안 경로 → 받을 경로).
    기본 실행: 팩 zip을 <Root>/_Downloads/에 받아(이미 있고 MD5가 같으면 건너뜀) 필요한 파일만 <Root>/<Name>/<To>로 푼다.
      풀린 파일이 이미 같은 크기로 있으면 건너뛴다.
    -Update: zip을 다시 받아 MD5를 잠금 파일에 쓴다 (팩이 갱신되었을 때 — 그 뒤 생성 스크립트를 다시 돌려 확인할 것).
    ColorKeys(팩, 선택): 풀린 PNG에서 정확히 그 색인 픽셀을 투명으로 바꾼 사본을 만든다 (교회 장식 판의 단색 바탕 — 원본처럼 gitignore).
    생성 스크립트(Projects/Crypt2D/Tools/BuildCrypt2D.py)는 풀린 PNG의 크기·칸을 읽어 .esprite/.eflipbook/.etileset을 쓴다.
.EXAMPLE
    .\Projects\Crypt2D\Scripts\FetchAssets.ps1
    .\Projects\Crypt2D\Scripts\FetchAssets.ps1 -Update
#>
param(
    [switch]$Update
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest 진행 표시가 다운로드를 크게 느리게 한다
Add-Type -AssemblyName System.IO.Compression.FileSystem

$RootDir   = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$LockPath  = Join-Path $PSScriptRoot "Assets.json"
$Lock      = Get-Content -Raw -Encoding UTF8 $LockPath | ConvertFrom-Json
$AssetRoot = Join-Path $RootDir $Lock.Root
$CacheDir  = Join-Path $AssetRoot "_Downloads"
New-Item -ItemType Directory -Force $CacheDir | Out-Null

function Get-Md5([string]$Path)
{
    return (Get-FileHash -Algorithm MD5 $Path).Hash.ToLower()
}

$Downloaded = 0
$Extracted  = 0
$Skipped    = 0
foreach ($Pack in $Lock.Packs)
{
    $Zip = Join-Path $CacheDir "$($Pack.Name).zip"
    $bHave = (Test-Path $Zip) -and ((Get-Md5 $Zip) -eq $Pack.Md5.ToLower())
    if ($Update -or -not $bHave)
    {
        Write-Host "받기: $($Pack.Name) ← $($Pack.Url)"
        Invoke-WebRequest -Uri $Pack.Url -OutFile $Zip -UserAgent "ProjectE-Crypt2D-FetchAssets"
        $Downloaded++
        $Hash = Get-Md5 $Zip
        if ($Update)
        {
            $Pack.Md5 = $Hash
        }
        elseif ($Hash -ne $Pack.Md5.ToLower())
        {
            Remove-Item $Zip
            throw "$($Pack.Name): MD5 불일치 (받음 $Hash, 기대 $($Pack.Md5)) — 팩이 바뀌었으면 -Update 후 생성 스크립트로 확인"
        }
    }

    $Archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try
    {
        foreach ($File in $Pack.Files)
        {
            $Entry = $Archive.Entries | Where-Object { $_.FullName -eq $File.From } | Select-Object -First 1
            if (-not $Entry) { throw "$($Pack.Name): 압축 안에 '$($File.From)'이 없음" }
            $Target = Join-Path (Join-Path $AssetRoot $Pack.Name) $File.To
            if ((Test-Path $Target) -and (Get-Item $Target).Length -eq $Entry.Length)
            {
                $Skipped++
                continue
            }
            New-Item -ItemType Directory -Force (Split-Path $Target) | Out-Null
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, $Target, $true)
            $Extracted++
        }
    }
    finally
    {
        $Archive.Dispose()
    }
}

# 색 키: 단색 바탕 → 투명 (결과가 입력보다 새것이면 건너뜀)
$KeyPacks = @($Lock.Packs | Where-Object { $_.ColorKeys })
if ($KeyPacks.Count -gt 0)
{
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class ECrypt2DColorKey
{
    public static int Apply(string InPath, string OutPath, int R, int G, int B)
    {
        using (Bitmap Source = new Bitmap(InPath))
        using (Bitmap Copy = new Bitmap(Source.Width, Source.Height, PixelFormat.Format32bppArgb))
        {
            using (Graphics Gfx = Graphics.FromImage(Copy))
            {
                Gfx.DrawImage(Source, new Rectangle(0, 0, Source.Width, Source.Height));
            }
            BitmapData Data = Copy.LockBits(new Rectangle(0, 0, Copy.Width, Copy.Height), ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
            byte[] Bytes = new byte[Data.Stride * Copy.Height];
            Marshal.Copy(Data.Scan0, Bytes, 0, Bytes.Length);
            int Keyed = 0;
            for (int Index = 0; Index < Bytes.Length; Index += 4)
            {
                if (Bytes[Index + 3] != 0 && Bytes[Index] == B && Bytes[Index + 1] == G && Bytes[Index + 2] == R)
                {
                    Bytes[Index] = 0; Bytes[Index + 1] = 0; Bytes[Index + 2] = 0; Bytes[Index + 3] = 0;
                    Keyed++;
                }
            }
            Marshal.Copy(Bytes, 0, Data.Scan0, Bytes.Length);
            Copy.UnlockBits(Data);
            Copy.Save(OutPath, ImageFormat.Png);
            return Keyed;
        }
    }
}
"@
    foreach ($Pack in $KeyPacks)
    {
        foreach ($Key in $Pack.ColorKeys)
        {
            $In  = Join-Path (Join-Path $AssetRoot $Pack.Name) $Key.From
            $Out = Join-Path (Join-Path $AssetRoot $Pack.Name) $Key.To
            if ((Test-Path $Out) -and (Get-Item $Out).LastWriteTime -ge (Get-Item $In).LastWriteTime) { continue }
            $Count = [ECrypt2DColorKey]::Apply($In, $Out, [int]$Key.Color[0], [int]$Key.Color[1], [int]$Key.Color[2])
            Write-Host "색 키: $($Pack.Name)/$($Key.To) (픽셀 $Count 개 투명)"
        }
    }
}

if ($Update)
{
    $Json = $Lock | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($LockPath, $Json + "`n", (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "잠금 파일 갱신: $LockPath"
}
Write-Host ("완료: 받음 {0}개, 풂 {1}개, 건너뜀 {2}개 → {3}" -f $Downloaded, $Extracted, $Skipped, $AssetRoot)
