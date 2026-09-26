<#
.SYNOPSIS
    Compares scenario captures with the reference archive, offline.

.DESCRIPTION
    Each PNG under scenarios/out/<name>/ is compared with the file of the same
    name under scenarios/reference/<name>/. Files with identical bytes pass
    without being decoded. Any other pair is compared pixel by pixel: the
    report gives how many pixels differ, the largest channel difference and
    the box they fall in, and writes a diff image to out/<name>/diff/, black
    where the images agree and brighter where they differ more.

    Exits 1 if any capture differs from its reference, so it can gate a
    commit. A capture with no reference is reported as new and does not fail.

.EXAMPLE
    scenarios\compare.ps1                       # every scenario with captures
    scenarios\compare.ps1 pole                  # one scenario
    scenarios\compare.ps1 pole -Accept          # its captures become the reference
    scenarios\compare.ps1 -A x.png -B y.png     # two files, no archive
#>
param(
    [string[]] $Name,
    [switch]   $Accept,
    [string]   $A,
    [string]   $B
)

$ErrorActionPreference = "Stop"

$root      = $PSScriptRoot
$outDir    = Join-Path $root "out"
$refDir    = Join-Path $root "reference"

# PowerShell decodes and encodes; the C# only walks bytes. Compiled C# that
# names System.Drawing types fails on current .NET, which splits them across
# assemblies Add-Type does not resolve.
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;

public sealed class PixelDiff
{
    public int Width, Height, Differing, MaxDelta;
    public int MinX = int.MaxValue, MinY = int.MaxValue, MaxX = -1, MaxY = -1;
    public byte[] Image;   // BGRA, black where the two agree

    // l and r are 32-bit BGRA rows with no padding.
    public static PixelDiff Compare(byte[] l, byte[] r, int width, int height)
    {
        var result = new PixelDiff { Width = width, Height = height, Image = new byte[l.Length] };
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                int at = (y * width + x) * 4;
                int delta = 0;
                for (int c = 0; c < 3; ++c)
                {
                    delta = Math.Max(delta, Math.Abs(l[at + c] - r[at + c]));
                }
                result.Image[at + 3] = 255;
                if (delta == 0)
                {
                    continue;
                }

                ++result.Differing;
                result.MaxDelta = Math.Max(result.MaxDelta, delta);
                result.MinX = Math.Min(result.MinX, x);
                result.MinY = Math.Min(result.MinY, y);
                result.MaxX = Math.Max(result.MaxX, x);
                result.MaxY = Math.Max(result.MaxY, y);

                // Small differences are the ones worth seeing: scaled so a
                // difference of 1 is already visible.
                byte shown = (byte)Math.Min(255, 64 + delta * 8);
                result.Image[at] = result.Image[at + 1] = result.Image[at + 2] = shown;
            }
        }
        return result;
    }
}
"@

$argb = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb

# A PNG as a bitmap's size and its BGRA bytes.
function Read-Pixels([string] $Path)
{
    $bitmap = [System.Drawing.Bitmap]::new($Path)
    try
    {
        $rect  = [System.Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height)
        $data  = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $argb)
        $bytes = [byte[]]::new($bitmap.Width * $bitmap.Height * 4)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        $bitmap.UnlockBits($data)
        return [pscustomobject]@{ Width = $bitmap.Width; Height = $bitmap.Height; Bytes = $bytes }
    }
    finally
    {
        $bitmap.Dispose()
    }
}

function Write-Pixels([string] $Path, [byte[]] $Bytes, [int] $Width, [int] $Height)
{
    $bitmap = [System.Drawing.Bitmap]::new($Width, $Height, $argb)
    try
    {
        $rect = [System.Drawing.Rectangle]::new(0, 0, $Width, $Height)
        $data = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $argb)
        [System.Runtime.InteropServices.Marshal]::Copy($Bytes, 0, $data.Scan0, $Bytes.Length)
        $bitmap.UnlockBits($data)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally
    {
        $bitmap.Dispose()
    }
}

