# build.ps1 — Invoke from any shell (Git Bash, cmd, Claude's Bash tool, etc.)
#
# Binaries land in  bin\<Configuration>\  at the repo root:
#   bin\Debug\mdxmixer.exe      bin\Release\mdxmixer.exe
#   bin\Test\mdxmixer_test.exe
# Intermediates go to obj\<Configuration>\. Both are git-ignored.
#
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1 [Debug|Release|Test] [Clean]
#
# Examples:
#   powershell -ExecutionPolicy Bypass -File build.ps1                # Debug
#   powershell -ExecutionPolicy Bypass -File build.ps1 Release
#   powershell -ExecutionPolicy Bypass -File build.ps1 Test           # mdxmixer_test.exe
#   powershell -ExecutionPolicy Bypass -File build.ps1 Debug Clean    # clean, then build
#   powershell -ExecutionPolicy Bypass -File build.ps1 Release -Install
#
# -Install copies the built exe to a RUN LOCATION outside the build tree, the
# way MDropDX12 is run from MilkAssetsin. The point is that a rebuild cannot
# disturb the copy you are using: this script only kills instances running from
# bin\<Configuration>, so the installed one keeps running while you work on the
# next build. Config travels with the exe (mdxmixer.json is exe-relative), and
# an existing one at the destination is never overwritten.
#
# x64 is the only platform: this is an x64-only project, so there is nothing to
# select and no platform suffix on the output directories. A literal "x64" in
# either argument position is accepted and ignored, so older invocations still work.

param(
    [string]$Configuration = "Debug",
    [string]$Target        = "Build",   # "Build" or "Clean"
    [string]$Unused        = "",        # swallows a trailing legacy "x64"/"Clean"
    [switch]$Install,                   # copy the result to the run location
    [string]$InstallDir    = "C:\Code\Entertainment\MilkAssets\bin\mdxmixer"
)

# Tolerate the old  build.ps1 <Config> x64 [Clean]  argument shape.
if ($Target -eq "x64") { $Target = if ($Unused) { $Unused } else { "Build" } }
if ($Target -eq "")    { $Target = "Build" }

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── 1. Locate vswhere ──────────────────────────────────────────────────────────
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found. Install Visual Studio 2022 or later."
    exit 1
}

# ── 2. Find MSBuild ────────────────────────────────────────────────────────────
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild `
           -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1

# Fallback: vswhere without -requires (works after fresh VS install where component
# registration may not be complete yet)
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    $msbuild = & $vswhere -latest -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
}

# Fallback: known installation paths
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    $paths = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($p in $paths) { if (Test-Path $p) { $msbuild = $p; break } }
}

if (-not $msbuild -or -not (Test-Path $msbuild)) {
    Write-Error "MSBuild not found. Ensure Visual Studio 2022 with C++ workload is installed."
    exit 1
}

$cores = [Environment]::ProcessorCount
Write-Host "MSBuild  : $msbuild"
Write-Host "Config   : $Configuration (x64)"
Write-Host "Target   : $Target"
Write-Host "Output   : $(Join-Path $PSScriptRoot "bin\$Configuration")"
Write-Host "Parallel : $cores cores (/m + /MP)"
Write-Host ""

# ── 3. Kill running exe (only from THIS configuration's output dir) ───────────
# A copy running from elsewhere (an installed mdxmixer) carries the same process
# name, so matching on the name alone would kill it on every build.
$exeName = if ($Configuration -eq "Test") { "mdxmixer_test" } else { "mdxmixer" }
$outDir = Join-Path $PSScriptRoot "bin\$Configuration"

$outDirFull = $null
if (Test-Path -LiteralPath $outDir) {
    $outDirFull = (Resolve-Path -LiteralPath $outDir).Path
}
if ($outDirFull) {
    $prefix = $outDirFull.TrimEnd('\') + '\'
    Get-Process -Name $exeName -ErrorAction SilentlyContinue | ForEach-Object {
        $path = $null
        try { $path = $_.Path } catch { $path = $null }
        if ($path -and $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            Write-Host "Killing $exeName.exe (PID $($_.Id)) from $($prefix)..."
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        }
        elseif ($path) {
            Write-Host "Leaving $exeName.exe (PID $($_.Id)) alone -- runs from $path"
        }
    }
}

# ── 4. Run the build ───────────────────────────────────────────────────────────
$project = Join-Path $PSScriptRoot "src\mdxmixer\mdxmixer.vcxproj"

# "Clean" target means clean first, then build (full rebuild)
if ($Target -eq "Clean") {
    Write-Host "Cleaning..."
    & $msbuild $project `
        /t:Clean `
        /p:Configuration=$Configuration `
        /p:Platform=x64 `
        /p:PlatformToolset=v143 `
        /m `
        /nologo `
        /clp:Summary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host ""
    Write-Host "Building..."
    $Target = "Build"
}

