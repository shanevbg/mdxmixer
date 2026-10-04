# release.ps1 — build, test, and package mdxmixer.
#
#   powershell -ExecutionPolicy Bypass -File release.ps1
#   ... -SkipBuild    package whatever is already in bin\Release
#   ... -SkipTests    package without running the suite (say why in the PR)
#   ... -ZipOnly      no MSI, so WiX is not needed
#   ... -DryRun       print the payload and stop
#
# Produces, in dist\:
#   mdxmixer-v<version>-portable.zip   unzip anywhere and run
#   mdxmixer-v<version>-x64.msi        per-user installer, no admin needed
#
# Both carry the same payload. The ZIP is the honest default for this program
# — it is a single exe that keeps its settings beside itself — and the MSI
# exists for people who would rather have a Start Menu entry and an entry in
# Apps & features than a folder they have to remember.

param(
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$ZipOnly,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist '_stage'

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Note($text) { Write-Host "   $text" -ForegroundColor DarkGray }

# ── 1. The version, from the one place that has it ───────────────────────
#
# The three NUMBERS, never MDXM_VERSION_STR. That macro is computed from them
# by stringification, so there is no literal "0.1.0" in version.h for a regex
# to find — and MDropDX12 learned what happens when you match on the string
# anyway: its public-snapshot workflow matched the COMMENT, and every release
# was titled "MDropDX12 v// to be expanded before it is stringified, ...".

Step 'Version'
$versionFile = Join-Path $root 'src\mdxmixer\version.h'
if (-not (Test-Path $versionFile)) { throw "version.h not found at $versionFile" }
$vh = Get-Content $versionFile -Raw
$maj = if ($vh -match '#define\s+MDXM_VERSION_MAJOR\s+(\d+)') { $Matches[1] } else { $null }
$min = if ($vh -match '#define\s+MDXM_VERSION_MINOR\s+(\d+)') { $Matches[1] } else { $null }
$pat = if ($vh -match '#define\s+MDXM_VERSION_PATCH\s+(\d+)') { $Matches[1] } else { $null }
if ($null -eq $maj -or $null -eq $min -or $null -eq $pat) {
    throw 'Could not read MDXM_VERSION_MAJOR/MINOR/PATCH from version.h'
}
$version = "$maj.$min.$pat"
Note "mdxmixer $version"

# ── 2. Build ─────────────────────────────────────────────────────────────

if (-not $SkipBuild) {
    Step 'Build (Release)'
    & (Join-Path $root 'build.ps1') Release
    if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)" }
}

$exe = Join-Path $root 'bin\Release\mdxmixer.exe'
if (-not (Test-Path $exe)) { throw "No build at $exe — drop -SkipBuild" }

# The binary has to agree with version.h.
#
# mdxmixer.rc reads version.h, so these cannot drift while the build is
# current — but -SkipBuild packages whatever is lying in bin\Release, which
# may predate the version bump. Shipping an installer named v0.2.0 full of
# v0.1.0 binaries is the kind of thing nobody notices until a bug report
# quotes the wrong version back.
$fileVersion = (Get-Item $exe).VersionInfo.FileVersion
if ($fileVersion -notmatch "^$([regex]::Escape($version))\b") {
    throw "bin\Release\mdxmixer.exe reports $fileVersion but version.h says $version — rebuild"
}
Note "mdxmixer.exe $fileVersion"

if (-not $SkipTests) {
    Step 'Tests'
    & (Join-Path $root 'build.ps1') Test
    if ($LASTEXITCODE -ne 0) { throw "Test build failed ($LASTEXITCODE)" }
    $testExe = Join-Path $root 'bin\Test\mdxmixer_test.exe'
    & $testExe
    # A release that cannot pass its own suite is not a release. Fatal rather
    # than a warning: a warning scrolls past in a build log.
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE) — not packaging" }
}

# ── 3. Stage the payload ─────────────────────────────────────────────────
#
# One payload, both packages. Anything that belongs in the ZIP belongs in the
# MSI, so there is no second list to forget to update.

Step 'Payload'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

$payload = @(
    @{ From = $exe;                                    To = 'mdxmixer.exe' }
    @{ From = Join-Path $root 'README.md';             To = 'README.md' }
    @{ From = Join-Path $root 'LICENSE';               To = 'LICENSE' }
    @{ From = Join-Path $root 'CONTRIBUTING.md';       To = 'CONTRIBUTING.md' }
    @{ From = Join-Path $root 'docs\ipc.md';           To = 'docs\ipc.md' }
    @{ From = Join-Path $root 'docs\rollout.md';       To = 'docs\rollout.md' }
)

