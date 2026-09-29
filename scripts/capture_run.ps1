[CmdletBinding()]
param(
    [ValidateSet('dos2', 'host')]
    [string]$Target = 'dos2',

    # 0 = unlimited (interactive default). Use a finite budget for scripted runs.
    [long]$Budget = 0,

    [switch]$ResetDisk,
    [switch]$SkipClean,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$SkipRun,
    [switch]$NoStringTrace,
    # M12.4: the M12.2 '$' diagnostic is now opt-in. -NoStringTrace is still
    # accepted for old command lines and is the default behavior.
    [switch]$StringTrace,
    [switch]$TraceDisk,

    [string]$LogDirectory = 'logs',
    [string]$LogName,

    [switch]$StrictRunExitCode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    $scriptDir = Split-Path -Parent $PSCommandPath
    return (Resolve-Path (Join-Path $scriptDir '..')).Path
}

# Native-process stdout must never be captured as the function's PowerShell
# return value. Start-Transcript can then record it exactly as it is displayed,
# while the exit code travels out-of-band through this script-scoped variable.
$script:LastMdExitCode = 0

function Invoke-MdStep {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [switch]$AllowFailure
    )

    Write-Host ''
    Write-Host ('=' * 78)
    Write-Host "[capture] $Name"
    Write-Host "[capture] command: .\md.bat $($Arguments -join ' ')"
    Write-Host ('=' * 78)

    & $script:MdBat @Arguments
    $exitCode = $LASTEXITCODE
    $script:LastMdExitCode = $exitCode

    Write-Host "[capture] exit code: $exitCode"

    if (($exitCode -ne 0) -and -not $AllowFailure) {
        throw "$Name failed with exit code $exitCode"
    }
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$script:MdBat = Join-Path $repoRoot 'md.bat'
if (-not (Test-Path -LiteralPath $script:MdBat -PathType Leaf)) {
    throw "md.bat was not found at $script:MdBat"
}

$logDirPath = if ([System.IO.Path]::IsPathRooted($LogDirectory)) {
    $LogDirectory
} else {
    Join-Path $repoRoot $LogDirectory
}

New-Item -ItemType Directory -Path $logDirPath -Force | Out-Null

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($LogName)) {
    $LogName = "microdos-$Target-$timestamp.txt"
} elseif (-not $LogName.EndsWith('.txt', [System.StringComparison]::OrdinalIgnoreCase)) {
    $LogName += '.txt'
}

$logPath = Join-Path $logDirPath $LogName
$latestPath = Join-Path $logDirPath 'latest.txt'

$transcriptStarted = $false
$overallExitCode = 0
$runExitCode = 0
$previousTrace = $env:MICRODOS_TRACE_FIRST_DOLLAR
$traceWasPresent = Test-Path Env:MICRODOS_TRACE_FIRST_DOLLAR

