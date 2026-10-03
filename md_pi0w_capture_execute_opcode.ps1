param(
    [string]$Repo = "C:\microDOS"
)

$ErrorActionPreference = "Stop"

$OutDir = Join-Path $Repo "build-pi0w\out"
$Elf = Join-Path $OutDir "microdos_pi0w_no_strict_align_failing.elf"
$Capture = Join-Path $OutDir "f6_execute_opcode_codegen.txt"

$Toolchain = "C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-aarch64-none-elf\bin"
$Objdump = Join-Path $Toolchain "aarch64-none-elf-objdump.exe"
$Nm = Join-Path $Toolchain "aarch64-none-elf-nm.exe"

foreach ($p in @($Elf, $Objdump, $Nm)) {
    if (-not (Test-Path $p)) {
        throw "Required file not found: $p"
    }
}

Remove-Item $Capture -Force -ErrorAction SilentlyContinue

function Add-Line([string]$Text = "") {
    Add-Content -LiteralPath $Capture -Value $Text -Encoding UTF8
}

function Add-Block([string]$Title, $Lines) {
    Add-Line ""
    Add-Line ("=" * 78)
    Add-Line $Title
    Add-Line ("=" * 78)
    $Lines | Out-File -LiteralPath $Capture -Append -Encoding utf8 -Width 4096
}

Add-Line "microDOS Pi Zero 2 W - exact md_execute_opcode codegen"
Add-Line ("Captured: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"))
Add-Line ("ELF: " + $Elf)
Add-Line ""
Add-Line "This is the preserved no--mstrict-align ELF that reproduces the first-F6 stop."
Add-Line "md_execute_opcode.isra.0 = 0x863e0"
Add-Line "next symbol md_interp_step = 0x889f0"

$symbols = & $Nm -n -C $Elf
Add-Block "RELEVANT SYMBOLS" (
    $symbols | Select-String -Pattern "md_execute_opcode|md_interp_step|md_decode_rm|md_x86_flags_materialize"
)

# Dump the exact function range.  Using addresses avoids any issue with objdump
# accepting a GCC-generated '.isra.0' symbol name in --disassemble=.
$dis = & $Objdump -drwC -l `
    --start-address=0x863e0 `
    --stop-address=0x889f0 `
    $Elf

Add-Block "FULL md_execute_opcode.isra.0 DISASSEMBLY" $dis

Add-Block "F6 / F7 OPCODE TESTS WITH CONTEXT" (
    $dis | Select-String `
        -Pattern "#0xf6|#0xf7|0xF6|0xF7" `
        -Context 18,36
)

Add-Block "ALL STORES TO MdX86 LAZY-FLAG OFFSET REGION WITH CONTEXT" (
    $dis | Select-String `
        -Pattern "\[x[0-9]+,\s*#(?:28|29|30|31|32|33|34|35)\]|\[x[0-9]+,\s*#0x(?:1c|1d|1e|1f|20|21|22|23)\]" `
        -Context 10,16
)

Add-Block "32-BIT / PAIRED STORE CANDIDATES" (
    $dis | Select-String `
        -Pattern "\bstur\s+w|\bstr\s+w|\bstp\s+w" `
        -Context 4,8
)

Add-Block "16-BIT / BYTE STORE CANDIDATES" (
    $dis | Select-String `
        -Pattern "\bstrh\b|\bsturh\b|\bstrb\b|\bsturb\b" `
        -Context 3,6
)

# Also provide a compact grep-like view to make it easy to spot offsets.
Add-Block "COMPACT LINES CONTAINING OFFSETS 28..35 / 0x1c..0x23" (
    $dis | Where-Object {
        $_ -match "#28\]" -or
        $_ -match "#29\]" -or
        $_ -match "#30\]" -or
        $_ -match "#31\]" -or
        $_ -match "#32\]" -or
        $_ -match "#33\]" -or
        $_ -match "#34\]" -or
        $_ -match "#35\]" -or
        $_ -match "#0x1c\]" -or
        $_ -match "#0x1d\]" -or
        $_ -match "#0x1e\]" -or
        $_ -match "#0x1f\]" -or
        $_ -match "#0x20\]" -or
        $_ -match "#0x21\]" -or
        $_ -match "#0x22\]" -or
        $_ -match "#0x23\]"
    }
)

Write-Host ""
Write-Host "=== EXACT F6 CODEGEN CAPTURE COMPLETE ==="
Write-Host "ELF was NOT rebuilt."
Write-Host ""
Write-Host "Upload this file:"
Write-Host "  $Capture"
