<#
.SYNOPSIS
    Runs scenarios in one process, then compares their captures.

.DESCRIPTION
    Every scenario named, or every one in scenarios/ that does not end by
    handing over to the user, runs in a single encke process: startup and the
    first settle, which build the octree and load the assets, are paid once.
    Between files the engine resets what a scenario changes (see
    App::reset_for_next_scenario). Then compare.ps1 checks each one's
    captures against the reference.

    Files whose startup settings (srgb_ui, collision_lod) differ cannot share
    a process; the engine refuses the run and says which. -Separate runs each
    file in a process of its own, as before.

    The log of each process goes to out/run.log (or out/<name>.log with
    -Separate). Exits 1 if a scenario failed or a capture differs.

.EXAMPLE
    scenarios\run.ps1                       # every non-interactive scenario
    scenarios\run.ps1 walk, torchship       # these, in this order
    scenarios\run.ps1 -Preset release       # another build
    scenarios\run.ps1 -Separate             # one process per file
#>
param(
    [string[]] $Name,
    [string]   $Preset = "clang-sanitize",
    [switch]   $Separate
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$repo = Split-Path $root -Parent
$exe  = Join-Path $repo "build\$Preset\encke.exe"
$out  = Join-Path $root "out"

if (-not (Test-Path $exe))
{
    throw "No $exe; build the $Preset preset first."
}

# The ASan runtime, for the sanitized build.
$env:PATH = "F:\msys2\clang64\bin;" + $env:PATH

if (-not $Name)
{
    # A scenario ending in "interactive" never quits.
    $Name = Get-ChildItem $root -Filter *.json |
        Where-Object { -not (Select-String -Path $_.FullName -Pattern '"op":\s*"interactive"' -Quiet) } |
        ForEach-Object BaseName
}

New-Item -ItemType Directory -Force $out | Out-Null
$files  = $Name | ForEach-Object { Join-Path $root "$_.json" }
$failed = $false

if ($Separate)
{
    foreach ($file in $files)
    {
        $base = [IO.Path]::GetFileNameWithoutExtension($file)
        & $exe --scenario $file *> (Join-Path $out "$base.log")
        if ($LASTEXITCODE -ne 0)
        {
            Write-Host "$base failed (exit $LASTEXITCODE); see out\$base.log" -ForegroundColor Red
            $failed = $true
        }
    }
}
else
{
    $arguments = $files | ForEach-Object { "--scenario"; $_ }
    & $exe @arguments *> (Join-Path $out "run.log")
    if ($LASTEXITCODE -ne 0)
    {
        Write-Host "the run failed (exit $LASTEXITCODE); see out\run.log" -ForegroundColor Red
        $failed = $true
    }
}

& (Join-Path $root "compare.ps1") -Name $Name
if ($LASTEXITCODE -ne 0)
{
    $failed = $true
}

if ($failed)
{
    exit 1
}
