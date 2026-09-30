<#
.SYNOPSIS
    One-command ghost character pipeline: rigged FBX -> HPL1 base model + 15
    BVH clips (+ optional install into one or more game folders).

.DESCRIPTION
    1. Blender (headless) runs hpl_dae_export.py on the FBX, base mesh only:
       writes <Name>.dae, <Name>.tga (power of two, 24-bit) and <Name>.mat
       into -Out and prints a validation report ("[hpl] ..." lines).
    2. python runs bvh_to_hpl_clip.py for <Name>: 15 <Name>_<slot>.dae clips
       + <Name>_clips.json from the Motifect BVH pack.
    3. Prints a summary; with -Redist copies the files into
       <redist>\multiplayer\models and deletes the stale *.collcach there.
    Re-running with the same -Name overwrites that character in place
    (how malik / phillip are upgraded). See README_characters.md.

.EXAMPLE
    .\build_character.ps1 -Fbx C:\Art\ella.fbx -Name ella -BvhDir C:\Art\motifect\BVH

.EXAMPLE
    .\build_character.ps1 -Fbx C:\Art\meshy_biped.fbx -Name red -BvhDir C:\Art\motifect\BVH `
        -Texture C:\Art\meshy_biped_texture_0.png -DecimateTo 12000 -Scale 1.06

.EXAMPLE
    .\build_character.ps1 -Fbx C:\Art\malik_v2.fbx -Name malik -BvhDir C:\Art\motifect\BVH `
        -Redist 'D:\SteamLibrary\steamapps\common\Penumbra Overture\redist',
                'D:\SteamLibrary\steamapps\common\Penumbra Overture\redist_guest'
#>
[CmdletBinding()]
param(
    # Rigged character FBX (Mixamo download: FBX Binary, With Skin, T-pose).
    [Parameter(Mandatory = $true)][string]$Fbx,
    # Model name: lowercase letters/digits, no '_' (files are <Name>_<slot>.dae).
    [Parameter(Mandatory = $true)][string]$Name,
    # Motifect pack BVH folder (contains idle_neutral.bvh, walk_forward.bvh, ...).
    [string]$BvhDir,
    # blender.exe (3.6 or newer).
    [string]$Blender = 'C:\Program Files\Blender Foundation\Blender 4.2\blender.exe',
    # Game folder(s) to install into (each gets multiplayer\models\...).
    [string[]]$Redist = @(),
    # Size factor on top of the FBX's own size (0.95 for a Mixamo FBX; the
    # shipped Meshy characters use 1.06: Meshy exports 1.70 m tall).
    [double]$Scale = 0.95,
    # Diffuse image to use instead of the one inside the FBX (png/jpg/tga).
    [string]$Texture,
    # Largest texture edge in pixels (power of two).
    [int]$MaxTex = 2048,
    # Decimate the mesh to at most this many triangles before export
    # (Blender Decimate, collapse; 0 = off). 12000 is the recommended budget.
    [int]$DecimateTo = 0,
    # Output folder (default: the repository's multiplayer\models).
    [string]$Out,
    # Python 3 executable (default: python, then 'py -3').
    [string]$Python,
    # Export the base model only, no BVH clips.
    [switch]$SkipClips
)

$ErrorActionPreference = 'Stop'
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$tools = $PSScriptRoot
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $tools) 'models' }

function Fail([string]$msg) {
    Write-Host ''
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

function Step([string]$msg) {
    Write-Host ''
    Write-Host "==> $msg" -ForegroundColor Cyan
}

# Runs a native program, streams its output, returns @(exitCode, lines).
# stderr is merged as plain text (PowerShell 5.1 would otherwise turn every
# stderr line into an error record and stop under ErrorActionPreference Stop).
function Invoke-Native([string]$exe, [string[]]$argList) {
    $lines = New-Object System.Collections.Generic.List[string]
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @argList 2>&1 | ForEach-Object {
            $s = "$_"
            $lines.Add($s)
            Write-Host $s
        }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    return @($code, $lines)
}

# ---------------------------------------------------------------- checks
if ($Name -cnotmatch '^[a-z0-9]+$') {
    Fail "-Name '$Name': use lowercase letters and digits only (no '_', no spaces), e.g. 'ella'"
}
if (-not (Test-Path -LiteralPath $Fbx -PathType Leaf)) { Fail "FBX not found: $Fbx" }
if ($DecimateTo -lt 0) { Fail "-DecimateTo must be a triangle count (e.g. 12000) or 0 for off" }
$Fbx = (Resolve-Path -LiteralPath $Fbx).Path

if (-not (Test-Path -LiteralPath $Blender -PathType Leaf)) {
    $found = $null
    $bf = $null
    if ($env:ProgramFiles) { $bf = Join-Path $env:ProgramFiles 'Blender Foundation' }
    if ($bf -and (Test-Path -LiteralPath $bf)) {
        $found = Get-ChildItem -LiteralPath $bf -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'blender.exe' } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            Select-Object -First 1
    }
    if ($found) {
        Write-Host "blender.exe not at '$Blender'; using $found"
        $Blender = $found
    } else {
        Fail "blender.exe not found at '$Blender'. Install Blender 3.6+ or pass -Blender 'C:\path\to\blender.exe'"
    }
}

