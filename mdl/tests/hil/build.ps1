<#
.SYNOPSIS
  Build the AT32F435 HIL targets.

.DESCRIPTION
  Nothing in this sandbox's toolchain is on the default PATH, and each
  PowerShell tool call is a fresh process, so every build has to prepend
  the same three directories and pass PYTHON= explicitly. This script is
  that boilerplate, nothing more -- the actual build is still just
  `mingw32-make` in each target directory.

  Three directories are needed, not one:
    - arm-none-eabi-gcc      the cross compiler
    - mingw64/bin            `make` is mingw32-make.exe, not make.exe
    - Git usr/bin            the Makefiles use `mkdir -p` and `rm -rf`

.PARAMETER Target
  m0 | m1 | m2 | m3 | m4 | all   (default: all)

.PARAMETER Clean
  Run `make clean` first. Always do this after editing a header that
  lives outside the target's own directory -- the -MMD dependency files
  cover headers, but not Makefile or linker-script changes.

.EXAMPLE
  .\build.ps1                 # build everything, print a size table
  .\build.ps1 m4              # just M4
  .\build.ps1 m1 -Clean       # rebuild M1 from scratch
  .\build.ps1 all -Clean -Verbose   # full rebuild, show compiler lines
#>
[CmdletBinding()]
param(
    [ValidateSet('m0', 'm1', 'm2', 'm3', 'm4', 'all')]
    [string]$Target = 'all',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

# ---- toolchain locations (see memory/reference_toolchain_paths.md) -----
$ArmBin   = 'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin'
$MinGwBin = 'C:\Users\51771\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin'
$GitUsr   = 'C:\Program Files\Git\usr\bin'
$Python   = 'C:\Users\51771\AppData\Local\Programs\Python\Python312\python.exe'

foreach ($p in @($ArmBin, $MinGwBin, $GitUsr)) {
    if (-not (Test-Path $p)) { throw "toolchain directory missing: $p" }
}
if (-not (Test-Path $Python)) { throw "python missing: $Python" }

$env:Path = "$ArmBin;$MinGwBin;$GitUsr;" + $env:Path

# packer.py needs a real interpreter; the bare `python` on PATH is the
# Microsoft Store stub, which errors out instead of running.
$MakeArgs = @("PYTHON=$($Python -replace '\\','/')")

$Root = $PSScriptRoot
$Targets = if ($Target -eq 'all') { 'm0', 'm1', 'm2', 'm3', 'm4' } else { @($Target) }

# Windows PowerShell 5.1 wraps every stderr line from a native .exe in an
# ErrorRecord when you redirect with 2>&1. Under ErrorActionPreference
# 'Stop' that terminates the script on the linker's harmless "LOAD segment
# with RWX permissions" note -- and on every compiler warning. gcc's real
# diagnostics only reach stdout via that same redirect, so dropping 2>&1
# is not an option either. Relax the preference for the build loop and
# decide success from whether firmware.elf actually appeared.
$ErrorActionPreference = 'Continue'

$Results = @()
$Failed  = $false

foreach ($t in $Targets) {
    $dir = Join-Path $Root "at32f435_$t"
    if (-not (Test-Path $dir)) { throw "no such target directory: $dir" }

    Write-Host "==== $t ====" -ForegroundColor Cyan
    Push-Location $dir
    try {
        if ($Clean) {
            & mingw32-make clean @MakeArgs 2>&1 | Out-Null
        }

        $out = & mingw32-make @MakeArgs 2>&1
        $makeExit = $LASTEXITCODE

        # The linker's "LOAD segment with RWX permissions" note fires on
        # every target (the .mdl_arena section is NOLOAD but sits in RAM)
        # and is expected -- filter it so it doesn't drown real output.
        # Errors and warnings are NOT the same signal and must not share a
        # bucket. They used to: everything landed in one $problems list, the
        # target was reported as built-with-warnings, and success was decided
        # by Test-Path on firmware.elf -- which a STALE elf from an earlier
        # run satisfies. A target whose compile failed outright therefore
        # printed 'built, with warnings' and the script still finished
        # 'BUILD OK': a green light over a red build, which is worse than
        # no check at all.
        $errors = $out | Where-Object {
            $_ -match ': error|: fatal error|undefined reference|No such file|Error \d+'
        }
        $warnings = $out | Where-Object {
            $_ -match ': warning:'
        } | Where-Object {
            # Expected on every target: .mdl_arena is NOLOAD but in RAM.
            $_ -notmatch 'LOAD segment with RWX'
        }

        if ($VerbosePreference -eq 'Continue') { $out | Write-Host }

        $elf = Join-Path $dir 'build\firmware.elf'

        # make's own exit code is the authority on whether THIS run
        # succeeded; the elf merely existing only says some earlier one did.
        if ($makeExit -ne 0 -or $errors -or -not (Test-Path $elf)) {
            $Failed = $true
            Write-Host "FAILED (make exit $makeExit)" -ForegroundColor Red
            $errors | Select-Object -First 12 | ForEach-Object { Write-Host "  $_" }
            if (-not $errors) {
                $out | Select-Object -Last 12 | ForEach-Object { Write-Host "  $_" }
            }
            continue
        }

        if ($warnings) {
            Write-Host "ok, with warnings:" -ForegroundColor Yellow
            $warnings | Select-Object -First 6 | ForEach-Object { Write-Host "  $_" }
        } else {
            Write-Host "ok" -ForegroundColor Green
        }

        $fields = ((& arm-none-eabi-size $elf)[1] -split '\s+' | Where-Object { $_ -ne '' })
        $flash = [int]$fields[0] + [int]$fields[1]
        $Results += [pscustomobject]@{
            Target  = $t
            Text    = [int]$fields[0]
            Data    = [int]$fields[1]
            Bss     = [int]$fields[2]
            FlashB  = $flash
            'Pct256K' = [math]::Round($flash / 262144 * 100, 1)
        }
    }
    finally { Pop-Location }
}

if ($Results) {
    Write-Host ''
    $Results | Format-Table -AutoSize
}

if ($Failed) {
    Write-Host 'BUILD FAILED' -ForegroundColor Red
    exit 1
}
Write-Host 'BUILD OK' -ForegroundColor Green
