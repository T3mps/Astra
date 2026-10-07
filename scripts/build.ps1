# Generates and builds Astra on Windows -- the PowerShell twin of
# scripts/build.sh, with the same arguments:
#
#   scripts/build.ps1 <Debug|Release|Dist> [premake options...]
#
# Fetches the premake pinned in scripts/premake.lock into .tools\ (SHA-256
# checked), generates a solution for the newest Visual Studio installed
# (found with vswhere), and builds the whole solution with that MSBuild.
param([string] $Config)

$ErrorActionPreference = 'Stop'
$PremakeArgs = @($args)

if ($Config -notin @('Debug', 'Release', 'Dist')) {
    Write-Error 'usage: scripts/build.ps1 <Debug|Release|Dist> [premake options...]'
    exit 2
}

$root = Split-Path -Parent $PSScriptRoot

$lock = @{}
foreach ($line in Get-Content (Join-Path $root 'scripts/premake.lock')) {
    if ($line -match '^\s*(#|$)') { continue }
    $key, $value = $line -split '\s+', 2
    $lock[$key] = $value.Trim()
}
$version = $lock['version']
$premakeDir = Join-Path $root ".tools/premake/$version"
$premake = Join-Path $premakeDir 'premake5.exe'

if (-not (Test-Path $premake)) {
    $url = "https://github.com/premake/premake-core/releases/download/v$version/premake-$version-windows.zip"
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        Write-Host "build.ps1: fetching $url"
        $zip = Join-Path $tmp 'premake.zip'
        Invoke-WebRequest -Uri $url -OutFile $zip -MaximumRetryCount 4 -RetryIntervalSec 2
        $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
        if ($actual -ne $lock['windows']) {
            Write-Error "build.ps1: SHA-256 mismatch for $url (expected $($lock['windows']), got $actual) -- refusing it"
            exit 1
        }
        Expand-Archive -Path $zip -DestinationPath $tmp
        New-Item -ItemType Directory -Force -Path $premakeDir | Out-Null
        Copy-Item (Join-Path $tmp 'premake5.exe') $premake
    }
    finally {
        Remove-Item -Recurse -Force $tmp
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsVersion = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationVersion
if (-not $vsVersion) { Write-Error 'build.ps1: no Visual Studio with MSBuild found'; exit 1 }
$action = switch ([int]($vsVersion.Split('.')[0])) {
    17 { 'vs2022' }
    18 { 'vs2026' }
    default { Write-Error "build.ps1: unsupported Visual Studio $vsVersion"; exit 1 }
}
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1

Push-Location $root
try {
    & $premake $action @PremakeArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $solution = if ($action -eq 'vs2026' -and (Test-Path 'Astra.slnx')) { 'Astra.slnx' } else { 'Astra.sln' }
    Write-Host "build.ps1: $Config with Visual Studio $vsVersion ($action, $solution)"
    & $msbuild $solution "/p:Configuration=$Config" /m /nologo /v:minimal
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