# The README's pictures, so the shipped copy is not a page of broken image
# links. Added by directory rather than named one by one: the screenshots are
# REGENERATED (MDXM_TAB + MDXM_CAPTURE, see the end of the README), so a hand
# written list here is a list that goes stale the first time a shot is added.
#
# Filtered to image extensions rather than taking the whole directory: that
# folder also holds _preview.html, which exists to render the SVGs in a
# browser while editing them, and a scratch file has no business in a release.
foreach ($img in (Get-ChildItem (Join-Path $root 'docs\images') -File -ErrorAction SilentlyContinue |
                  Where-Object { $_.Extension -in '.png', '.svg', '.jpg', '.gif', '.webp' })) {
    $payload += @{ From = $img.FullName; To = "docs\images\$($img.Name)" }
}

foreach ($item in $payload) {
    if (-not (Test-Path $item.From)) { throw "payload file missing: $($item.From)" }
    $dest = Join-Path $stage $item.To
    New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force | Out-Null
    Copy-Item $item.From $dest -Force
    Note $item.To
}

# NOT shipped, and each for its own reason:
#   mdxmixer.json   settings are per-machine and written on first run; a
#                   bundled one would hand every installer the author's
#                   device ids and his 4% aux level.
#   *.pdb           debug symbols, several times the size of the exe.
#   tools\          make_icon.ps1 is a build-time thing, not a runtime one.
#   docs\specs\     the design record, written for whoever maintains this.

if ($DryRun) {
    Step 'Dry run — nothing packaged'
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1)
        '{0,-24} {1,10:N0} bytes' -f $rel, $_.Length
    }
    exit 0
}

# ── 4. Portable ZIP ──────────────────────────────────────────────────────

Step 'Portable ZIP'
$zip = Join-Path $dist "mdxmixer-v$version-portable.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Note "$([IO.Path]::GetFileName($zip))  $('{0:N0}' -f (Get-Item $zip).Length) bytes"

# ── 5. MSI ───────────────────────────────────────────────────────────────

if ($ZipOnly) {
    Step 'Done (ZIP only)'
    exit 0
}

Step 'MSI'
$wix = Get-Command wix -ErrorAction SilentlyContinue
if (-not $wix) {
    Write-Host '   WiX is not installed. Either:' -ForegroundColor Yellow
    Write-Host '     dotnet tool install --global wix --version 5.*' -ForegroundColor Yellow
    Write-Host '     wix extension add -g WixToolset.UI.wixext/5.0.2' -ForegroundColor Yellow
    Write-Host '   (version 5 on purpose: WiX 6 and later require accepting the' -ForegroundColor Yellow
    Write-Host '    Open Source Maintenance Fee EULA. 5 is MS-RL and free.)' -ForegroundColor Yellow
    Write-Host '   or re-run with -ZipOnly.' -ForegroundColor Yellow
    throw 'wix not found on PATH'
}

# PER-USER, into %LocalAppData%\Programs — and this is not a preference.
#
# mdxmixer keeps mdxmixer.json BESIDE THE EXE, which is what makes the
# portable copy portable: a folder you can move between machines with its
# settings intact. Install that into Program Files and the first setting the
# user changes cannot be written, because Program Files is not user-writable
# — the app would come up with defaults every single launch.
#
# Per-user also means no UAC prompt, no admin, and an uninstall that does not
# need elevation either. The cost is that it installs for one account rather
# than all of them, which for a personal audio mixer is the right trade.
#
# UpgradeCode is FIXED FOREVER. It is how Windows recognises that v0.2.0 is
# an upgrade of v0.1.0 rather than a second product; change it and users get
# two entries in Apps & features and two copies on disk.
$upgradeCode = 'B4F2A6D1-3E57-4C8A-9D21-7F6E0B5C84A3'

# The licence page wants RTF, and LICENSE is plain text. Converted here
# rather than keeping a second copy in the repo: two copies of a licence is
# exactly the pair that drifts, and the one nobody reads is the one that goes
# stale.
$rtf = Join-Path $dist 'license.rtf'
$licenseText = Get-Content (Join-Path $root 'LICENSE') -Raw
$escaped = $licenseText -replace '\\', '\\\\' -replace '\{', '\\{' -replace '\}', '\\}'
$escaped = $escaped -replace "`r`n", '\par ' -replace "`n", '\par '
# One line on purpose. PowerShell's line continuation is a BACKTICK, not a
# backslash: written with a backslash this parsed as Set-Content with no
# -Value (producing a 3-byte file and a blank licence page) and the RTF on
# the next line as a separate expression, which went to stdout.
$rtfBody = '{\rtf1\ansi\deff0{\fonttbl{\f0\fswiss Segoe UI;}}\fs18 ' + $escaped + '}'
Set-Content -Path $rtf -Encoding ASCII -Value $rtfBody

