<#
NV2-G hot-loop census (host, no hardware).

Boots the pinned MS-DOS 2.0 under the canonical interpreter, runs a workload
single-stepped, and ranks every interpreter back-edge loop by the guest
instructions it executes, with the owner today (Native-v2 special compiler,
NV2-G, or nobody) and EVERY NV2-G blocker in the loop body.

Workloads:
  tools     MASM FIND; / SORT <FIND.ASM >S.TXT / FIND /C "MOV" FIND.ASM / CHKDSK
            (real MS-DOS 2.0 programs from third_party\msdos\v2.0\bin)
  dos2test  DOS2TEST.COM (the splitbench session)
  both      (default)

Usage:
  .\md.bat build host
  .\scripts\md_nv2g_census.ps1 [-Workload tools|dos2test|both] [-Top 30]

Read "loop body owned today: X% of session" first; then the ranked rows and
the "sole blocker" table, which says what the next NV2-G feature would unlock.
#>
param(
    [ValidateSet("tools", "dos2test", "both")][string]$Workload = "both",
    [int]$Top = 30
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Bin = Join-Path $Repo "third_party\msdos\v2.0\bin"
$Src = Join-Path $Repo "third_party\msdos\v2.0\source"

function Find-Exe([string]$name) {
    foreach ($d in @("build-host\Release", "build-host\RelWithDebInfo", "build-host")) {
        $p = Join-Path $Repo "$d\$name.exe"
        if (Test-Path $p) { return $p }
    }
    throw "$name.exe not found. Run: .\md.bat build host  (needs .\md.bat deps msdos)"
}

$Census = Find-Exe "microdos_nv2g_census"
$Mkfat = Find-Exe "mkfat12"

$needed = @("MSDOS.SYS", "COMMAND.COM")
if ($Workload -ne "dos2test") { $needed += @("MASM.EXE", "SORT.EXE", "FIND.EXE", "CHKDSK.COM") }
foreach ($f in $needed) {
    if (-not (Test-Path (Join-Path $Bin $f))) { throw "missing $Bin\$f. Run: .\md.bat deps msdos" }
}
if ($Workload -ne "dos2test" -and -not (Test-Path (Join-Path $Src "FIND.ASM"))) {
    throw "missing $Src\FIND.ASM. Run: .\md.bat deps msdos"
}

$Logs = Join-Path $Repo "logs"
$Work = Join-Path $Repo "build-host\nv2g_census"
New-Item -ItemType Directory -Force -Path $Logs, $Work | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"

function New-Image([string]$out, [string[]]$adds) {
    $a = @("--command", (Join-Path $Bin "COMMAND.COM")) + $adds + @("--output", $out)
    & $Mkfat @a | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "mkfat12 failed for $out" }
}

function Invoke-Census([string]$name, [string]$img, [string[]]$cmds) {
    $log = Join-Path $Logs "nv2g-census-$stamp-$name.txt"
    $a = @((Join-Path $Bin "MSDOS.SYS"), $img, "--top", "$Top")
    foreach ($c in $cmds) { $a += "--run"; $a += $c }
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "NV2-G CENSUS: $name"
    Write-Host "============================================================"
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Census @a 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($rc -ne 0) { throw "census $name failed (exit $rc), see $log" }
    Write-Host "log: $log"
}

if ($Workload -eq "dos2test" -or $Workload -eq "both") {
    $img = Join-Path $Work "census_dos2test.img"
    New-Image $img @("--add", (Join-Path $Repo "tests\dos2\DOS2TEST.COM"), "DOS2TEST.COM")
    Invoke-Census "dos2test" $img @("DOS2TEST")
}

if ($Workload -eq "tools" -or $Workload -eq "both") {
    $img = Join-Path $Work "census_tools.img"
    New-Image $img @(
        "--add", (Join-Path $Bin "MASM.EXE"), "MASM.EXE",
        "--add", (Join-Path $Bin "SORT.EXE"), "SORT.EXE",
        "--add", (Join-Path $Bin "FIND.EXE"), "FIND.EXE",
        "--add", (Join-Path $Bin "CHKDSK.COM"), "CHKDSK.COM",
        "--add", (Join-Path $Src "FIND.ASM"), "FIND.ASM")
    # ' is typed as " by the census tool (no shell escaping of DOS quotes).
    Invoke-Census "tools" $img @(
        "MASM FIND;",
        "SORT <FIND.ASM >S.TXT",
        "FIND /C 'MOV' FIND.ASM",
        "CHKDSK")
}
