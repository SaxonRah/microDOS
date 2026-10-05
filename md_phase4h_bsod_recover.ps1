param(
    [string]$Repo = "C:\microDOS"
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path $Repo).Path

$GoodCommit = "7665864c0278c5605426e58342b6c9100d5db33a"

$Files = @(
    @{
        Path = "pico/CMakeLists.txt"
        ExpectedBlob = "4b380e6e40dd7a446b09a805c5e931c867c59306"
        ExpectedBytes = 12668
    },
    @{
        Path = "pico/microdos_pico.c"
        ExpectedBlob = "b32444b16161b229430aeabfab7e6338e79ecce0"
        ExpectedBytes = 31068
    },
    @{
        Path = "src/runtime/native_v2_runtime.c"
        ExpectedBlob = "561fe33c38cb7464ee1879356e29499505b6d16c"
        ExpectedBytes = 21278
    }
)

$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$BackupDir = Join-Path $Repo "build-pico\recovery-phase4h-bsod-v4-$Stamp"
New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null

function Export-GitBlobRaw([string]$ObjectId, [string]$OutPath) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "git"
    $psi.Arguments = "cat-file blob $ObjectId"
    $psi.WorkingDirectory = $Repo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi

    if (-not $proc.Start()) {
        throw "Could not start git cat-file."
    }

    $fs = [IO.File]::Open($OutPath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $proc.StandardOutput.BaseStream.CopyTo($fs)
    }
    finally {
        $fs.Dispose()
    }

    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    if ($proc.ExitCode -ne 0) {
        throw "git cat-file failed for $ObjectId : $stderr"
    }
}

function Count-Nuls([byte[]]$Bytes) {
    $n = 0
    foreach ($b in $Bytes) {
        if ($b -eq 0) { ++$n }
    }
    return $n
}

Write-Host "=== microDOS Phase 4H BSOD exact historical recovery v5 ==="
Write-Host "Known-good commit:"
Write-Host "  $GoodCommit"
Write-Host ""
Push-Location $Repo
try {
    & git cat-file -e "$GoodCommit^{commit}" 2>$null
    $commitExit = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($commitExit -ne 0) {
    throw "Known-good commit $GoodCommit is not available in the local Git object database. Nothing was changed."
}
Write-Host "Local Git history preflight: PASS"
Write-Host ""

Write-Host "Archiving current damaged working-tree files:"
Write-Host "  $BackupDir"

foreach ($f in $Files) {
    $src = Join-Path $Repo $f.Path
    if (-not (Test-Path $src)) {
        throw "Required working-tree file is missing: $src"
    }

    $safe = ($f.Path -replace '[\\/]', '__') + ".damaged"
    Copy-Item $src (Join-Path $BackupDir $safe) -Force

    $bytes = [IO.File]::ReadAllBytes($src)
    Write-Host ("  {0}: {1} bytes, NUL={2}" -f $f.Path, $bytes.Length, (Count-Nuls $bytes))
}

Write-Host ""
Write-Host "Validating known-good historical Git blobs BEFORE changing anything..."

$Candidates = @()

foreach ($f in $Files) {
    $spec = "${GoodCommit}:$($f.Path)"

    Push-Location $Repo
    try {
        $oidLines = & git rev-parse --verify $spec 2>$null
        $gitExit = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    if ($gitExit -ne 0 -or $null -eq $oidLines) {
        throw "git rev-parse --verify '$spec' failed with exit code $gitExit. No source file has been replaced."
    }

    $oid = ($oidLines | Select-Object -First 1).Trim()
    if ([string]::IsNullOrWhiteSpace($oid)) {
        throw "git rev-parse returned an empty object id for '$spec'. No source file has been replaced."
    }

    Write-Host "  $($f.Path)"
    Write-Host "    historical blob: $oid"
    Write-Host "    expected blob:   $($f.ExpectedBlob)"

    if ($oid -ne $f.ExpectedBlob) {
        throw "Historical blob mismatch for $($f.Path). Expected $($f.ExpectedBlob), got $oid. No source file has been replaced."
    }

    $candidateName = ($f.Path -replace '[\\/]', '__') + ".good"
    $candidate = Join-Path $BackupDir $candidateName
    Export-GitBlobRaw $oid $candidate

    $bytes = [IO.File]::ReadAllBytes($candidate)
    $nul = Count-Nuls $bytes

    Write-Host "    bytes: $($bytes.Length) expected=$($f.ExpectedBytes)"
    Write-Host "    NUL:   $nul"

    if ($bytes.Length -ne $f.ExpectedBytes) {
        throw "Historical byte-count mismatch for $($f.Path). No source file has been replaced."
    }
    if ($nul -ne 0) {
        throw "Historical candidate for $($f.Path) contains NUL bytes. No source file has been replaced."
    }

    $Candidates += @{
        Path = $f.Path
        Candidate = $candidate
    }
}

Write-Host ""
Write-Host "All three historical candidates validated."
Write-Host "Replacing ONLY the three zeroed production files..."

foreach ($c in $Candidates) {
    $dst = Join-Path $Repo $c.Path
    [IO.File]::WriteAllBytes($dst, [IO.File]::ReadAllBytes($c.Candidate))
    Write-Host "  restored $($c.Path)"
}

Write-Host ""
Write-Host "Verifying restored working-tree files..."

foreach ($f in $Files) {
    $path = Join-Path $Repo $f.Path
    $bytes = [IO.File]::ReadAllBytes($path)
    $nul = Count-Nuls $bytes
    if ($bytes.Length -ne $f.ExpectedBytes -or $nul -ne 0) {
        throw "Post-write verification failed for $($f.Path). Crash-state and good candidates remain in $BackupDir."
    }
    Write-Host "  PASS $($f.Path): $($bytes.Length) bytes, NUL=0"
}

Write-Host ""
Write-Host "Configuring and rebuilding known-good production..."
Push-Location $Repo
try {
    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed after exact historical restore."
    }

    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Production build failed after exact historical restore."
    }

    Write-Host ""
    Write-Host "=== RECOVERY PASS ==="
    Write-Host "Restored the three zeroed production files exactly from commit $GoodCommit."
    Write-Host "microdos_pico_nativev2 configures and builds successfully."
    Write-Host ""
    Write-Host "Recovery archive:"
    Write-Host "  $BackupDir"
    Write-Host ""
    Write-Host "Current status of the recovered files:"
    & git status --short -- pico/CMakeLists.txt pico/microdos_pico.c src/runtime/native_v2_runtime.c
    Write-Host ""
    Write-Host "IMPORTANT: do not commit/push yet. The current branch/index still records the bad zero blobs."
}
finally {
    Pop-Location
}