if ($Texture) {
    if (-not (Test-Path -LiteralPath $Texture -PathType Leaf)) { Fail "texture not found: $Texture" }
    $Texture = (Resolve-Path -LiteralPath $Texture).Path
}

$py = $null
$pyArgs = @()
if (-not $SkipClips) {
    if (-not $BvhDir) { Fail "-BvhDir <Motifect pack>\BVH is required (or pass -SkipClips)" }
    if (-not (Test-Path -LiteralPath (Join-Path $BvhDir 'idle_neutral.bvh') -PathType Leaf)) {
        Fail "-BvhDir '$BvhDir' does not contain idle_neutral.bvh: point it at the pack's BVH folder"
    }
    $BvhDir = (Resolve-Path -LiteralPath $BvhDir).Path

    # python 3: -Python, else 'python', else the 'py -3' launcher. The
    # Microsoft Store alias 'python.exe' exists but only prints a hint, so
    # each candidate is actually run.
    $cands = @()
    if ($Python) { $cands += , @($Python) } else { $cands += , @('python'); $cands += , @('py', '-3') }
    foreach ($c in $cands) {
        $exe = $c[0]
        if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { continue }
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $rest = @()
        if ($c.Count -gt 1) { $rest = $c[1..($c.Count - 1)] }
        $v = & $exe @rest -c 'import sys; print(sys.version_info[0])' 2>$null
        $ok = ($LASTEXITCODE -eq 0) -and ("$v".Trim() -eq '3')
        $ErrorActionPreference = $old
        if ($ok) { $py = $exe; $pyArgs = $rest; break }
    }
    if (-not $py) {
        Fail ("Python 3 not found. Install it from https://www.python.org/downloads/ " +
              "(tick 'Add python.exe to PATH'), or pass -Python C:\path\to\python.exe")
    }
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$dae = Join-Path $Out "$Name.dae"
$tga = Join-Path $Out "$Name.tga"
$mat = Join-Path $Out "$Name.mat"

# ------------------------------------------------------- 1. Blender export
Step "Blender: $Fbx -> $dae (base mesh only)"
$exporter = Join-Path $tools 'hpl_dae_export.py'
$bargs = @('--background', '--factory-startup', '--python-exit-code', '1',
           '--python', $exporter, '--',
           $Fbx, $Out, $Name, $Scale.ToString('R', $inv), '--no-clips',
           '--max-tex', $MaxTex.ToString($inv))
if ($Texture) { $bargs += @('--texture', $Texture) }
if ($DecimateTo -gt 0) { $bargs += @('--decimate-to', $DecimateTo.ToString($inv)) }
$r = Invoke-Native $Blender $bargs
$blenderCode = $r[0]
$report = @($r[1] | Where-Object { $_.StartsWith('[hpl]') })
if ($blenderCode -eq 2) {
    Fail "the exporter reported errors (see the [hpl] ERROR lines above); fix the rig and run again"
}
if ($blenderCode -ne 0 -or -not (Test-Path -LiteralPath $dae)) {
    Fail "Blender export failed (exit code $blenderCode); see the output above"
}

# ---------------------------------------------------- texture + material
if (-not (Test-Path -LiteralPath $tga)) {
    Write-Host "WARNING: $Name.tga was not written (no texture in the FBX?). Pass -Texture <file>, or copy a power-of-two $Name.tga into $Out" -ForegroundColor Yellow
}
if (-not (Test-Path -LiteralPath $mat)) {
    # same format as the shipped malik.mat / phillip.mat (LF, tabs, no BOM)
    $matText = "<Material>`n" +
               "`t<Main Type=`"Diffuse`" DepthTest=`"True`" UseAlpha=`"False`" />`n" +
               "`t<TextureUnits>`n" +
               "`t`t<Diffuse File=`"$Name.tga`" Mipmaps=`"True`" Type=`"2D`" Wrap=`"Repeat`" />`n" +
               "`t</TextureUnits>`n" +
               "</Material>`n"
    [System.IO.File]::WriteAllText($mat, $matText, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "wrote $mat"
}

# --------------------------------------------------------- 2. BVH clips
$clipLine = 'clips: skipped (-SkipClips)'
if (-not $SkipClips) {
    Step "Clips: bvh_to_hpl_clip.py --name $Name"
    $conv = Join-Path $tools 'bvh_to_hpl_clip.py'
    $cargs = @() + $pyArgs + @($conv, '--base', $dae, '--bvh-dir', $BvhDir, '--out', $Out, '--name', $Name)
    $r = Invoke-Native $py $cargs
    if ($r[0] -ne 0) {
        Fail "bvh_to_hpl_clip.py failed (exit code $($r[0])); see its output above"
    }
    $clips = @(Get-ChildItem -LiteralPath $Out -Filter "${Name}_*.dae" -File)
    $clipLine = "clips: $($clips.Count) x ${Name}_<slot>.dae + ${Name}_clips.json"
    $last = @($r[1] | Where-Object { $_ -like '*wrote*_clips.json*' }) | Select-Object -Last 1
    if ($last) { $clipLine += " -- $($last.Trim())" }
}

# ------------------------------------------------------------ 3. summary
Step "Summary for '$Name'"
foreach ($l in $report) {
    if ($l -match 'ERROR') { Write-Host $l -ForegroundColor Red }
    elseif ($l -match 'WARNING') { Write-Host $l -ForegroundColor Yellow }
    else { Write-Host $l }
}
Write-Host $clipLine
$files = @(Get-ChildItem -LiteralPath $Out -File | Where-Object {
    $_.Name -eq "$Name.dae" -or $_.Name -eq "$Name.mat" -or $_.Name -eq "$Name.tga" -or
    $_.Name -like "${Name}_*.dae" -or $_.Name -eq "${Name}_clips.json" })
$mb = ($files | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("output: {0} files, {1:N1} MB in {2}" -f $files.Count, $mb, $Out)

# ------------------------------------------------------------ 4. install
# 'powershell -File' passes "-Redist a,b" as ONE string: split entries that
# are not an existing folder on ',' / ';'
$targets = @()
foreach ($rd in $Redist) {
    if (-not $rd) { continue }
    if (Test-Path -LiteralPath $rd -PathType Container) { $targets += $rd; continue }
    $targets += @($rd -split '[,;]' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
foreach ($rd in $targets) {
    if (-not $rd) { continue }
    Step "Install -> $rd"
    if (-not (Test-Path -LiteralPath $rd -PathType Container)) {
        Write-Host "WARNING: '$rd' does not exist; skipped" -ForegroundColor Yellow
        continue
    }
    if (-not (Test-Path -LiteralPath (Join-Path $rd 'overture.exe'))) {
        Write-Host "note: no overture.exe in '$rd' (installing anyway)"
    }
    $dst = Join-Path (Join-Path $rd 'multiplayer') 'models'
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    foreach ($f in $files) {
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dst $f.Name) -Force
    }
    # the engine prefers a .collcach newer than the .dae: drop them all
    $caches = @(Get-ChildItem -LiteralPath $dst -Filter '*.collcach' -File -ErrorAction SilentlyContinue)
    foreach ($c in $caches) { Remove-Item -LiteralPath $c.FullName -Force }
    Write-Host ("copied {0} files to {1}; deleted {2} .collcach file(s)" -f $files.Count, $dst, $caches.Count)
}

Write-Host ''
Write-Host "Done. Check it: tools\anim_viewer.html (drop the files on it), then in game with" -ForegroundColor Green
Write-Host "ghost_preview=1 (README_characters.md 'Checking the result')." -ForegroundColor Green
