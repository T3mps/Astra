# Runs AstraTest for one configuration -- the PowerShell twin of
# scripts/run-tests.sh, with the same arguments:
#
#   scripts/run-tests.ps1 <Debug|Release|Dist> [--rng-seed N]
#
# Without --rng-seed the suite runs in declaration order; with it, GoogleTest
# shuffles test order with seed N (1..99999), so a failure reproduces by
# passing the same seed. JUnit XML goes to test-results\. Run
# scripts/build.ps1 <Config> first.
param([string] $Config)

$ErrorActionPreference = 'Stop'
$usage = 'usage: scripts/run-tests.ps1 <Debug|Release|Dist> [--rng-seed N]'

if ($Config -notin @('Debug', 'Release', 'Dist')) { Write-Error $usage; exit 2 }

$seed = $null
$rest = @($args)
for ($i = 0; $i -lt $rest.Count; $i++) {
    if ($rest[$i] -eq '--rng-seed' -and $i + 1 -lt $rest.Count) { $seed = $rest[++$i] }
    else { Write-Error $usage; exit 2 }
}
if ($null -ne $seed) {
    if ($seed -notmatch '^\d+$' -or [int]$seed -lt 1 -or [int]$seed -gt 99999) {
        Write-Error 'run-tests.ps1: --rng-seed must be 1..99999 (GoogleTest''s range)'
        exit 2
    }
}

$root = Split-Path -Parent $PSScriptRoot
$binary = Join-Path $root "bin/$Config-windows-x86_64/AstraTest/AstraTest.exe"
if (-not (Test-Path $binary)) {
    Write-Error "run-tests.ps1: no $Config AstraTest under bin\ -- run scripts/build.ps1 $Config first"
    exit 1
}

$results = Join-Path $root 'test-results'
New-Item -ItemType Directory -Force -Path $results | Out-Null
$suffix = if ($null -ne $seed) { "-seed$seed" } else { '' }
$report = Join-Path $results "AstraTest-windows-$Config$suffix.xml"
$testArgs = @('--gtest_brief=1', "--gtest_output=xml:$report")
if ($null -ne $seed) { $testArgs += @('--gtest_shuffle', "--gtest_random_seed=$seed") }

Push-Location $root
try {
    Write-Host "run-tests.ps1: $binary $($testArgs -join ' ')"
    & $binary @testArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
