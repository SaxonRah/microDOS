param(
    [string]$Repo = "C:\microDOS"
)

$ErrorActionPreference = "Stop"

$Elf = Join-Path $Repo "build-pi0w\out\microdos_pi0w.elf"
$OutDir = Join-Path $Repo "build-pi0w\out"
$Capture = Join-Path $OutDir "f6_codegen_capture.txt"
$PreservedElf = Join-Path $OutDir "microdos_pi0w_no_strict_align_failing.elf"

$Toolchain = "C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-aarch64-none-elf\bin"
$Objdump = Join-Path $Toolchain "aarch64-none-elf-objdump.exe"
$Nm = Join-Path $Toolchain "aarch64-none-elf-nm.exe"
$Readelf = Join-Path $Toolchain "aarch64-none-elf-readelf.exe"

foreach ($p in @($Elf, $Objdump, $Nm, $Readelf)) {
    if (-not (Test-Path $p)) {
        throw "Required file not found: $p"
    }
}

# Preserve the exact ELF that reproduced the no--mstrict-align F6 failure.
Copy-Item $Elf $PreservedElf -Force

Remove-Item $Capture -Force -ErrorAction SilentlyContinue

function Add-Line([string]$Text = "") {
    Add-Content -LiteralPath $Capture -Value $Text -Encoding UTF8
}

function Add-CommandOutput([string]$Title, [scriptblock]$Command) {
    Add-Line ""
    Add-Line ("=" * 78)
    Add-Line $Title
    Add-Line ("=" * 78)
    & $Command 2>&1 | Out-File -LiteralPath $Capture -Append -Encoding utf8 -Width 4096
}

Add-Line "microDOS Pi Zero 2 W - no-strict-align F6 codegen capture"
Add-Line ("Captured: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"))
Add-Line ("ELF:      " + $Elf)
Add-Line ("Preserved:" + $PreservedElf)
Add-Line ""

$fi = Get-Item $Elf
Add-Line ("ELF bytes: " + $fi.Length)
Add-Line ("ELF mtime: " + $fi.LastWriteTime)

Add-CommandOutput "ELF HEADER" {
    & $Readelf -h $Elf
}

Add-CommandOutput "SECTION HEADERS" {
    & $Readelf -S $Elf
}

Add-CommandOutput "SORTED SYMBOLS" {
    & $Nm -n -C $Elf
}

Add-CommandOutput "INTERESTING SYMBOLS" {
    & $Nm -n -C $Elf |
        Select-String -Pattern "md_interp_step|md_execute_opcode|md_op_group3|md_x86_logic8|md_x86_lazy|kernel_main"
}

Add-CommandOutput "md_interp_step DISASSEMBLY" {
    & $Objdump -d -C -l --disassemble=md_interp_step $Elf
}

Add-CommandOutput "kernel_main DISASSEMBLY" {
    & $Objdump -d -C -l --disassemble=kernel_main $Elf
}

# Static-inline helpers may have been fully folded into md_interp_step.
# Keep a complete disassembly so we can correlate exact addresses even when
# GCC does not leave md_op_group3/md_x86_lazy as standalone symbols.
$FullDisasm = Join-Path $OutDir "microdos_pi0w_no_strict_align_full_disasm.txt"
& $Objdump -drwC -l $Elf |
    Out-File -LiteralPath $FullDisasm -Encoding utf8 -Width 4096

Add-Line ""
Add-Line ("Full disassembly saved separately: " + $FullDisasm)

Add-CommandOutput "LOAD/STORE CANDIDATES IN md_interp_step" {
    $step = & $Objdump -d -C --disassemble=md_interp_step $Elf
    $step | Select-String -Pattern "\bstr\b|\bstrh\b|\bstur\b|\bstp\b|\bldr\b|\bldrh\b|\bldur\b|\bldp\b"
}

Add-CommandOutput "OFFSETS OF INTEREST (0x1c..0x24)" {
    # MdX86 offsets around the lazy-flag fields:
    # lazy_op=28 (0x1c), lazy_carry=29, lazy_a=30 (0x1e),
    # lazy_b=32 (0x20), lazy_res=34 (0x22).
    $step = & $Objdump -d -C --disassemble=md_interp_step $Elf
    $step | Select-String -Pattern "#0x1c|#0x1d|#0x1e|#0x1f|#0x20|#0x21|#0x22|#0x23|#0x24"
}

Write-Host ""
Write-Host "=== F6 CODEGEN CAPTURE COMPLETE ==="
Write-Host "Preserved failing ELF:"
Write-Host "  $PreservedElf"
Write-Host ""
Write-Host "Primary capture:"
Write-Host "  $Capture"
Write-Host ""
Write-Host "Full disassembly:"
Write-Host "  $FullDisasm"
Write-Host ""
Write-Host "Upload f6_codegen_capture.txt next."
