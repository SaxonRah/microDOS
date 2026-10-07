param(
    [string]$Repo = "C:\microDOS",
    [int]$Slots = 128
)

$ErrorActionPreference = "Stop"

if ($Slots -lt 32 -or ($Slots -band ($Slots - 1)) -ne 0) {
    throw "Slots must be a power of two and at least 32."
}

$Path = Join-Path $Repo "pico\CMakeLists.txt"

if (-not (Test-Path $Path)) {
    throw "missing file: $Path"
}

$text = [IO.File]::ReadAllText($Path) -replace "`r`n", "`n"

$old = @'
target_compile_definitions(microdos_pico_native3_profile PRIVATE
    MICRODOS_ENABLE_NATIVE3=1
    MD_N3_PROFILE=1
    MD_JIT_PROFILE=1
    MD_JIT_LEGACY_HOTNESS=0
    MD_JIT_BLOCK_SLOTS=256
    MD_X86_TRACK_WRITES=1
    "MICRODOS_NATIVE3_CODE_BYTES=(128u*1024u)")
'@

$new = @"
target_compile_definitions(microdos_pico_native3_profile PRIVATE
    MICRODOS_ENABLE_NATIVE3=1
    MD_N3_PROFILE=1
    MD_JIT_PROFILE=1
    MD_JIT_LEGACY_HOTNESS=0
    MD_JIT_BLOCK_SLOTS=256
    MD_NATIVE_V2_RT_SLOTS=$Slots
    MD_X86_TRACK_WRITES=1
    "MICRODOS_NATIVE3_CODE_BYTES=(128u*1024u)")
"@

$first = $text.IndexOf($old, [StringComparison]::Ordinal)

if ($first -lt 0) {
    # Allow rerunning the experiment script by replacing an existing slot count.
    $pattern = '(?ms)(target_compile_definitions\(microdos_pico_native3_profile PRIVATE\s+MICRODOS_ENABLE_NATIVE3=1\s+MD_N3_PROFILE=1\s+MD_JIT_PROFILE=1\s+MD_JIT_LEGACY_HOTNESS=0\s+MD_JIT_BLOCK_SLOTS=256\s+)MD_NATIVE_V2_RT_SLOTS=\d+(\s+MD_X86_TRACK_WRITES=1\s+"MICRODOS_NATIVE3_CODE_BYTES=\(128u\*1024u\)"\))'

    if ($text -notmatch $pattern) {
        throw "Native-3 profile target anchor not found. No files changed."
    }

    $text = [regex]::Replace(
        $text,
        $pattern,
        ('$1MD_NATIVE_V2_RT_SLOTS=' + $Slots + '$2'),
        1
    )
}
else {
    $second = $text.IndexOf(
        $old,
        $first + $old.Length,
        [StringComparison]::Ordinal
    )

    if ($second -ge 0) {
        throw "Native-3 profile target anchor is not unique. No files changed."
    }

    $text = $text.Substring(0, $first) +
        $new +
        $text.Substring($first + $old.Length)
}

$enc = [System.Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($Path, $text, $enc)

Write-Host "Native-3 DOS profile NV2 slots: $Slots"
Write-Host "Changed: pico\CMakeLists.txt"
Write-Host ""
Write-Host "Run:"
Write-Host '  .\scripts\md_native3_dos_profile.ps1'
