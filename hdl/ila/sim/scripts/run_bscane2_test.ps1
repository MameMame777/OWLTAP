#==============================================================================
# OwlTAP ILA BSCANE2 -- DSim UVM Test Runner (PowerShell)
#
# Verifies ila_bscane2_top.sv (Xilinx 7-series / Zynq PL USB JTAG path).
# BSCANE2 unisim primitive is replaced by the behavioral model in
# hdl/ila/sim/tb/bscane2_model.sv.
#
# Usage:
#   .\run_bscane2_test.ps1 <test_name> [-Waves] [-Verbosity UVM_LOW]
#   .\run_bscane2_test.ps1 -Regression
#
# Tests:
#   ila_bscane2_smoke_test    -- IDCODE read via USER1 + 37-bit frame
#   ila_bscane2_trigger_test  -- trigger match + full buffer readback
#==============================================================================

param(
    [Parameter(Position=0)]
    [string]$TestName,

    [switch]$Waves,

    [ValidateSet("UVM_LOW","UVM_MEDIUM","UVM_HIGH","UVM_DEBUG")]
    [string]$Verbosity = "UVM_LOW",

    [int]$Seed = 1,

    [switch]$CompileOnly,

    [switch]$Regression,

    [switch]$Help
)

$ErrorActionPreference = "Stop"

if ($Help) {
    Write-Host @"
Usage: .\run_bscane2_test.ps1 <test_name> [options]
       .\run_bscane2_test.ps1 -Regression

Tests:
  ila_bscane2_smoke_test
  ila_bscane2_config_read_test
  ila_bscane2_trigger_test
"@
    exit 0
}

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$SimDir     = Split-Path -Parent $ScriptDir
$ExecDir    = Join-Path  $SimDir "exec"
$LogDir     = Join-Path  $ExecDir "logs_bscane2"
$WaveDir    = Join-Path  $ExecDir "wave_bscane2"

$DsimHome   = "C:\Program Files\Altair\DSim\2026"
$ConfigFile = "dsim_bscane2_config.f"
$TopModule  = "ila_bscane2_tb_top"

function Setup-Environment {
    $activateScript = Join-Path $DsimHome "shell_activate.ps1"
    if (Test-Path $activateScript) {
        . $activateScript
    } else {
        Write-Warning "shell_activate.ps1 not found -- falling back to manual setup"
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

    # License: personal free-tier file — override any stale 2025.x env vars
    $licFile = "$env:LOCALAPPDATA\metrics-ca\dsim-license.json"
    if (Test-Path $licFile) {
        $env:DSIM_LICENSE_FILE = $licFile
        $env:DSIM_LICENSE      = $licFile
    }
    # Ensure dsim_root and dsim_home point to 2026, not 2025.1
    $env:DSIM_ROOT = $DsimHome
    $env:DSIM_HOME = $DsimHome

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
    if ($CompileOnly) { $args += @("-genimage", "compiled_bscane2_${Name}") }

    Write-Host "================================================"
    Write-Host "DSim BSCANE2 Test: $Name"
    Write-Host "Log: $logFile"
    Write-Host "================================================"

    Push-Location $ExecDir
    try {
        $p = Start-Process -FilePath $dsimExe -ArgumentList $args `
                           -Wait -PassThru -NoNewWindow
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
    $tests = @("ila_bscane2_smoke_test", "ila_bscane2_config_read_test", "ila_bscane2_trigger_test")
    $results = @()
    foreach ($t in $tests) {
        $results += Run-OneTest -Name $t
    }
    Write-Host ""
    Write-Host "================================================"
    Write-Host "BSCANE2 REGRESSION SUMMARY"
    Write-Host "================================================"
    $failCount = 0
    foreach ($r in $results) {
        $status = if ($r.Pass) { "PASS" } else { "FAIL"; $failCount++ }
        Write-Host ("{0,-36}  {1}" -f $r.Name, $status)
    }
    exit ($failCount -gt 0)
} else {
    if (-not $TestName) {
        Write-Error "Specify a test name or use -Regression"
        exit 1
    }
    $r = Run-OneTest -Name $TestName
    exit ($r.Pass ? 0 : 1)
}
