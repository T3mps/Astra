# Mirror the standalone working copy into the GitHub clone (publish step),
# excluding .git and build artifacts. Review the clone's git status after
# running, then commit + push from there.
$ErrorActionPreference = "Stop"
$src = "D:\dev\starworks\Mosaic"
$dst = "D:\dev\github\Mosaic"

robocopy $src $dst /MIR /NFL /NDL /NJH `
    /XD .git bin bin-int ide ide-md .vs `
    /XF *.user *.slnx *.sln

# robocopy exit codes < 8 are success (0 = no change, 1 = files copied, etc.).
if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)"; exit 1 }
Write-Host "Synced $src -> $dst. Review with: git -C `"$dst`" status"
exit 0
