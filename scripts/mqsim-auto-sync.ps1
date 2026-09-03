[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$GitExecutable,
    [int64]$MaximumAutomaticFileSize = 50MB
)

$ErrorActionPreference = 'Stop'
$env:GIT_TERMINAL_PROMPT = '0'
$env:GCM_INTERACTIVE = 'Never'


if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent $PSScriptRoot
}

if ([string]::IsNullOrWhiteSpace($GitExecutable)) {
    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -eq $gitCommand) {
        $gitCandidates = @(
            (Join-Path $env:ProgramFiles 'Git\cmd\git.exe'),
            (Join-Path $env:LOCALAPPDATA 'Programs\Git\cmd\git.exe')
        )
        $GitExecutable = $gitCandidates | Where-Object {
            Test-Path -LiteralPath $_ -PathType Leaf
        } | Select-Object -First 1
    } else {
        $GitExecutable = $gitCommand.Source
    }
}

$resolvedGitExecutable = (Resolve-Path -LiteralPath $GitExecutable -ErrorAction Stop).Path
$repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$safeRepository = $repository.Replace('\', '/')
$gitDirectory = Join-Path $repository '.git'
$logPath = Join-Path $gitDirectory 'auto-sync.log'
$mutex = New-Object System.Threading.Mutex($false, 'Local\MQSimGitAutoSync')
$hasLock = $false

function Write-AutoSyncLog {
    param([string]$Message)

    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $env:COMPUTERNAME, $Message
    Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8
}

function Invoke-RepositoryGit {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [switch]$AllowFailure
    )

    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $resolvedGitExecutable -c "safe.directory=$safeRepository" -C $repository @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "git $($Arguments -join ' ') failed with exit code $exitCode`n$($output -join [Environment]::NewLine)"
    }

    [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

try {
    if (-not (Test-Path -LiteralPath $gitDirectory -PathType Container)) {
        throw "Not a Git repository: $repository"
    }

    $hasLock = $mutex.WaitOne(0)
    if (-not $hasLock) {
        exit 0
    }

    $branchResult = Invoke-RepositoryGit -Arguments @('rev-parse', '--abbrev-ref', 'HEAD')
    $branch = ($branchResult.Output | Select-Object -First 1).ToString().Trim()
    if ($branch -ne 'main') {
        Write-AutoSyncLog "Skipped: current branch is '$branch', not 'main'."
        exit 0
    }

    $conflictResult = Invoke-RepositoryGit -Arguments @('diff', '--name-only', '--diff-filter=U')
    if ($conflictResult.Output.Count -gt 0) {
        Write-AutoSyncLog "Skipped: unresolved conflicts: $($conflictResult.Output -join ', ')"
        exit 1
    }

    Invoke-RepositoryGit -Arguments @('add', '--all') | Out-Null

    $candidateResult = Invoke-RepositoryGit -Arguments @('diff', '--cached', '--name-only', '--diff-filter=ACMR')
    $candidatePaths = @($candidateResult.Output | ForEach-Object { $_.ToString().Trim() } | Where-Object { $_ })
    $blockedPaths = New-Object System.Collections.Generic.List[string]

    foreach ($relativePath in $candidatePaths) {
        $normalizedPath = $relativePath.Replace('\', '/')
        $isSensitive = $normalizedPath -match '(^|/)(\.env($|\.)|credentials\.|secrets\.)' -or
            $normalizedPath -match '\.(pem|key|pfx|p12)$'
        $fullPath = Join-Path $repository $relativePath
        $isOversized = (Test-Path -LiteralPath $fullPath -PathType Leaf) -and
            ((Get-Item -LiteralPath $fullPath).Length -gt $MaximumAutomaticFileSize)

        if ($isSensitive -or $isOversized) {
            Invoke-RepositoryGit -Arguments @('reset', '--', $relativePath) | Out-Null
            $blockedPaths.Add($relativePath)
        }
    }

    if ($blockedPaths.Count -gt 0) {
        Write-AutoSyncLog "Left uncommitted by safety policy: $($blockedPaths -join ', ')"
    }

    $stagedResult = Invoke-RepositoryGit -Arguments @('diff', '--cached', '--quiet') -AllowFailure
    if ($stagedResult.ExitCode -eq 1) {
        $message = 'auto-sync: {0} [{1}]' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $env:COMPUTERNAME
        Invoke-RepositoryGit -Arguments @('commit', '-m', $message) | Out-Null
        Write-AutoSyncLog "Committed local changes: $message"
    } elseif ($stagedResult.ExitCode -ne 0) {
        throw 'Unable to inspect staged changes.'
    }

    Invoke-RepositoryGit -Arguments @('fetch', '--prune', 'origin', 'main') | Out-Null

    $rebaseResult = Invoke-RepositoryGit -Arguments @('rebase', 'origin/main') -AllowFailure
    if ($rebaseResult.ExitCode -ne 0) {
        Invoke-RepositoryGit -Arguments @('rebase', '--abort') -AllowFailure | Out-Null
        Write-AutoSyncLog 'Stopped: remote changes conflict with local commits. Rebase was aborted; manual resolution is required.'
        exit 1
    }

    $aheadResult = Invoke-RepositoryGit -Arguments @('rev-list', '--count', 'origin/main..HEAD')
    $aheadCount = [int](($aheadResult.Output | Select-Object -First 1).ToString().Trim())
    if ($aheadCount -eq 0) {
        Write-AutoSyncLog 'Synchronization completed; no local commits required a push.'
        exit 0
    }

    $pushResult = Invoke-RepositoryGit -Arguments @('push', 'origin', 'main') -AllowFailure
    if ($pushResult.ExitCode -ne 0) {
        Write-AutoSyncLog "Push failed: $($pushResult.Output -join ' ')"
        exit 1
    }

    Write-AutoSyncLog 'Synchronization completed.'
    exit 0
} catch {
    if (Test-Path -LiteralPath $gitDirectory -PathType Container) {
        Write-AutoSyncLog "ERROR: $($_.Exception.Message)"
    }
    exit 1
} finally {
    if ($hasLock) {
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
}
