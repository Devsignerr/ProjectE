<#
.SYNOPSIS
    데모 맵용 외부 에셋(Poly Haven, CC0)을 받는다. 원본 파일은 저장소에 넣지 않고(gitignore) 이 스크립트 + 잠금 파일로 재현한다.
.DESCRIPTION
    잠금 파일 Scripts/DemoAssets.json = 에셋 목록(Type models|textures, Id, Res) + 파일별 URL/MD5.
    기본 실행: 잠금 파일의 파일 중 없거나 MD5가 다른 것만 받는다 (이미 받은 파일은 건너뜀).
    -Update: 목록의 에셋을 Poly Haven API로 다시 풀어 파일 목록/MD5를 잠금 파일에 쓴 뒤 받는다 (에셋을 추가했을 때).
    저장 위치: <Root>/<Id>/ (glTF는 API가 주는 상대 경로 그대로 — textures/...). 임포트 설정(.eimport)은 커밋 대상이다.
    AlphaMaps(모델, 선택): 잎 카드처럼 알파가 별도 맵(<접두사>_alpha)인 에셋은 그 PNG도 받아 색(JPG)과 합친
    textures/<Id>_<접두사>_diffalpha_<해상도>.png를 만든다 (JPG 색에는 알파가 없음 — 생성 스크립트의 나눈 glTF가 이 파일을 쓴다).
.EXAMPLE
    .\Scripts\FetchDemoAssets.ps1
    .\Scripts\FetchDemoAssets.ps1 -Update