& $msbuild $project `
    /t:$Target `
    /p:Configuration=$Configuration `
    /p:Platform=x64 `
    /p:PlatformToolset=v143 `
    /m `
    /nologo `
    /clp:Summary

# REMEMBERED, NOT EXITED ON. There used to be an `exit $LASTEXITCODE` here,
# which made every line below it unreachable: -Install was accepted, printed
# nothing, installed nothing, and still exited 0, so the only symptom was a run
# location that quietly never changed.
$buildExit = $LASTEXITCODE

# ── Install: put the build somewhere a rebuild cannot touch ───────────────────
#
# Shane asked for "the same type of install" MDropDX12 has, so that future
# builds do not have an impact on a running mdxmixer -- which until now they
# did, every single time, because the only copy was the one in bin\Release that
# the linker overwrites.
#
# Only after a build that SUCCEEDED. Installing the previous binary because
# this build failed would replace a working copy with something older while
# reporting the failure, which is the worst of both.
if ($Install -and $Target -ne "Clean" -and $buildExit -eq 0) {
    if ($Configuration -eq "Test") {
        Write-Host "Nothing to install for the Test configuration."
    } else {
        $src = Join-Path $PSScriptRoot "bin\$Configuration\mdxmixer.exe"
        if (-not (Test-Path -LiteralPath $src)) {
            Write-Error "No build to install at $src"
            exit 1
        }
        if (-not (Test-Path -LiteralPath $InstallDir)) {
            New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
        }
        $dstExe = Join-Path $InstallDir "mdxmixer.exe"

        # The installed copy is very likely running -- that is the whole point
        # of it -- and the file is locked while it is. Ask it to exit first;
        # it is the user's own mixer, so this is the one instance that gets a
        # polite request rather than a kill.
        $running = @(Get-Process -Name mdxmixer -ErrorAction SilentlyContinue | Where-Object {
            $p = $null
            try { $p = $_.Path } catch { $p = $null }
            $p -and ($p -ieq $dstExe)
        })
        foreach ($proc in $running) {
            Write-Host "Stopping the installed mdxmixer (PID $($proc.Id)) so it can be replaced..."
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
        if ($running.Count -gt 0) { Start-Sleep -Milliseconds 700 }

        Copy-Item -LiteralPath $src -Destination $dstExe -Force
        $pdb = Join-Path $PSScriptRoot "bin\$Configuration\mdxmixer.pdb"
        if (Test-Path -LiteralPath $pdb) {
            Copy-Item -LiteralPath $pdb -Destination (Join-Path $InstallDir "mdxmixer.pdb") -Force
        }

        # Settings live beside the exe, so a fresh install would otherwise come
        # up with nothing. Seed it once from the build tree; never overwrite,
        # because after that the installed copy's config is the real one.
        foreach ($name in @("mdxmixer.json", "windows.json")) {
            $cfgSrc = Join-Path $PSScriptRoot "bin\$Configuration\$name"
            $cfgDst = Join-Path $InstallDir $name
            if ((Test-Path -LiteralPath $cfgSrc) -and -not (Test-Path -LiteralPath $cfgDst)) {
                Copy-Item -LiteralPath $cfgSrc -Destination $cfgDst
                Write-Host "Seeded $name from the build tree."
            }
        }

        Write-Host ""
        Write-Host "Installed: $dstExe"
        Write-Host "Run that copy. Rebuilds only kill instances under bin\$Configuration,"
        Write-Host "so it keeps running while the next build happens."
        if ($running.Count -gt 0) {
            Write-Host "It was running and has been stopped; start it again when you are ready."
        }
    }
}

# The BUILD's exit code, whatever the install did. A caller is asking whether
# the code compiled; a copy that failed has already said so loudly above.
exit $buildExit