function Test-SameBytes([string] $Left, [string] $Right)
{
    return (Get-FileHash $Left).Hash -eq (Get-FileHash $Right).Hash
}

# Compares one pair and prints a line; returns whether they match.
function Compare-Pair([string] $Label, [string] $Left, [string] $Right, [string] $DiffPath)
{
    if (Test-SameBytes $Left $Right)
    {
        Write-Host "  same      $Label" -ForegroundColor Green
        return $true
    }

    $l = Read-Pixels $Left
    $r = Read-Pixels $Right
    if ($l.Width -ne $r.Width -or $l.Height -ne $r.Height)
    {
        Write-Host "  DIFFERS   $Label -- size $($l.Width)x$($l.Height) against $($r.Width)x$($r.Height)" -ForegroundColor Red
        return $false
    }

    $result = [PixelDiff]::Compare($l.Bytes, $r.Bytes, $l.Width, $l.Height)
    if ($result.Differing -eq 0)
    {
        # Different bytes, same pixels: the PNG encoding differs.
        Write-Host "  same      $Label (pixels; the files differ)" -ForegroundColor Green
        return $true
    }

    $total = $result.Width * $result.Height
    $share = 100.0 * $result.Differing / $total
    $box   = "($($result.MinX),$($result.MinY))-($($result.MaxX),$($result.MaxY))"
    Write-Host ("  DIFFERS   {0}: {1} pixels ({2:0.###}%), max delta {3}, in {4}" -f
                $Label, $result.Differing, $share, $result.MaxDelta, $box) -ForegroundColor Red
    if ($DiffPath)
    {
        Write-Pixels $DiffPath $result.Image $result.Width $result.Height
        Write-Host "            diff: $DiffPath"
    }
    return $false
}

if ($A -or $B)
{
    if (-not ($A -and $B))
    {
        throw "-A and -B go together."
    }
    $same = Compare-Pair "$A vs $B" (Resolve-Path $A) (Resolve-Path $B) $null
    exit ($same ? 0 : 1)
}

if (-not (Test-Path $outDir))
{
    throw "No captures: $outDir does not exist. Run a scenario first."
}

$scenarios = if ($Name) { $Name } else { Get-ChildItem $outDir -Directory | ForEach-Object Name }

if ($Accept)
{
    foreach ($scenario in $scenarios)
    {
        $from = Join-Path $outDir $scenario
        $to   = Join-Path $refDir $scenario
        New-Item -ItemType Directory -Force $to | Out-Null
        foreach ($file in Get-ChildItem $from -Filter *.png -File)
        {
            Copy-Item $file.FullName (Join-Path $to $file.Name) -Force
            Write-Host "  accepted  $scenario/$($file.Name)"
        }
    }
    exit 0
}

$failed = $false
foreach ($scenario in $scenarios)
{
    $captures = Join-Path $outDir $scenario
    if (-not (Test-Path $captures))
    {
        Write-Host "$scenario`: no captures" -ForegroundColor Yellow
        $failed = $true
        continue
    }

    Write-Host $scenario
    $diffDir = Join-Path $captures "diff"
    Remove-Item $diffDir -Recurse -Force -ErrorAction SilentlyContinue

    foreach ($file in Get-ChildItem $captures -Filter *.png -File)
    {
        $reference = Join-Path (Join-Path $refDir $scenario) $file.Name
        if (-not (Test-Path $reference))
        {
            Write-Host "  new       $($file.Name) (no reference; -Accept to keep it)" -ForegroundColor Yellow
            continue
        }

        New-Item -ItemType Directory -Force $diffDir | Out-Null
        if (-not (Compare-Pair $file.Name $file.FullName $reference (Join-Path $diffDir $file.Name)))
        {
            $failed = $true
        }
    }
}

exit ($failed ? 1 : 0)