#>
param(
    [switch]$Update
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest 진행 표시가 다운로드를 크게 느리게 한다
$RootDir  = Resolve-Path (Join-Path $PSScriptRoot "..")
$LockPath = Join-Path $PSScriptRoot "DemoAssets.json"
$Lock     = Get-Content -Raw -Encoding UTF8 $LockPath | ConvertFrom-Json
$AssetRoot = Join-Path $RootDir $Lock.Root

# 텍스처 에셋에서 받을 맵 (API 키 → 형식). nor_gl = OpenGL(+Y) 노멀 — 엔진 규약(노멀 맵 +Y = UV 위쪽)과 같다
$TextureMaps = @("Diffuse", "nor_gl", "Rough", "arm")

function Resolve-Asset($Asset)
{
    $Files = Invoke-RestMethod -Uri "https://api.polyhaven.com/files/$($Asset.Id)" -UserAgent "ProjectE-FetchDemoAssets"
    $Result = @()
    if ($Asset.Type -eq "models")
    {
        $Gltf = $Files.gltf.($Asset.Res).gltf
        if (-not $Gltf) { throw "$($Asset.Id): glTF $($Asset.Res) 없음" }
        $Result += [pscustomobject]@{ Path = "$($Asset.Id).gltf"; Url = $Gltf.url; Md5 = $Gltf.md5 }
        foreach ($Include in $Gltf.include.PSObject.Properties)
        {
            $Result += [pscustomobject]@{ Path = $Include.Name; Url = $Include.Value.url; Md5 = $Include.Value.md5 }
        }
        foreach ($Prefix in @($Asset.AlphaMaps))
        {
            if (-not $Prefix) { continue }
            $Alpha = $Files."$($Prefix)_alpha".($Asset.Res).png
            if (-not $Alpha) { throw "$($Asset.Id): 알파 맵 $($Prefix)_alpha $($Asset.Res) PNG 없음" }
            $Result += [pscustomobject]@{ Path = "textures/" + [System.IO.Path]::GetFileName($Alpha.url); Url = $Alpha.url; Md5 = $Alpha.md5 }
        }
    }
    else
    {
        foreach ($Map in $TextureMaps)
        {
            $Entry = $Files.$Map.($Asset.Res).jpg
            if (-not $Entry) { continue }
            $Result += [pscustomobject]@{ Path = [System.IO.Path]::GetFileName($Entry.url); Url = $Entry.url; Md5 = $Entry.md5 }
        }
    }
    return $Result
}

if ($Update)
{
    foreach ($Asset in $Lock.Assets)
    {
        Write-Host "API: $($Asset.Type)/$($Asset.Id) ($($Asset.Res))"
        $Resolved = @(Resolve-Asset $Asset)
        $Asset | Add-Member -NotePropertyName Files -NotePropertyValue $Resolved -Force
    }
    $Json = $Lock | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($LockPath, $Json + "`n", (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "잠금 파일 갱신: $LockPath"
}

$Downloaded = 0
$Skipped    = 0
$TotalBytes = 0
foreach ($Asset in $Lock.Assets)
{
    if (-not $Asset.Files) { throw "$($Asset.Id): 잠금 파일에 파일 목록이 없음 — -Update로 다시 풀 것" }
    $Dir = Join-Path $AssetRoot $Asset.Id
    foreach ($File in $Asset.Files)
    {
        $Target = Join-Path $Dir $File.Path
        if ((Test-Path $Target) -and ((Get-FileHash -Algorithm MD5 $Target).Hash -eq $File.Md5.ToUpper()))
        {
            $Skipped++
            continue
        }
        New-Item -ItemType Directory -Force (Split-Path $Target) | Out-Null
        Write-Host "받기: $($Asset.Id)/$($File.Path)"
        Invoke-WebRequest -Uri $File.Url -OutFile $Target -UserAgent "ProjectE-FetchDemoAssets"
        $Hash = (Get-FileHash -Algorithm MD5 $Target).Hash
        if ($Hash -ne $File.Md5.ToUpper())
        {
            Remove-Item $Target
            throw "$($Asset.Id)/$($File.Path): MD5 불일치 (받음 $Hash, 기대 $($File.Md5))"
        }
        $TotalBytes += (Get-Item $Target).Length
        $Downloaded++
    }
}
# 알파 합치기: 색(JPG RGB) + 알파(PNG 회색조 R) → RGBA PNG. 결과가 입력보다 새것이면 건너뜀
$AlphaAssets = @($Lock.Assets | Where-Object { $_.AlphaMaps })
if ($AlphaAssets.Count -gt 0)
{
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class EDemoAlphaMerge
{
    static byte[] Read(Bitmap Image, int Width, int Height)
    {
        using (Bitmap Copy = new Bitmap(Width, Height, PixelFormat.Format32bppArgb))
        {
            using (Graphics G = Graphics.FromImage(Copy))
            {
                G.DrawImage(Image, new Rectangle(0, 0, Width, Height));
            }
            BitmapData Data = Copy.LockBits(new Rectangle(0, 0, Width, Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            byte[] Bytes = new byte[Data.Stride * Height];
            Marshal.Copy(Data.Scan0, Bytes, 0, Bytes.Length);
            Copy.UnlockBits(Data);
            return Bytes;
        }
    }
    public static void Merge(string ColorPath, string AlphaPath, string OutPath)
    {
        using (Bitmap Color = new Bitmap(ColorPath))
        using (Bitmap Alpha = new Bitmap(AlphaPath))
        {
            int Width = Color.Width, Height = Color.Height;
            byte[] C = Read(Color, Width, Height);
            byte[] A = Read(Alpha, Width, Height);
            for (int Index = 0; Index < C.Length; Index += 4)
            {
                C[Index + 3] = A[Index + 2]; // BGRA: 알파 맵의 R
            }
            using (Bitmap Out = new Bitmap(Width, Height, PixelFormat.Format32bppArgb))
            {
                BitmapData Data = Out.LockBits(new Rectangle(0, 0, Width, Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
                Marshal.Copy(C, 0, Data.Scan0, C.Length);
                Out.UnlockBits(Data);
                Out.Save(OutPath, ImageFormat.Png);
            }
        }
    }
}
"@
    foreach ($Asset in $AlphaAssets)
    {
        foreach ($Prefix in @($Asset.AlphaMaps))
        {
            $Dir      = Join-Path $AssetRoot "$($Asset.Id)/textures"
            $Color    = Join-Path $Dir "$($Asset.Id)_$($Prefix)_diff_$($Asset.Res).jpg"
            $AlphaMap = Join-Path $Dir "$($Asset.Id)_$($Prefix)_alpha_$($Asset.Res).png"
            $Out      = Join-Path $Dir "$($Asset.Id)_$($Prefix)_diffalpha_$($Asset.Res).png"
            if ((Test-Path $Out) -and (Get-Item $Out).LastWriteTime -ge (Get-Item $Color).LastWriteTime -and (Get-Item $Out).LastWriteTime -ge (Get-Item $AlphaMap).LastWriteTime)
            {
                continue
            }
            Write-Host "알파 합치기: $($Asset.Id)/$($Prefix)"
            [EDemoAlphaMerge]::Merge($Color, $AlphaMap, $Out)
        }
    }
}
Write-Host ("완료: 받음 {0}개 ({1:N1} MB), 건너뜀 {2}개 → {3}" -f $Downloaded, ($TotalBytes / 1MB), $Skipped, $AssetRoot)