$wxs = Join-Path $dist 'mdxmixer.wxs'
$components = New-Object System.Text.StringBuilder
$dirs = @{}

# The component list is GENERATED from the staged payload rather than
# maintained by hand, so the MSI and the ZIP cannot drift apart. Each file is
# its own component with its own stable GUID: deriving the GUID from the
# file's relative path means a rebuild produces the same GUIDs, which is what
# lets Windows upgrade in place instead of reinstalling everything.
function New-StableGuid([string]$text) {
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $hash = $md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text))
    ([guid]::new($hash)).ToString().ToUpper()
}

foreach ($file in (Get-ChildItem $stage -Recurse -File | Sort-Object FullName)) {
    $rel = $file.FullName.Substring($stage.Length + 1)
    $sub = Split-Path $rel -Parent
    $dirId = 'INSTALLFOLDER'
    if ($sub) {
        if (-not $dirs.ContainsKey($sub)) { $dirs[$sub] = 'dir' + (New-StableGuid $sub).Replace('-', '') }
        $dirId = $dirs[$sub]
    }
    $compId = 'cmp' + (New-StableGuid $rel).Replace('-', '')
    $guid = New-StableGuid "mdxmixer:$rel"
    $src = $file.FullName.Replace('&', '&amp;')
    [void]$components.AppendLine(@"
      <Component Id="$compId" Directory="$dirId" Guid="{$guid}">
        <File Source="$src" KeyPath="yes" />
      </Component>
"@)
}

$subDirs = New-Object System.Text.StringBuilder
foreach ($sub in $dirs.Keys) {
    [void]$subDirs.AppendLine("        <Directory Id=""$($dirs[$sub])"" Name=""$sub"" />")
}

$wxsText = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- GENERATED by release.ps1. Edit the script, not this file. -->
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"
     xmlns:ui="http://wixtoolset.org/schemas/v4/wxs/ui">
  <!-- Language is NOT optional in practice. Left unset, WiX writes
       ProductLanguage=0, and Windows then registers the product without an
       Apps and features entry: the files install, the shortcut appears, and
       the only way to remove it is msiexec with the product code. Measured,
       not assumed. The first build of this installer did exactly that.
       (No double hyphens anywhere in these comments: XML forbids them, and
       wix rejects the whole file with WIX0104.) -->
  <Package Name="mdxmixer"
           Manufacturer="Shane Baker"
           Version="$version"
           UpgradeCode="$upgradeCode"
           Language="1033"
           Codepage="1252"
           Scope="perUser"
           Compressed="yes">

    <!-- Replace an older copy in place rather than sitting beside it. The
         message is what a user sees if they run an OLD installer over a
         newer install, which is a mistake worth naming. -->
    <MajorUpgrade DowngradeErrorMessage="A newer version of mdxmixer is already installed. Uninstall it first if you really want this older one." />
    <MediaTemplate EmbedCab="yes" />

    <!-- The Id ends in .ico because Windows Installer uses the Icon table key
         as the FILENAME it extracts to; the icon lands at
         %AppData%\Microsoft\Installer\{ProductCode}\AppIcon.ico and the Start
         Menu shortcut points there.

         Apps and features shows a generic icon regardless, and that is a
         Windows behaviour rather than a fault here: a per-user install gets
         its uninstall key written into HKLM by the installer service, and
         Windows will not put a path inside one user's roaming profile into an
         HKLM value, so DisplayIcon stays empty. Measured on MDropDX12's
         installer both with and without the extension (mdropdx12#436). -->
    <Icon Id="AppIcon.ico" SourceFile="$($root.Replace('&','&amp;'))\res\mdxmixer.ico" />
    <Property Id="ARPPRODUCTICON" Value="AppIcon.ico" />
    <Property Id="ARPHELPLINK" Value="https://github.com/shanevbg/mdxmixer" />
    <Property Id="ARPNOREPAIR" Value="1" />
    <!-- So Settings can show where it went, and the Modify button does not
         offer a repair this package has no use for. -->
    <SetProperty Id="ARPINSTALLLOCATION" Value="[INSTALLFOLDER]" After="CostFinalize" />

    <StandardDirectory Id="LocalAppDataFolder">
      <Directory Id="ProgramsFolder" Name="Programs">
        <Directory Id="INSTALLFOLDER" Name="mdxmixer">
$($subDirs.ToString())        </Directory>
      </Directory>
    </StandardDirectory>

    <StandardDirectory Id="ProgramMenuFolder">
      <Directory Id="AppShortcutFolder" Name="mdxmixer" />
    </StandardDirectory>

    <!-- Two features, which is what gives the Custom page something to
         offer. Express takes the first and not the second.

         Autostart is Level 1000 so it is NOT part of a Typical install:
         something that puts itself in your startup without being asked is a
         thing people resent, and the app has its own switch for it on the
         Options tab either way. Choosing Custom (or Complete) is how you say
         yes at install time. -->
    <Feature Id="Main" Title="mdxmixer"
             Description="The mixer itself, its documentation, and a Start Menu shortcut."
             Level="1" AllowAbsent="no" ConfigurableDirectory="INSTALLFOLDER">
      <ComponentGroupRef Id="Payload" />
      <ComponentRef Id="StartMenuShortcut" />
    </Feature>

    <Feature Id="Autostart" Title="Start with Windows"
             Description="Launch mdxmixer when you sign in. You can change this later on the Options tab."
             Level="1000">
      <ComponentRef Id="AutostartRunKey" />
    </Feature>

    <!-- Typical / Custom / Complete, a licence page, and a Browse button on
         the Custom page for the install folder. WixUI_Mondo is the stock set
         that offers a setup-type choice; the alternatives offer either a
         folder or a feature tree, not both. -->
    <ui:WixUI Id="WixUI_Mondo" InstallDirectory="INSTALLFOLDER" />
    <WixVariable Id="WixUILicenseRtf" Value="$($rtf.Replace('&','&amp;'))" />
  </Package>

  <Fragment>
    <ComponentGroup Id="Payload">
$($components.ToString())    </ComponentGroup>

    <!-- A per-user component needs a per-user keypath, and a registry value
         is the conventional one: HKCU is writable without elevation, where
         the installed FILES are only a keypath for per-machine installs. -->
    <Component Id="StartMenuShortcut" Directory="AppShortcutFolder" Guid="{$(New-StableGuid 'mdxmixer:startmenu')}">
      <Shortcut Id="AppShortcut"
                Name="mdxmixer"
                Description="Audio mixer: per-app channels, separate personal and streaming levels"
                Target="[INSTALLFOLDER]mdxmixer.exe"
                WorkingDirectory="INSTALLFOLDER"
                Icon="AppIcon.ico" />
      <RemoveFolder Id="RemoveAppShortcutFolder" Directory="AppShortcutFolder" On="uninstall" />
      <RegistryValue Root="HKCU" Key="Software\mdxmixer" Name="installed" Type="integer" Value="1" KeyPath="yes" />
    </Component>

    <!-- EXACTLY what the app's own autostart toggle writes: HKCU Run, value
         name "mdxmixer", the quoted exe path. Identical on purpose. If the
         installer wrote a different name or an unquoted path, the Options
         tab would report autostart off while Windows started it anyway, and
         unticking it there would leave the installer's value behind. -->
    <Component Id="AutostartRunKey" Directory="INSTALLFOLDER" Guid="{$(New-StableGuid 'mdxmixer:autostart')}">
      <RegistryValue Root="HKCU"
                     Key="Software\Microsoft\Windows\CurrentVersion\Run"
                     Name="mdxmixer"
                     Type="string"
                     Value="&quot;[INSTALLFOLDER]mdxmixer.exe&quot;"
                     KeyPath="yes" />
    </Component>
  </Fragment>
</Wix>
"@

Set-Content -Path $wxs -Value $wxsText -Encoding UTF8

$msi = Join-Path $dist "mdxmixer-v$version-x64.msi"
& wix build -arch x64 -ext WixToolset.UI.wixext -o $msi $wxs
if ($LASTEXITCODE -ne 0) { throw "wix build failed ($LASTEXITCODE)" }
Note "$([IO.Path]::GetFileName($msi))  $('{0:N0}' -f (Get-Item $msi).Length) bytes"

# ── 6. Done ──────────────────────────────────────────────────────────────

Step "mdxmixer $version"
Get-ChildItem $dist -File | Where-Object { $_.Extension -in '.zip', '.msi' } | ForEach-Object {
    '   {0,-40} {1,10:N0} bytes' -f $_.Name, $_.Length
}
Write-Host ''
Write-Host '   The MSI installs per-user, into %LocalAppData%\Programs\mdxmixer,' -ForegroundColor DarkGray
Write-Host '   because mdxmixer keeps its settings beside the exe and Program Files' -ForegroundColor DarkGray
Write-Host '   is not user-writable. No admin rights are needed to install it.' -ForegroundColor DarkGray
