# Vendor the standalone Mosaic into a consumer repo (Astra / Manifold2D /
# Arcane) as a faithful source mirror. Excludes .git/.github, build outputs, and
# the standalone test-dep vendor/ tree (each consumer supplies its own Catch2).
# The vendored premake5.lua is a standalone workspace and is INERT in a consumer
# -- the consumer builds Mosaic via its own project.
#
# Usage: .\scripts\vendor.ps1 -Consumer <path-to-consumer-repo-root>
param([Parameter(Mandatory = $true)][string]$Consumer)
$ErrorActionPreference = "Stop"
$src = "D:\dev\starworks\Mosaic"
$dst = "$Consumer\ThirdParty\Mosaic"

robocopy $src $dst /MIR /NFL /NDL /NJH `
    /XD .git .github bin bin-int ide ide-md .vs vendor `
    /XF *.user *.slnx *.sln

if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)"; exit 1 }
Write-Host "Vendored $src -> $dst"
exit 0
