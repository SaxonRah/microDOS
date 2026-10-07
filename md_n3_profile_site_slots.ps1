param(
    [string]$Repo = "C:\microDOS",
    [int]$SiteSlots = 1024
)

$ErrorActionPreference = "Stop"

if ($SiteSlots -lt 256 -or ($SiteSlots -band ($SiteSlots - 1)) -ne 0) {
    throw "SiteSlots must be a power of two and at least 256."
}

$Path = Join-Path $Repo "pico\CMakeLists.txt"

if (-not (Test-Path $Path)) {
    throw "missing file: $Path"
}

$text = [IO.File]::ReadAllText($Path) -replace "`r`n", "`n"

# Normalize the profile target back to the proven 32-slot NV2 runtime first.
$text = [regex]::Replace(
    $text,
    '(?m)^\s*MD_NATIVE_V2_RT_SLOTS=\d+\s*$\n?',
    ""
)

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
    MD_N3_SITE_SLOTS=$SiteSlots
    MD_NATIVE_V2_RT_SLOTS=32
    MD_X86_TRACK_WRITES=1
    "MICRODOS_NATIVE3_CODE_BYTES=(128u*1024u)")
"@

$first = $text.IndexOf($old, [StringComparison]::Ordinal)

if ($first -lt 0) {
    # Allow rerunning after a previous site-slot experiment.
    $pattern = '(?ms)(target_compile_definitions\(microdos_pico_native3_profile PRIVATE\s+MICRODOS_ENABLE_NATIVE3=1\s+MD_N3_PROFILE=1\s+MD_JIT_PROFILE=1\s+MD_JIT_LEGACY_HOTNESS=0\s+MD_JIT_BLOCK_SLOTS=256\s+)(?:MD_N3_SITE_SLOTS=\d+\s+)?(?:MD_NATIVE_V2_RT_SLOTS=\d+\s+)?(MD_X86_TRACK_WRITES=1\s+"MICRODOS_NATIVE3_CODE_BYTES=\(128u\*1024u\)"\))'

    if ($text -notmatch $pattern) {
        throw "Native-3 profile target anchor not found. No files changed."
    }

    $replacement =
        '${1}' +
        "MD_N3_SITE_SLOTS=$SiteSlots`n    " +
        "MD_NATIVE_V2_RT_SLOTS=32`n    " +
        '${2}'

    $text = [regex]::Replace(
        $text,
        $pattern,
        $replacement,
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

Write-Host "Native-3 DOS profile:"
Write-Host "  N3 site slots: $SiteSlots"
Write-Host "  NV2 slots:     32 (restored proven baseline)"
Write-Host ""
Write-Host "Run:"
Write-Host '  .\scripts\md_native3_dos_profile.ps1'
