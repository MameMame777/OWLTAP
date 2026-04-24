#==============================================================================
# OwlTAP ILA — DSim UVM Test Runner (PowerShell)
#
# Usage:
#   .\run_test.ps1 <test_name> [-Waves] [-Verbosity LVL] [-Seed N]
#   .\run_test.ps1 -Regression
#
# Tests: ila_tap_smoke_test, ila_trigger_match_test,
#        ila_prepost_split_test, ila_chain_bypass_test
#
# Modeled after AXIUART_RV32I/scripts/run_test.ps1.
#==============================================================================

param(
    [Parameter(Position=0)]
    [string]$TestName,

    [switch]$Waves,

    [ValidateSet("UVM_LOW", "UVM_MEDIUM", "UVM_HIGH", "UVM_DEBUG")]
    [string]$Verbosity = "UVM_LOW",

    [int]$Seed = 1,

    [switch]$CompileOnly,

    [switch]$Regression,

    [switch]$Help
)

$ErrorActionPreference = "Stop"

if ($Help) {
    Write-Host @"
Usage: .\run_test.ps1 <test_name> [options]
       .\run_test.ps1 -Regression

Tests:
  ila_tap_smoke_test
  ila_config_read_test
  ila_trigger_match_test
  ila_prepost_split_test
  ila_chain_bypass_test
"@
    exit 0
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$SimDir    = Split-Path -Parent $ScriptDir
$ExecDir   = Join-Path  $SimDir "exec"
$LogDir    = Join-Path  $ExecDir "logs"
$WaveDir   = Join-Path  $ExecDir "wave"

# Always use DSim 2026 regardless of any pre-existing DSIM_HOME env var.
$DsimHome = "C:\Program Files\Altair\DSim\2026"
$ConfigFile = "dsim_config.f"
$TopModule  = "ila_tb_top"

function Setup-Environment {
    # DSim 2026: use the official shell_activate.ps1 for all PATH/env vars.
    $activateScript = Join-Path $DsimHome "shell_activate.ps1"
    if (Test-Path $activateScript) {
        . $activateScript
    } else {
        Write-Warning "shell_activate.ps1 not found at $activateScript — falling back to manual setup."
        $env:DSIM_HOME     = $DsimHome
        $env:DSIM_ROOT     = $DsimHome
        $env:DSIM_LIB_PATH = Join-Path $DsimHome "lib"
        $paths = @(
            (Join-Path $DsimHome "bin"),
            (Join-Path $DsimHome "mingw\bin"),
            (Join-Path $DsimHome "dsim_deps\bin"),
            (Join-Path $DsimHome "lib")
        )
        $env:PATH = ($paths -join ";") + ";" + $env:PATH
    }

    # -------------------------------------------------------------------------
    # License setup — two mutually exclusive modes:
    #
    #   Altair Units (local license server, licensing2026.0):
    #     ALTAIR_LICENSE_PATH = 6200@localhost
    #     DSIM_LICENSE must be UNSET (AU is unavailable while DSIM_LICENSE is set)
    #
    #   Free Individual License (cloud-issued dsim-license.json):
    #     DSIM_LICENSE = path to dsim-license.json
    #     ALTAIR_LICENSE_PATH must be UNSET
    #
    # If DSIM_LICENSE is set but points to an old DSim version directory,
    # clear it so Altair Units licensing can take over.
    # -------------------------------------------------------------------------
    $altairLmHome = "C:\Program Files\Altair\licensing2026.0"

    # Always clear stale DSIM_LICENSE (old version path or missing file).
    if ($env:DSIM_LICENSE -and
        ($env:DSIM_LICENSE -like "*\DSim\*" -or -not (Test-Path $env:DSIM_LICENSE))) {
        Remove-Item Env:DSIM_LICENSE -ErrorAction SilentlyContinue
    }
    # Always clear stale ALTAIR_LICENSE_PATH so we can re-evaluate below.
    Remove-Item Env:ALTAIR_LICENSE_PATH -ErrorAction SilentlyContinue

    if (-not $env:DSIM_LICENSE) {
        # Use Altair Units only if the license server has actual .lic files to serve.
        $licFiles = Get-ChildItem $altairLmHome -Filter "*.lic" -ErrorAction SilentlyContinue
        if ($licFiles) {
            $env:ALTAIR_LICENSE_PATH = "6200@localhost"
            Write-Host "License: Altair Units (6200@localhost)"
        } else {
            # Free Individual License (cloud-issued dsim-license.json).
            $licFile = Join-Path $env:LOCALAPPDATA "metrics-ca\dsim-license.json"
            if (Test-Path $licFile) {
                $env:DSIM_LICENSE = $licFile
                Write-Host "License: Free Individual ($licFile)"
            } else {
                Write-Warning "No license found. Set DSIM_LICENSE or place a .lic file in $altairLmHome."
            }
        }
    }

    New-Item -ItemType Directory -Force -Path $LogDir  | Out-Null
    New-Item -ItemType Directory -Force -Path $WaveDir | Out-Null
}

function Run-OneTest {
    param([string]$Name)

    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $logFile   = Join-Path $LogDir  "${Name}_${timestamp}.log"
    $waveFile  = Join-Path $WaveDir "${Name}_${timestamp}.mxd"

    $dsimExe = Join-Path $DsimHome "bin\dsim.exe"
    $args = @(
        "-timescale", "1ns/1ps",
        "-f",         $ConfigFile,
        "-top",       $TopModule,
        "-sv_seed",   $Seed,
        "-l",         $logFile,
        "-uvm",       "1.2",
        "+UVM_TESTNAME=$Name",
        "+UVM_VERBOSITY=$Verbosity"
    )
    if ($Waves)       { $args += @("-waves", $waveFile) }
    if ($CompileOnly) { $args += @("-genimage", "compiled_${Name}") }

    Write-Host "================================================"
    Write-Host "DSim ILA Test: $Name"
    Write-Host "Log: $logFile"
    Write-Host "================================================"
    Write-Host "$dsimExe $($args -join ' ')"

    Push-Location $ExecDir
    try {
        $p = Start-Process -FilePath $dsimExe -ArgumentList $args -Wait -PassThru -NoNewWindow
        $exitCode = $p.ExitCode
    } finally {
        Pop-Location
    }

    $uvmErr = 0; $uvmFat = 0; $passed = $false
    if (Test-Path $logFile) {
        Get-Content $logFile | ForEach-Object {
            if ($_ -match "^\s*UVM_ERROR\s*:\s*(\d+)") { $uvmErr = [int]$Matches[1] }
            if ($_ -match "^\s*UVM_FATAL\s*:\s*(\d+)") { $uvmFat = [int]$Matches[1] }
            if ($_ -match "TEST PASSED")               { $passed = $true }
        }
    }

    $ok = ($uvmErr -eq 0 -and $uvmFat -eq 0 -and $passed -and $exitCode -eq 0)
    Write-Host ""
    Write-Host "UVM_ERROR=$uvmErr  UVM_FATAL=$uvmFat  exit=$exitCode  passed=$passed"
    if ($ok) { Write-Host "Status: PASS" } else { Write-Host "Status: FAIL" }

    return [PSCustomObject]@{ Name=$Name; Pass=$ok; Log=$logFile }
}

Setup-Environment

if ($Regression) {
    $regFile = Join-Path $SimDir "regression_tests.json"
    if (-not (Test-Path $regFile)) {
        Write-Error "regression_tests.json not found: $regFile"
        exit 2
    }
    $cfg = Get-Content $regFile -Raw | ConvertFrom-Json
    $results = @()
    foreach ($t in $cfg.tests) {
        $results += Run-OneTest -Name $t
    }

    Write-Host ""
    Write-Host "================================================"
    Write-Host "REGRESSION SUMMARY"
    Write-Host "================================================"
    $failCount = 0
    foreach ($r in $results) {
        $status = if ($r.Pass) { "PASS" } else { "FAIL"; $failCount++ }
        Write-Host ("{0,-32}  {1}" -f $r.Name, $status)
    }
    exit ($failCount -gt 0)
}

if (-not $TestName) {
    Write-Error "TestName is required (or use -Regression)"
    exit 2
}

$r = Run-OneTest -Name $TestName
exit (!$r.Pass)