try {
    Start-Transcript -Path $logPath -Force | Out-Null
    $transcriptStarted = $true

    Write-Host 'microDOS captured run'
    Write-Host "[capture] started:     $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))"
    Write-Host "[capture] repo:        $repoRoot"
    Write-Host "[capture] target:      $Target"
    if ($Target -eq 'dos2') {
        if ($Budget -gt 0) {
            Write-Host "[capture] budget:      $Budget"
        } else {
            Write-Host "[capture] budget:      unlimited"
        }
        Write-Host "[capture] reset disk:  $([bool]$ResetDisk)"
        Write-Host "[capture] string trace:$([bool]$StringTrace -and -not [bool]$NoStringTrace)"
        Write-Host "[capture] disk trace:  $([bool]$TraceDisk)"
    }
    Write-Host "[capture] log:         $logPath"

    if (Get-Command git -ErrorAction SilentlyContinue) {
        try {
            $commit = (& git -C $repoRoot rev-parse --short HEAD 2>$null)
            if ($LASTEXITCODE -eq 0 -and $commit) {
                Write-Host "[capture] git commit:  $commit"
            }

            $dirty = (& git -C $repoRoot status --porcelain 2>$null)
            if ($LASTEXITCODE -eq 0) {
                Write-Host "[capture] git dirty:   $([bool]$dirty)"
            }
        } catch {
            # Git metadata is useful but never required for a captured run.
        }
    }

    if (-not $SkipClean) {
        Invoke-MdStep -Name 'CLEAN' -Arguments @('clean')
    }

    if (-not $SkipBuild) {
        Invoke-MdStep -Name 'BUILD HOST' -Arguments @('build', 'host')
    }

    if (-not $SkipTests) {
        Invoke-MdStep -Name 'TEST' -Arguments @('test')
    }

    if ($ResetDisk -and $Target -eq 'dos2') {
        Invoke-MdStep -Name 'RESET DOS2 DISK IMAGE' -Arguments @('image', 'dos2')
    }

    if (-not $SkipRun) {
        if ($Target -eq 'dos2') {
            Write-Host ''
            Write-Host '[capture] Interactive DOS run starting.'
            Write-Host '[capture] Keyboard remains attached to the real console.'
            Write-Host '[capture] Ctrl+] exits microDOS. Ctrl+C goes to DOS as ^C. Disk writes are saved immediately.'

            if ($TraceDisk) {
                $env:MICRODOS_TRACE_DISK = '1'
            } else {
                Remove-Item Env:MICRODOS_TRACE_DISK -ErrorAction SilentlyContinue
            }

            if ($NoStringTrace -or -not $StringTrace) {
                Remove-Item Env:MICRODOS_TRACE_FIRST_DOLLAR -ErrorAction SilentlyContinue
            } else {
                $env:MICRODOS_TRACE_FIRST_DOLLAR = '1'
                Write-Host "[capture] M12.2 diagnostic: first post-COMMAND '$' write will dump COMMAND AH=40/STRING_OUT origin plus the guest trace and stop."
            }

            $args = @('run', 'dos2', [string]$Budget)

            Invoke-MdStep -Name 'RUN DOS2' -Arguments $args -AllowFailure
            $runExitCode = $script:LastMdExitCode
        } else {
            Invoke-MdStep -Name 'RUN HOST' -Arguments @('run', 'host') -AllowFailure
            $runExitCode = $script:LastMdExitCode
        }

        if ($StrictRunExitCode -and $runExitCode -ne 0) {
            throw "Run step exited with code $runExitCode"
        }
    }

    Write-Host ''
    Write-Host ('=' * 78)
    Write-Host '[capture] COMPLETE'
    Write-Host "[capture] run exit:    $runExitCode"
    Write-Host "[capture] finished:    $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))"
    Write-Host "[capture] log:         $logPath"
    Write-Host ('=' * 78)
}
catch {
    $overallExitCode = 1
    Write-Host ''
    Write-Host ('=' * 78)
    Write-Host '[capture] FAILED'
    Write-Host "[capture] $($_.Exception.Message)"
    Write-Host "[capture] log: $logPath"
    Write-Host ('=' * 78)
}
finally {
    if ($traceWasPresent) {
        $env:MICRODOS_TRACE_FIRST_DOLLAR = $previousTrace
    } else {
        Remove-Item Env:MICRODOS_TRACE_FIRST_DOLLAR -ErrorAction SilentlyContinue
    }

    if ($transcriptStarted) {
        try {
            Stop-Transcript | Out-Null
        } catch {
        }
    }

    if (Test-Path -LiteralPath $logPath -PathType Leaf) {
        try {
            Copy-Item -LiteralPath $logPath -Destination $latestPath -Force
        } catch {
            Write-Warning "Could not update $latestPath`: $($_.Exception.Message)"
        }
    }
}

Write-Host ''
Write-Host "Captured log: $logPath"
if (Test-Path -LiteralPath $latestPath) {
    Write-Host "Latest copy:  $latestPath"
}

exit $overallExitCode
