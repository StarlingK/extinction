param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [string]$Destination = (Split-Path $PSScriptRoot -Parent),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$overlay = Join-Path $root 'toolkit\xboxrecomp-0.17.0\src'
$target = Join-Path $Destination 'xboxrecomp-0.17.0'

if (-not (Test-Path $Archive)) { throw "archive not found: $Archive" }
if (Test-Path $target) {
    if (-not $Force) { throw "$target already exists; pass -Force to replace it" }
    Remove-Item $target -Recurse -Force
}
New-Item -ItemType Directory -Force $Destination | Out-Null
Expand-Archive $Archive -DestinationPath $Destination

$n = 0
Get-ChildItem $overlay -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($overlay.Length + 1)
    $dest = Join-Path $target "src\$rel"
    New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
    Copy-Item $_.FullName $dest -Force
    $n++
}
"toolkit ready at $target ($n patched files)"

