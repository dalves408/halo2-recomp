# Converts a folder of upscaled pack PNGs to BC7 DDS files for the hi-res
# texture pack (src/gpu/tex_pack.c), keeping file names and subfolders, and
# copies mip_aliases.txt beside them. Cube-map faces (*_faceN.png) are
# skipped: they need texassemble to become one DDS cubemap.
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Pack,
    [string]$Texconv = 'texconv.exe'   # DirectXTex texconv: on PATH, or give its full path
)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command $Texconv -ErrorAction SilentlyContinue)) { throw "texconv not found: $Texconv (get it from github.com/microsoft/DirectXTex releases)" }
$Source = (Resolve-Path $Source).Path
New-Item -ItemType Directory -Force $Pack | Out-Null
$Pack = (Resolve-Path $Pack).Path
$done = 0; $skipped = 0; $failed = 0
Get-ChildItem $Source -Recurse -Filter *.png | ForEach-Object {
    if ($_.Name -notmatch '^[0-9a-fA-F]{16}_\d+x\d+_') { $skipped++; return }
    if ($_.BaseName -match '_face\d$') { $skipped++; return }
    $rel = $_.DirectoryName.Substring($Source.Length).TrimStart('\')
    $out = if ($rel) { Join-Path $Pack $rel } else { $Pack }
    New-Item -ItemType Directory -Force $out | Out-Null
    & $Texconv -nologo -f BC7_UNORM -m 0 -dx10 -y -o $out $_.FullName | Out-Null
    if ($LASTEXITCODE -eq 0) { $done++ } else { $failed++; Write-Warning "texconv failed: $($_.FullName)" }
}
Copy-Item (Join-Path $PSScriptRoot '..\mip_aliases.txt') $Pack -Force
"$done DDS written to $Pack, $skipped skipped (not pack names or cube faces), $failed failed"
