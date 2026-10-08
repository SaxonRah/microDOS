param(
    [string]$Repo = "C:\microDOS"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Repo = [IO.Path]::GetFullPath($Repo)
$Path = Join-Path $Repo "tests\test_native_v2g.c"

if (-not (Test-Path -LiteralPath $Path)) {
    throw "Missing file: $Path"
}

$text = [IO.File]::ReadAllText($Path) -replace "`r`n", "`n"

$old = @'
    /* Different producers merging at a join (21A5:00E3 shape): G-3. */
    ok &= expect_reject("producer-merge", producer_merge,
                        sizeof(producer_merge), 0x0100u);
'@

$new = @'
    /*
     * G-2B2: different FLAGS producers may merge at a join when every
     * incoming producer can materialize exact native lazy state.
     * producer_merge contains a guarded byte store, so validate the normal
     * G memory/store metadata as well as successful compilation.
     */
    ok &= expect_ok_mem("producer-merge", producer_merge,
                        sizeof(producer_merge), 1u);
'@

$count = ([regex]::Matches(
    $text,
    [regex]::Escape($old)
)).Count

if ($count -ne 1) {
    throw "producer-merge test anchor: expected exactly 1 match, found $count"
}

$text = $text.Replace($old, $new)

$oldPass = 'puts("native-v2g G-1A/G-2B0/G-2B1 compiler tests PASS");'
$newPass = 'puts("native-v2g G-1A/G-2B0/G-2B1/G-2B2 compiler tests PASS");'

$countPass = ([regex]::Matches(
    $text,
    [regex]::Escape($oldPass)
)).Count

if ($countPass -ne 1) {
    throw "PASS banner anchor: expected exactly 1 match, found $countPass"
}

$text = $text.Replace($oldPass, $newPass)

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backupDir = Join-Path $Repo "nv2g_g2bc_testfix_backup\$stamp"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
Copy-Item -LiteralPath $Path `
    -Destination (Join-Path $backupDir "test_native_v2g.c") `
    -Force

[IO.File]::WriteAllText(
    $Path,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host ""
Write-Host "APPLIED: G-2B2 producer-merge host-test update"
Write-Host "file:   $Path"
Write-Host "backup: $backupDir"
Write-Host ""
Write-Host "Changed only the stale expectation:"
Write-Host "  producer-merge: reject -> successful G memory/store compile"
Write-Host ""
Write-Host "Next:"
Write-Host "  cd $Repo"
Write-Host "  .\scripts\run_nv2g_g2bc_validation.ps1"
